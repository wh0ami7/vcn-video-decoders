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
  bool invalidated;  // set by invalidate, once the queue is drained
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

// Driver sessions of this plug-in in this process, for the log: a host that
// keeps creating sessions without ending them shows up as a growing "live".
static long g_started, g_ended;

// Ends the decoder session, if there is one, and logs it with the counts. The
// caller holds s->lock or is the only one left using s.
static void end_session(Storage* s, const char* why) {
  if (!s->vcn) return;
  vcnav1_destroy(s->vcn);
  s->vcn = NULL;
  long ended = __atomic_add_fetch(&g_ended, 1, __ATOMIC_RELAXED),
       started = __atomic_load_n(&g_started, __ATOMIC_RELAXED);
  os_log_info(logger(),
              "session %dx%d ended (%{public}s): %ld started, %ld ended, %ld "
              "live in this process",
              s->width, s->height, why, started, ended, started - ended);
}

static Storage* storage_of(VTVideoDecoderRef dec) {
  return CMBaseObjectGetDerivedStorage(VTVideoDecoderGetCMBaseObject(dec));
}

// Marks the decode queue. The output callback runs on it, and a call made from
// inside the callback must not wait for the queue, which is busy running it.
static char kQueueKey;
static bool on_queue(Storage* s) {
  return dispatch_get_specific(&kQueueKey) == s;
}
// Waits until every queued frame has been emitted, also those queued from
// inside the callback while it waited. From inside the callback the frames
// queued after the running one cannot be waited for.
static void drain(Storage* s) {
  if (!s->queue || on_queue(s)) return;
  do
    dispatch_sync(s->queue, ^{
                  });
  while (__atomic_load_n(&s->depth, __ATOMIC_RELAXED) > 0);
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
  drain(s);
  pthread_mutex_lock(&s->lock);
  // Frames handed in from now on are refused, and none is emitted any more.
  __atomic_store_n(&s->invalidated, true, __ATOMIC_RELAXED);
  end_session(s, "invalidated");
  pthread_mutex_unlock(&s->lock);
  return noErr;
}

static void base_finalize(CMBaseObjectRef obj) {
  Storage* s = CMBaseObjectGetDerivedStorage(obj);
  drain(s);
  end_session(s, "finalized");
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
  if (!key || !out) return kVTParameterErr;
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
  if (!fmt) return kVTParameterErr;
  CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(fmt);
  if (dim.width <= 0 || dim.height <= 0)
    return kVTVideoDecoderUnsupportedDataFormatErr;
  // A second start replaces the decoder of the first one.
  drain(s);
  pthread_mutex_lock(&s->lock);
  end_session(s, "replaced by a new start");
  // Kept only once the new session runs: a start that fails must leave no
  // format that canAcceptFormatDescription could match.
  if (s->format) CFRelease(s->format);
  s->format = NULL;
  s->session = session;
  pthread_mutex_unlock(&s->lock);
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
  VcnAV1* vcn = vcnav1_create(maxW, maxH, err, sizeof(err));
  if (!vcn) {
    os_log_error(logger(), "vcnav1_create: %{public}s", err);
    if (av1C) CFRelease(av1C);
    return kVTVideoDecoderNotAvailableNowErr;
  }
  pthread_mutex_lock(&s->lock);
  s->vcn = vcn;
  __atomic_add_fetch(&g_started, 1, __ATOMIC_RELAXED);
  pthread_mutex_unlock(&s->lock);
  int cr =
      cfg && cfgLen > 4 ? vcnav1_configure(s->vcn, cfg + 4, cfgLen - 4) : 0;
  if (cr)
    os_log_error(logger(), "configOBUs rejected: %{public}s",
                 vcnav1_last_error(s->vcn));
  if (av1C) CFRelease(av1C);
  // A sequence header the engine cannot decode: VideoToolbox can pick another
  // decoder.
  if (cr == VCNAV1_UNSUPPORTED) {
    pthread_mutex_lock(&s->lock);
    end_session(s, "unsupported sequence header");
    pthread_mutex_unlock(&s->lock);
    return kVTVideoDecoderUnsupportedDataFormatErr;
  }

  CFDictionaryRef attrs =
      vcnav1_copy_pixel_buffer_attributes(s->width, s->height, s->tenBit);
  if (!attrs) {  // out of memory: no session rather than one with no attributes
    pthread_mutex_lock(&s->lock);
    end_session(s, "out of memory");
    pthread_mutex_unlock(&s->lock);
    return kVTAllocationFailedErr;
  }
  VTDecoderSessionSetPixelBufferAttributes(session, attrs);
  CFRelease(attrs);
  pthread_mutex_lock(&s->lock);
  s->format = (CMVideoFormatDescriptionRef)CFRetain(fmt);
  pthread_mutex_unlock(&s->lock);
  long started = __atomic_load_n(&g_started, __ATOMIC_RELAXED),
       ended = __atomic_load_n(&g_ended, __ATOMIC_RELAXED);
  os_log(logger(),
         "session %dx%d %{public}s: %ld started, %ld ended, %ld live in this "
         "process",
         s->width, s->height, s->tenBit ? "10-bit" : "8-bit", started, ended,
         started - ended);
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
           "over 16.7 ms %d, max depth %d, firmware errors %u",
           s->width, s->height, s->st_n, now_ms() - s->st_t0,
           s->st_dec_sum / s->st_n, s->st_dec_max, s->st_q_sum / s->st_n,
           s->st_q_max, s->st_over, s->st_depth_max,
           vcnav1_firmware_errors(s->vcn));
    s->st_n = s->st_over = s->st_depth_max = 0;
    s->st_dec_sum = s->st_dec_max = s->st_q_sum = s->st_q_max = 0;
  }
}

// A sample larger than one bitstream buffer of the driver pool (BS_SIZE in
// the session code) cannot be decoded: refuse it before copying anything.
enum { kMaxSampleBytes = 0x1800000 };

// What VideoToolbox is told when a sample fails. A broken session (a driver
// call failed, or the GPU did not finish a command) is a malfunction, so that
// the host can fall back to another decoder; bad data is kept for streams
// that cannot be decoded as they are.
static OSStatus decode_status(int failure) {
  switch (failure) {
    case VCNAV1_FAIL_DEVICE:
      return kVTVideoDecoderMalfunctionErr;
    case VCNAV1_FAIL_MEMORY:
      return kVTAllocationFailedErr;
    default:
      return kVTVideoDecoderBadDataErr;
  }
}

// Decode one temporal unit and hand its shown frame to VideoToolbox.
static void decode_and_emit(Storage* s, VTVideoDecoderFrame frame,
                            CMSampleBufferRef sbuf, double enqueued) {
  double started = now_ms();
  CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sbuf);
  size_t len = bb ? CMBlockBufferGetDataLength(bb) : 0;
  OSStatus status = noErr;
  uint8_t* copy = NULL;
  char* ptr = NULL;
  if (!len || len > kMaxSampleBytes)
    status = kVTVideoDecoderBadDataErr;
  else if (CMBlockBufferIsRangeContiguous(bb, 0, len) &&
           !CMBlockBufferGetDataPointer(bb, 0, NULL, NULL, &ptr) && ptr) {
  } else {
    copy = malloc(len);
    if (!copy)
      status = kVTAllocationFailedErr;
    else if (CMBlockBufferCopyDataBytes(bb, 0, len, copy))
      status = kVTVideoDecoderBadDataErr;
    ptr = (char*)copy;
  }
  pthread_mutex_lock(&s->lock);
  int depth = __atomic_load_n(&s->depth, __ATOMIC_RELAXED);
  if (depth > s->st_depth_max) s->st_depth_max = depth;
  bool invalidated = s->invalidated;
  if (!status && !s->vcn) status = kVTInvalidSessionErr;
  if (!status &&
      vcnav1_decode(s->vcn, (const uint8_t*)ptr, len, 0, on_output, s)) {
    os_log_error(logger(), "decode: %{public}s", vcnav1_last_error(s->vcn));
    status = decode_status(vcnav1_failure(s->vcn));
  }
  CVPixelBufferRef out = s->lastFrame;
  s->lastFrame = NULL;
  stats_add(s, started - enqueued, now_ms() - started);
  pthread_mutex_unlock(&s->lock);
  free(copy);
  // A sample that failed hands back no picture, even one it showed before. A
  // temporal unit that shows no frame is emitted as dropped, with no error, as
  // the VP9 plug-in does with a chunk of hidden frames.
  if (status && out) {
    CVPixelBufferRelease(out);
    out = NULL;
  }
  if (!invalidated)
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
  if (!sbuf) return kVTParameterErr;
  if (__atomic_load_n(&s->invalidated, __ATOMIC_RELAXED))
    return kVTInvalidSessionErr;
  if (!(flags & kVTDecodeFrame_EnableAsynchronousDecompression)) {
    double t = now_ms();
    // From inside the output callback the queue is already running this code.
    if (on_queue(s))
      decode_and_emit(s, frame, sbuf, t);
    else
      dispatch_sync(s->queue, ^{
        decode_and_emit(s, frame, sbuf, t);
      });  // keeps order with queued work
    return noErr;
  }
  // From inside the output callback the running frame still holds its slot,
  // so waiting for one could wait for ever: queue the frame without one.
  bool counted = !on_queue(s);
  if (counted) dispatch_semaphore_wait(s->slots, DISPATCH_TIME_FOREVER);
  CFRetain(sbuf);
  double t = now_ms();
  __atomic_add_fetch(&s->depth, 1, __ATOMIC_RELAXED);
  dispatch_async(s->queue, ^{
    decode_and_emit(s, frame, sbuf, t);
    __atomic_sub_fetch(&s->depth, 1, __ATOMIC_RELAXED);
    CFRelease(sbuf);
    if (counted) dispatch_semaphore_signal(s->slots);
  });
  if (infoOut) *infoOut = kVTDecodeInfo_Asynchronous;
  return noErr;
}

static OSStatus dec_copy_supported_properties(VTVideoDecoderRef dec,
                                              CFDictionaryRef* out) {
  if (!out) return kVTParameterErr;
  *out = NULL;
  const void* k[] = {kVTPropertyTypeKey, kVTPropertyReadWriteStatusKey};
  const void* v[] = {kVTPropertyType_Boolean,
                     kVTPropertyReadWriteStatus_ReadOnly};
  CFDictionaryRef hw =
      CFDictionaryCreate(NULL, k, v, 2, &kCFTypeDictionaryKeyCallBacks,
                         &kCFTypeDictionaryValueCallBacks);
  if (!hw) return kVTAllocationFailedErr;
  const void* keys[] = {
      kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder};
  const void* vals[] = {hw};
  *out = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks,
                            &kCFTypeDictionaryValueCallBacks);
  CFRelease(hw);
  return *out ? noErr : kVTAllocationFailedErr;
}

static Boolean dec_can_accept_format(VTVideoDecoderRef dec,
                                     CMFormatDescriptionRef fmt) {
  Storage* s = storage_of(dec);
  if (!fmt) return false;
  // A decoder whose start failed, whose session broke down to nothing or that
  // was invalidated takes no format: VideoToolbox must make a new one.
  pthread_mutex_lock(&s->lock);
  CMFormatDescriptionRef cur =
      s->vcn && !s->invalidated && s->format ? CFRetain(s->format) : NULL;
  pthread_mutex_unlock(&s->lock);
  if (!cur) return false;
  Boolean same = false;
  CMVideoDimensions a = CMVideoFormatDescriptionGetDimensions(cur),
                    b = CMVideoFormatDescriptionGetDimensions(fmt);
  if (CMFormatDescriptionGetMediaSubType(fmt) ==
          CMFormatDescriptionGetMediaSubType(cur) &&
      a.width == b.width && a.height == b.height) {
    CFDataRef x = copy_av1C(cur), y = copy_av1C(fmt);
    same = (!x && !y) || (x && y && CFEqual(x, y));
    if (x) CFRelease(x);
    if (y) CFRelease(y);
  }
  CFRelease(cur);
  return same;
}

static OSStatus dec_finish_delayed_frames(VTVideoDecoderRef dec) {
  Storage* s = storage_of(dec);
  drain(s);  // every queued frame has been emitted when this returns
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
  if (!out) return kVTParameterErr;
  *out = NULL;
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
  if (!s->queue || !s->slots) {
    CFRelease(obj);
    return kVTAllocationFailedErr;
  }
  dispatch_queue_set_specific(s->queue, &kQueueKey, s, NULL);
  *out = (VTVideoDecoderRef)obj;
  return noErr;
}
