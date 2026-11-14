// VideoToolbox decoder plugin for 'av01' on AMD VCN (macOS x86_64), built on
// vcnav1.
//
// ABI recovered from Apple's own plugins (AppleGVAVPXDecoder, AV1DecoderSW):
//   factory  OSStatus f(FourCharCode codec, CFAllocatorRef, VTVideoDecoderRef
//   *out) object   CMDerivedObjectCreate(alloc, &{0, &CMBaseClass,
//   &VTVideoDecoderClass}, VTVideoDecoderGetClassID(), &obj) CMBaseClass
//   (packed 4)  version 1, derivedStorageSize, equal, invalidate, finalize,
//                                   copyDebugDescription, copyProperty,
//                                   setProperty, notificationBarrier, handlers
//   VTVideoDecoderClass (packed 4)  version 3, startSession, decodeFrame,
//   copySupportedPropertyDictionary,
//                                   setProperties, copySerializableProperties,
//                                   canAcceptFormatDescription,
//                                   finishDelayedFrames, (tile-decoder entries)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../vcnav1/vcnav1.h"

// ---- private CoreMedia / VideoToolbox plugin SPI
// -------------------------------------------
typedef const void* CMBaseClassID;
typedef struct OpaqueCMBaseObject* CMBaseObjectRef;
typedef struct OpaqueVTVideoDecoder* VTVideoDecoderRef;
typedef struct OpaqueVTVideoDecoderSession* VTVideoDecoderSession;
typedef struct OpaqueVTVideoDecoderFrame* VTVideoDecoderFrame;

#pragma pack(push, 4)
typedef struct {
  uint32_t version;
  size_t derivedStorageSize;
  Boolean (*equal)(CMBaseObjectRef, CMBaseObjectRef);
  OSStatus (*invalidate)(CMBaseObjectRef);
  void (*finalize)(CMBaseObjectRef);
  CFStringRef (*copyDebugDescription)(CMBaseObjectRef);
  OSStatus (*copyProperty)(CMBaseObjectRef, CFStringRef, CFAllocatorRef, void*);
  OSStatus (*setProperty)(CMBaseObjectRef, CFStringRef, CFTypeRef);
  OSStatus (*notificationBarrier)(CMBaseObjectRef);
  const void* propertyHandlers;
} CMBaseClass;

typedef struct {
  uint32_t version;
  OSStatus (*startSession)(VTVideoDecoderRef, VTVideoDecoderSession,
                           CMVideoFormatDescriptionRef);
  OSStatus (*decodeFrame)(VTVideoDecoderRef, VTVideoDecoderFrame,
                          CMSampleBufferRef, VTDecodeFrameFlags,
                          VTDecodeInfoFlags*);
  OSStatus (*copySupportedPropertyDictionary)(VTVideoDecoderRef,
                                              CFDictionaryRef*);
  OSStatus (*setProperties)(VTVideoDecoderRef, CFDictionaryRef);
  OSStatus (*copySerializableProperties)(VTVideoDecoderRef, CFAllocatorRef,
                                         CFDictionaryRef*);
  Boolean (*canAcceptFormatDescription)(VTVideoDecoderRef,
                                        CMFormatDescriptionRef);
  OSStatus (*finishDelayedFrames)(VTVideoDecoderRef);
  void* reserved[6];
} VTVideoDecoderClass;
#pragma pack(pop)

typedef struct {
  void* reserved;
  const CMBaseClass* baseClass;
  const VTVideoDecoderClass* decoderClass;
} VTVideoDecoderVTable;

extern OSStatus CMDerivedObjectCreate(CFAllocatorRef, const void* vtable,
                                      CMBaseClassID, CMBaseObjectRef*);
extern void* CMBaseObjectGetDerivedStorage(CMBaseObjectRef);
extern CMBaseClassID VTVideoDecoderGetClassID(void);
extern CMBaseObjectRef VTVideoDecoderGetCMBaseObject(VTVideoDecoderRef);
extern OSStatus VTDecoderSessionSetPixelBufferAttributes(VTVideoDecoderSession,
                                                         CFDictionaryRef);
extern OSStatus VTDecoderSessionEmitDecodedFrame(VTVideoDecoderSession,
                                                 VTVideoDecoderFrame, OSStatus,
                                                 VTDecodeInfoFlags,
                                                 CVImageBufferRef);

// ---- decoder instance
// ----------------------------------------------------------------------
typedef struct {
  VTVideoDecoderSession session;
  CMVideoFormatDescriptionRef format;
  VcnAV1* vcn;
  int32_t width, height;
  bool tenBit;
  bool emitted;
  CVPixelBufferRef
      lastFrame;  // retained output of the temporal unit being decoded
  pthread_mutex_t lock;
  dispatch_queue_t
      queue;  // serial: frames are decoded (and emitted) in submission order
  dispatch_semaphore_t slots;  // bounds frames queued but not yet decoded
  // stats, logged every STATS_EVERY frames: decode = vcnav1_decode time, queue
  // = enqueue -> start
  int st_n, st_over, st_depth_max, depth;
  double st_dec_sum, st_dec_max, st_q_sum, st_q_max, st_t0;
} Storage;

#define STATS_EVERY 120

// Frames that may wait in the queue. The VCN still runs one command at a time
// (like AppleGVA); the queue only lets the caller keep feeding samples while
// the previous one decodes.
#define MAX_QUEUED 4

static os_log_t logger(void) {
  static os_log_t l;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    l = os_log_create("com.droga.vtav1vcn", "decoder");
  });
  return l;
}

static Storage* storage_of(VTVideoDecoderRef dec) {
  return CMBaseObjectGetDerivedStorage(VTVideoDecoderGetCMBaseObject(dec));
}

static CFDataRef copy_av1C(CMFormatDescriptionRef fmt) {
  CFDictionaryRef atoms = CMFormatDescriptionGetExtension(
      fmt, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
  if (!atoms || CFGetTypeID(atoms) != CFDictionaryGetTypeID()) return NULL;
  CFTypeRef v = CFDictionaryGetValue(atoms, CFSTR("av1C"));
  if (!v) return NULL;
  if (CFGetTypeID(v) == CFDataGetTypeID()) return CFRetain(v);
  if (CFGetTypeID(v) == CFArrayGetTypeID() && CFArrayGetCount(v) > 0) {
    CFTypeRef e = CFArrayGetValueAtIndex(v, 0);
    if (CFGetTypeID(e) == CFDataGetTypeID()) return CFRetain(e);
  }
  return NULL;
}

// ---- CMBaseClass
// ---------------------------------------------------------------------------
static OSStatus base_invalidate(CMBaseObjectRef obj) {
  Storage* s = CMBaseObjectGetDerivedStorage(obj);
  if (s->queue)
    dispatch_sync(s->queue, ^{
                  });
  pthread_mutex_lock(&s->lock);
  if (s->vcn) {
    vcnav1_destroy(s->vcn);
    s->vcn = NULL;
  }
  pthread_mutex_unlock(&s->lock);
  return noErr;
}

static void base_finalize(CMBaseObjectRef obj) {
  Storage* s = CMBaseObjectGetDerivedStorage(obj);
  if (s->queue)
    dispatch_sync(s->queue, ^{
                  });
  if (s->vcn) vcnav1_destroy(s->vcn);
  if (s->format) CFRelease(s->format);
  if (s->queue) dispatch_release(s->queue);
  if (s->slots) dispatch_release(s->slots);
  pthread_mutex_destroy(&s->lock);
}

static CFStringRef base_copy_debug_description(CMBaseObjectRef obj) {
  return CFStringCreateWithFormat(NULL, NULL, CFSTR("<AV1 VCN Decoder %p>"),
                                  obj);
}

static OSStatus base_copy_property(CMBaseObjectRef obj, CFStringRef key,
                                   CFAllocatorRef alloc, void* out) {
  if (CFEqual(
          key,
          kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder)) {
    *(CFTypeRef*)out = CFRetain(kCFBooleanTrue);
    return noErr;
  }
  return kVTPropertyNotSupportedErr;
}

static OSStatus base_set_property(CMBaseObjectRef obj, CFStringRef key,
                                  CFTypeRef value) {
  return kVTPropertyNotSupportedErr;
}

// ---- VTVideoDecoderClass
// -------------------------------------------------------------------
static OSStatus dec_start_session(VTVideoDecoderRef dec,
                                  VTVideoDecoderSession session,
                                  CMVideoFormatDescriptionRef fmt) {
  Storage* s = storage_of(dec);
  s->session = session;
  if (s->format) CFRelease(s->format);
  s->format = (CMVideoFormatDescriptionRef)CFRetain(fmt);
  CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(fmt);
  s->width = dim.width;
  s->height = dim.height;
  s->tenBit = false;

  CFDataRef av1C = copy_av1C(fmt);
  const uint8_t* cfg = av1C ? CFDataGetBytePtr(av1C) : NULL;
  CFIndex cfgLen = av1C ? CFDataGetLength(av1C) : 0;
  if (cfg && cfgLen >= 4) {
    // av1C: marker|version, profile|level,
    // tier|high_bitdepth|twelve_bit|mono|ss_x|ss_y|csp, delay
    int profile = cfg[1] >> 5, high = (cfg[2] >> 6) & 1,
        twelve = (cfg[2] >> 5) & 1, mono = (cfg[2] >> 4) & 1;
    int ssx = (cfg[2] >> 3) & 1, ssy = (cfg[2] >> 2) & 1;
    if (profile != 0 || twelve || mono || !ssx || !ssy) {
      os_log_error(logger(),
                   "unsupported av1C profile %d twelve %d mono %d ss %d%d",
                   profile, twelve, mono, ssx, ssy);
      CFRelease(av1C);
      return kVTVideoDecoderUnsupportedDataFormatErr;
    }
    s->tenBit = high;
  }
  int32_t maxW = s->width > 4096 ? s->width : 4096,
          maxH = s->height > 2304 ? s->height : 2304;
  if (s->width > 8192 || s->height > 4352) {
    if (av1C) CFRelease(av1C);
    return kVTVideoDecoderUnsupportedDataFormatErr;
  }
  char err[256] = "";
  vcnav1_gpu_armed = 1;
  s->vcn = vcnav1_create(maxW, maxH, err, sizeof(err));
  if (!s->vcn) {
    os_log_error(logger(), "vcnav1_create: %{public}s", err);
    if (av1C) CFRelease(av1C);
    return kVTVideoDecoderNotAvailableNowErr;
  }
  if (cfg && cfgLen > 4 && vcnav1_configure(s->vcn, cfg + 4, cfgLen - 4))
    os_log_error(logger(), "configOBUs rejected: %{public}s",
                 vcnav1_last_error(s->vcn));
  if (av1C) CFRelease(av1C);

  CFDictionaryRef attrs =
      vcnav1_copy_pixel_buffer_attributes(s->width, s->height, s->tenBit);
  VTDecoderSessionSetPixelBufferAttributes(session, attrs);
  CFRelease(attrs);
  os_log(logger(), "session %dx%d %{public}s", s->width, s->height,
         s->tenBit ? "10-bit" : "8-bit");
  return noErr;
}

static void on_output(void* ctx, CVPixelBufferRef frame, const AV1Parser* p,
                      int64_t tag) {
  Storage* s = ctx;
  // A temporal unit has exactly one shown frame; keep the last one if a stream
  // breaks that rule.
  if (s->lastFrame) CVPixelBufferRelease(s->lastFrame);
  s->lastFrame = CVPixelBufferRetain(frame);
}

static double now_ms(void) {
  static mach_timebase_info_data_t tb;
  if (!tb.denom) mach_timebase_info(&tb);
  return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

static void stats_add(Storage* s, double queued, double decode) {
  if (!s->st_n) s->st_t0 = now_ms();
  s->st_n++;
  s->st_dec_sum += decode;
  if (decode > s->st_dec_max) s->st_dec_max = decode;
  s->st_q_sum += queued;
  if (queued > s->st_q_max) s->st_q_max = queued;
  if (queued + decode > 16.7) s->st_over++;
  if (s->st_n >= STATS_EVERY) {
    os_log(logger(),
           "stats %dx%d: %d frames in %.0f ms, decode avg %.2f max %.2f ms, "
           "queue avg %.2f max %.2f ms, "
           "over 16.7 ms %d, max depth %d",
           s->width, s->height, s->st_n, now_ms() - s->st_t0,
           s->st_dec_sum / s->st_n, s->st_dec_max, s->st_q_sum / s->st_n,
           s->st_q_max, s->st_over, s->st_depth_max);
    s->st_n = s->st_over = s->st_depth_max = 0;
    s->st_dec_sum = s->st_dec_max = s->st_q_sum = s->st_q_max = 0;
  }
}

// Decode one temporal unit and hand the shown frame to VideoToolbox.
static void decode_and_emit(Storage* s, VTVideoDecoderFrame frame,
                            CMSampleBufferRef sbuf, double enqueued) {
  double started = now_ms();
  CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sbuf);
  size_t len = bb ? CMBlockBufferGetDataLength(bb) : 0;
  OSStatus status = noErr;
  uint8_t* copy = NULL;
  char* ptr = NULL;
  if (!len)
    status = kVTVideoDecoderBadDataErr;
  else if (CMBlockBufferIsRangeContiguous(bb, 0, len) &&
           !CMBlockBufferGetDataPointer(bb, 0, NULL, NULL, &ptr) && ptr) {
  } else if (len > 0x1800000u) {
    status = kVTVideoDecoderBadDataErr;
  } else {
    copy = malloc(len);
    if (!copy || CMBlockBufferCopyDataBytes(bb, 0, len, copy))
      status = kVTVideoDecoderBadDataErr;
    ptr = (char*)copy;
  }
  pthread_mutex_lock(&s->lock);
  if (!status && !s->vcn) status = kVTInvalidSessionErr;
  if (!status &&
      vcnav1_decode(s->vcn, (const uint8_t*)ptr, len, 0, on_output, s)) {
    os_log_error(logger(), "decode: %{public}s", vcnav1_last_error(s->vcn));
    status = kVTVideoDecoderBadDataErr;
  }
  CVPixelBufferRef out = s->lastFrame;
  s->lastFrame = NULL;
  stats_add(s, started - enqueued, now_ms() - started);
  pthread_mutex_unlock(&s->lock);
  free(copy);
  if (!status && !out) status = kVTVideoDecoderBadDataErr;
  VTDecoderSessionEmitDecodedFrame(s->session, frame, status,
                                   out ? 0 : kVTDecodeInfo_FrameDropped, out);
  if (out) CVPixelBufferRelease(out);
}

static OSStatus dec_decode_frame(VTVideoDecoderRef dec,
                                 VTVideoDecoderFrame frame,
                                 CMSampleBufferRef sbuf,
                                 VTDecodeFrameFlags flags,
                                 VTDecodeInfoFlags* infoOut) {
  Storage* s = storage_of(dec);
  if (infoOut) *infoOut = 0;
  if (!(flags & kVTDecodeFrame_EnableAsynchronousDecompression)) {
    double t = now_ms();
    dispatch_sync(s->queue, ^{
      decode_and_emit(s, frame, sbuf, t);
    });  // keeps order with queued work
    return noErr;
  }
  dispatch_semaphore_wait(s->slots, DISPATCH_TIME_FOREVER);
  CFRetain(sbuf);
  double t = now_ms();
  int depth = __atomic_add_fetch(&s->depth, 1, __ATOMIC_RELAXED);
  if (depth > s->st_depth_max) s->st_depth_max = depth;
  dispatch_async(s->queue, ^{
    decode_and_emit(s, frame, sbuf, t);
    __atomic_sub_fetch(&s->depth, 1, __ATOMIC_RELAXED);
    CFRelease(sbuf);
    dispatch_semaphore_signal(s->slots);
  });
  if (infoOut) *infoOut = kVTDecodeInfo_Asynchronous;
  return noErr;
}

static OSStatus dec_copy_supported_properties(VTVideoDecoderRef dec,
                                              CFDictionaryRef* out) {
  const void* k[] = {kVTPropertyTypeKey, kVTPropertyReadWriteStatusKey};
  const void* v[] = {kVTPropertyType_Boolean,
                     kVTPropertyReadWriteStatus_ReadOnly};
  CFDictionaryRef hw =
      CFDictionaryCreate(NULL, k, v, 2, &kCFTypeDictionaryKeyCallBacks,
                         &kCFTypeDictionaryValueCallBacks);
  const void* keys[] = {
      kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder};
  const void* vals[] = {hw};
  *out = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks,
                            &kCFTypeDictionaryValueCallBacks);
  CFRelease(hw);
  return noErr;
}

static Boolean dec_can_accept_format(VTVideoDecoderRef dec,
                                     CMFormatDescriptionRef fmt) {
  Storage* s = storage_of(dec);
  if (!s->format) return false;
  CMVideoDimensions a = CMVideoFormatDescriptionGetDimensions(s->format),
                    b = CMVideoFormatDescriptionGetDimensions(fmt);
  if (a.width != b.width || a.height != b.height) return false;
  CFDataRef x = copy_av1C(s->format), y = copy_av1C(fmt);
  Boolean same = (!x && !y) || (x && y && CFEqual(x, y));
  if (x) CFRelease(x);
  if (y) CFRelease(y);
  return same;
}

static OSStatus dec_finish_delayed_frames(VTVideoDecoderRef dec) {
  Storage* s = storage_of(dec);
  dispatch_sync(s->queue, ^{
                });  // every queued frame has been emitted when this returns
  return noErr;
}

static const CMBaseClass kBaseClass = {
    .version = 1,
    .derivedStorageSize = sizeof(Storage),
    .invalidate = base_invalidate,
    .finalize = base_finalize,
    .copyDebugDescription = base_copy_debug_description,
    .copyProperty = base_copy_property,
    .setProperty = base_set_property,
};

static const VTVideoDecoderClass kDecoderClass = {
    .version = 3,
    .startSession = dec_start_session,
    .decodeFrame = dec_decode_frame,
    .copySupportedPropertyDictionary = dec_copy_supported_properties,
    .canAcceptFormatDescription = dec_can_accept_format,
    .finishDelayedFrames = dec_finish_delayed_frames,
};

static const VTVideoDecoderVTable kVTable = {NULL, &kBaseClass, &kDecoderClass};

__attribute__((visibility("default"))) OSStatus AV1VCN_VTDecoder_CreateInstance(
    FourCharCode codec, CFAllocatorRef alloc, VTVideoDecoderRef* out) {
  if (codec != kCMVideoCodecType_AV1) return kVTCouldNotFindVideoDecoderErr;
  CMBaseObjectRef obj = NULL;
  OSStatus r =
      CMDerivedObjectCreate(alloc, &kVTable, VTVideoDecoderGetClassID(), &obj);
  if (r || !obj) return r ? r : kVTAllocationFailedErr;
  Storage* s = CMBaseObjectGetDerivedStorage(obj);
  memset(s, 0, sizeof(*s));
  pthread_mutex_init(&s->lock, NULL);
  s->queue = dispatch_queue_create(
      "com.droga.vtav1vcn.decode",
      dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
                                              QOS_CLASS_USER_INTERACTIVE, 0));
  s->slots = dispatch_semaphore_create(MAX_QUEUED);
  *out = (VTVideoDecoderRef)obj;
  return noErr;
}
