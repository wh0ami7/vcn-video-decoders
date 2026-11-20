// plugintest: the VideoToolbox side of a decoder plug-in, with the decoder
// session replaced by a stand-in (nothing opens the GPU or the driver). Built
// once for each plug-in, with -DTEST_AV1 or -DTEST_VP9. It checks:
//
//   1. the factory refuses a NULL result pointer;
//   2. a second session start replaces the decoder of the first;
//   3. a format of another codec is not accepted;
//   4. a shown frame, a sample with no shown frame (dropped, no error) and a
//      sample that fails after showing a frame (an error, no picture);
//   5. calls made from inside the output callback (finish delayed frames, a
//      synchronous decode, more asynchronous decodes than the queue holds) do
//      not trap or hang;
//   6. a NULL sample is refused;
//   7. after invalidate, frames are refused and nothing is emitted;
//   8. (AV1) a sequence header in av1C that the engine cannot decode refuses
//      the session;
//   9. a sample the decoder session fails as a device failure (a stuck GPU,
//      a driver call that failed) is a malfunction, not bad data, and the
//      format is not accepted after it; one that runs out of memory is
//      kVTAllocationFailedErr;
//  10. after a start that fails, no format is accepted;
//  11. canAcceptFormatDescription does not wait for a decode in progress;
//  12. a start that invalidate overtakes closes its new session again.
//
// usage: plugintest   (exit status 0 = pass)
#include <dispatch/dispatch.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#if defined(TEST_AV1)
#include "../vtav1/AV1VCNDecoder.c"
#define CODEC kCMVideoCodecType_AV1
#define CREATE AV1VCN_VTDecoder_CreateInstance
typedef VcnAV1 Vcn;
typedef VcnAV1OutputFn OutputFn;
#elif defined(TEST_VP9)
#include "../vtvp9/VP9VCNDecoder.c"
#define CODEC kCMVideoCodecType_VP9
#define CREATE VP9VCN_VTDecoder_CreateInstance
typedef VcnVP9 Vcn;
typedef VcnVP9OutputFn OutputFn;
#else
#error "build with -DTEST_AV1 or -DTEST_VP9"
#endif

static int failures;
static void expect(const char* what, bool ok) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) failures++;
}

// ---- stand-ins for the CoreMedia / VideoToolbox plug-in SPI
// -------------------
OSStatus CMDerivedObjectCreate(CFAllocatorRef alloc, const void* vtable,
                               CMBaseClassID id, CMBaseObjectRef* out) {
  (void)alloc;
  (void)vtable;
  (void)id;
  *out = calloc(1, sizeof(Storage));
  return *out ? noErr : kVTAllocationFailedErr;
}
void* CMBaseObjectGetDerivedStorage(CMBaseObjectRef obj) { return obj; }
CMBaseClassID VTVideoDecoderGetClassID(void) { return NULL; }
CMBaseObjectRef VTVideoDecoderGetCMBaseObject(VTVideoDecoderRef dec) {
  return (CMBaseObjectRef)dec;
}
OSStatus VTDecoderSessionSetPixelBufferAttributes(VTVideoDecoderSession s,
                                                  CFDictionaryRef a) {
  (void)s;
  (void)a;
  return noErr;
}

static int emits, emitErrors, emitImages, emitDropped;
static OSStatus lastStatus;
static void (*inCallback)(void);
OSStatus VTDecoderSessionEmitDecodedFrame(VTVideoDecoderSession session,
                                          VTVideoDecoderFrame frame,
                                          OSStatus status,
                                          VTDecodeInfoFlags flags,
                                          CVImageBufferRef image) {
  (void)session;
  (void)frame;
  __atomic_add_fetch(&emits, 1, __ATOMIC_RELAXED);
  if (status) __atomic_add_fetch(&emitErrors, 1, __ATOMIC_RELAXED);
  if (image) __atomic_add_fetch(&emitImages, 1, __ATOMIC_RELAXED);
  if (flags & kVTDecodeInfo_FrameDropped)
    __atomic_add_fetch(&emitDropped, 1, __ATOMIC_RELAXED);
  lastStatus = status;
  void (*hook)(void) = inCallback;
  inCallback = NULL;
  if (hook) hook();
  return noErr;
}

// ---- stand-in decoder session
// ------------------------------------------------- A sample's first byte says
// what it does: 0 shows nothing, 1 shows a frame, 2 shows a frame and then
// fails as a bad stream, 3 fails as a broken device, 4 runs out of memory, 5
// shows a frame after waiting (at most 3 s) for the test to let it go.
struct Fake {
  int unused;
};
static int creates, destroys;
static bool failCreate;  // the next session start fails
static int lastFailure;
// Invalidated by the next session start while it makes its session.
static VTVideoDecoderRef invalidateInCreate;
// Sample 5: says it is decoding, then waits to be let go.
static dispatch_semaphore_t slowEntered, slowRelease;
static bool slowTimedOut;
static void slow_decode(void) {
  dispatch_semaphore_signal(slowEntered);
  if (dispatch_semaphore_wait(
          slowRelease, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)))
    slowTimedOut = true;
}
static int fake_failure(uint8_t what);
#if defined(TEST_AV1)
static int configureResult;
#endif
static CVPixelBufferRef picture;

static Vcn* fake_create(void) {
  if (failCreate) return NULL;
  if (invalidateInCreate) base_invalidate((CMBaseObjectRef)invalidateInCreate);
  creates++;
  return (Vcn*)calloc(1, sizeof(struct Fake));
}
static void fake_destroy(Vcn* d) {
  destroys++;
  free(d);
}

#if defined(TEST_AV1)
int vcnav1_gpu_armed;
VcnAV1* vcnav1_create(uint32_t w, uint32_t h, char* err, size_t n) {
  (void)w, (void)h, (void)err, (void)n;
  return fake_create();
}
void vcnav1_destroy(VcnAV1* d) { fake_destroy(d); }
int vcnav1_configure(VcnAV1* d, const uint8_t* o, size_t n) {
  (void)d, (void)o, (void)n;
  return configureResult;
}
const char* vcnav1_last_error(VcnAV1* d) {
  (void)d;
  return "stand-in";
}
unsigned vcnav1_firmware_errors(VcnAV1* d) {
  (void)d;
  return 0;
}
CFDictionaryRef vcnav1_copy_pixel_buffer_attributes(uint32_t w, uint32_t h,
                                                    bool t) {
  (void)w, (void)h, (void)t;
  return CFDictionaryCreate(NULL, NULL, NULL, 0, NULL, NULL);
}
int vcnav1_decode(VcnAV1* d, const uint8_t* tu, size_t size, int64_t tag,
                  VcnAV1OutputFn fn, void* ctx) {
  (void)d, (void)size;
  if (tu[0] == 5) slow_decode();
  if (tu[0] == 1 || tu[0] == 2 || tu[0] == 5) fn(ctx, picture, NULL, tag);
  lastFailure = fake_failure(tu[0]);
  return tu[0] >= 2 && tu[0] <= 4 ? -1 : 0;
}
int vcnav1_failure(VcnAV1* d) {
  (void)d;
  return lastFailure;
}
#else
int vcnvp9_gpu_armed;
VcnVP9* vcnvp9_create(uint32_t w, uint32_t h, char* err, size_t n) {
  (void)w, (void)h, (void)err, (void)n;
  return fake_create();
}
void vcnvp9_destroy(VcnVP9* d) { fake_destroy(d); }
const char* vcnvp9_last_error(VcnVP9* d) {
  (void)d;
  return "stand-in";
}
unsigned vcnvp9_firmware_errors(VcnVP9* d) {
  (void)d;
  return 0;
}
CFDictionaryRef vcnvp9_copy_pixel_buffer_attributes(uint32_t w, uint32_t h,
                                                    bool t) {
  (void)w, (void)h, (void)t;
  return CFDictionaryCreate(NULL, NULL, NULL, 0, NULL, NULL);
}
int vcnvp9_decode(VcnVP9* d, const uint8_t* chunk, size_t size, int64_t tag,
                  VcnVP9OutputFn fn, void* ctx) {
  (void)d, (void)size;
  if (chunk[0] == 5) slow_decode();
  if (chunk[0] == 1 || chunk[0] == 2 || chunk[0] == 5)
    fn(ctx, picture, NULL, 16, 16, tag);
  lastFailure = fake_failure(chunk[0]);
  return chunk[0] >= 2 && chunk[0] <= 4 ? -1 : 0;
}
int vcnvp9_failure(VcnVP9* d) {
  (void)d;
  return lastFailure;
}
#endif

#if defined(TEST_AV1)
static int fake_failure(uint8_t what) {
  return what == 3   ? VCNAV1_FAIL_DEVICE
         : what == 4 ? VCNAV1_FAIL_MEMORY
                     : VCNAV1_FAIL_STREAM;
}
#else
static int fake_failure(uint8_t what) {
  return what == 3   ? VCNVP9_FAIL_DEVICE
         : what == 4 ? VCNVP9_FAIL_MEMORY
                     : VCNVP9_FAIL_STREAM;
}
#endif

// ---- helpers
// ------------------------------------------------------------------
static CMSampleBufferRef sample(uint8_t what) {
  static uint8_t bytes[6][4] = {{0, 1, 2, 3}, {1, 1, 2, 3}, {2, 1, 2, 3},
                                {3, 1, 2, 3}, {4, 1, 2, 3}, {5, 1, 2, 3}};
  CMBlockBufferRef bb = NULL;
  if (CMBlockBufferCreateWithMemoryBlock(NULL, bytes[what], 4, kCFAllocatorNull,
                                         NULL, 0, 4, 0, &bb))
    return NULL;
  CMSampleBufferRef sb = NULL;
  size_t sz = 4;
  CMSampleBufferCreate(NULL, bb, true, NULL, NULL, NULL, 1, 0, NULL, 1, &sz,
                       &sb);
  CFRelease(bb);
  return sb;
}

static CMVideoFormatDescriptionRef format(CMVideoCodecType codec,
                                          const uint8_t* av1C, size_t len) {
  CFDictionaryRef ext = NULL;
  if (av1C) {
    CFDataRef data = CFDataCreate(NULL, av1C, (CFIndex)len);
    const void* k1[] = {CFSTR("av1C")};
    const void* v1[] = {data};
    CFDictionaryRef atoms =
        CFDictionaryCreate(NULL, k1, v1, 1, &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);
    const void* k2[] = {
        kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms};
    const void* v2[] = {atoms};
    ext = CFDictionaryCreate(NULL, k2, v2, 1, &kCFTypeDictionaryKeyCallBacks,
                             &kCFTypeDictionaryValueCallBacks);
    CFRelease(atoms);
    CFRelease(data);
  }
  CMVideoFormatDescriptionRef f = NULL;
  CMVideoFormatDescriptionCreate(NULL, codec, 64, 64, ext, &f);
  if (ext) CFRelease(ext);
  return f;
}

static VTVideoDecoderRef dec;
static int reentered;
static void reenter(void) {
  // All of these run on the decode queue, from inside the output callback.
  dec_finish_delayed_frames(dec);
  CMSampleBufferRef sb = sample(1);
  dec_decode_frame(dec, NULL, sb, 0, NULL);
  for (int i = 0; i < MAX_QUEUED + 2; i++)
    dec_decode_frame(dec, NULL, sb,
                     kVTDecodeFrame_EnableAsynchronousDecompression, NULL);
  CFRelease(sb);
  reentered = 1;
}

static void hung(int sig) {
  (void)sig;
  static const char msg[] = "FAIL a call from inside the callback hung\n";
  write(1, msg, sizeof(msg) - 1);
  _exit(1);
}

int main(void) {
  int d0;
  signal(SIGALRM, hung);
  alarm(30);
  CVPixelBufferCreate(NULL, 16, 16,
                      kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, NULL,
                      &picture);

  expect("the factory refuses a NULL result pointer",
         CREATE(CODEC, NULL, NULL) == kVTParameterErr);
  expect("the factory makes a decoder", !CREATE(CODEC, NULL, &dec) && dec);
  CMBaseObjectRef obj = (CMBaseObjectRef)dec;

  CMVideoFormatDescriptionRef fmt = format(CODEC, NULL, 0);
  expect("a session starts",
         !dec_start_session(dec, NULL, fmt) && creates == 1);
  expect("a second start replaces the first decoder",
         !dec_start_session(dec, NULL, fmt) && creates == 2 && destroys == 1);
  expect("the same format is accepted", dec_can_accept_format(dec, fmt));
  CMVideoFormatDescriptionRef h264 = format(kCMVideoCodecType_H264, NULL, 0);
  expect("an H.264 format of the same size is not",
         !dec_can_accept_format(dec, h264));
  CFRelease(h264);

  CMSampleBufferRef shown = sample(1), hidden = sample(0), broken = sample(2);
  int e = emits;
  dec_decode_frame(dec, NULL, shown, 0, NULL);
  expect("a shown frame is emitted with its picture",
         emits == e + 1 && !lastStatus && emitImages == 1);
  dec_decode_frame(dec, NULL, hidden, 0, NULL);
  expect("a sample with no shown frame is dropped, with no error",
         emits == e + 2 && !lastStatus && emitDropped == 1 && emitImages == 1);
  dec_decode_frame(dec, NULL, broken, 0, NULL);
  expect("a sample that fails after showing a frame is an error, no picture",
         emits == e + 3 && lastStatus == kVTVideoDecoderBadDataErr &&
             emitImages == 1);
  CMSampleBufferRef stuck = sample(3);
  dec_decode_frame(dec, NULL, stuck, 0, NULL);
  expect("a sample failed by a stuck GPU or a driver call is a malfunction",
         emits == e + 4 && lastStatus == kVTVideoDecoderMalfunctionErr &&
             emitImages == 1);
  CFRelease(stuck);
  expect("after a device failure the format is not accepted",
         !dec_can_accept_format(dec, fmt));
  CMSampleBufferRef nomem = sample(4);
  dec_decode_frame(dec, NULL, nomem, 0, NULL);
  expect("a sample that runs out of memory is kVTAllocationFailedErr",
         emits == e + 5 && lastStatus == kVTAllocationFailedErr);
  CFRelease(nomem);
  expect("a new start takes the format again",
         !dec_start_session(dec, NULL, fmt) && dec_can_accept_format(dec, fmt));

  // A decode holds the session while the GPU works on its frame.
  CMSampleBufferRef slow = sample(5);
  slowEntered = dispatch_semaphore_create(0);
  slowRelease = dispatch_semaphore_create(0);
  dec_decode_frame(dec, NULL, slow,
                   kVTDecodeFrame_EnableAsynchronousDecompression, NULL);
  bool entered = !dispatch_semaphore_wait(
      slowEntered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
  Boolean accepted = dec_can_accept_format(dec, fmt);
  bool blocked = slowTimedOut;
  dispatch_semaphore_signal(slowRelease);
  dec_finish_delayed_frames(dec);
  expect("canAcceptFormatDescription does not wait for a running decode",
         entered && accepted && !blocked);
  CFRelease(slow);

  e = emits;
  inCallback = reenter;
  dec_decode_frame(dec, NULL, shown, 0, NULL);
  dec_finish_delayed_frames(dec);
  // 1 frame, then from the callback 1 synchronous and MAX_QUEUED + 2 queued.
  expect("calls from inside the output callback neither trap nor hang",
         reentered && emits == e + 2 + MAX_QUEUED + 2);
  inCallback = reenter;
  reentered = 0;
  e = emits;
  dec_decode_frame(dec, NULL, shown,
                   kVTDecodeFrame_EnableAsynchronousDecompression, NULL);
  dec_finish_delayed_frames(dec);
  expect("the same from an asynchronous frame",
         reentered && emits == e + 2 + MAX_QUEUED + 2);

  expect("a NULL sample is refused",
         dec_decode_frame(dec, NULL, NULL, 0, NULL) == kVTParameterErr &&
             dec_decode_frame(dec, NULL, NULL,
                              kVTDecodeFrame_EnableAsynchronousDecompression,
                              NULL) == kVTParameterErr);

  base_invalidate(obj);
  expect("after invalidate the format is not accepted",
         !dec_can_accept_format(dec, fmt));
  e = emits;
  OSStatus r1 = dec_decode_frame(dec, NULL, shown, 0, NULL),
           r2 = dec_decode_frame(dec, NULL, shown,
                                 kVTDecodeFrame_EnableAsynchronousDecompression,
                                 NULL);
  dec_finish_delayed_frames(dec);
  expect("after invalidate frames are refused and nothing is emitted",
         r1 == kVTInvalidSessionErr && r2 == kVTInvalidSessionErr &&
             emits == e && destroys == creates);
  base_finalize(obj);
  free(obj);

  expect("a decoder for the failed start test",
         !CREATE(CODEC, NULL, &dec) && dec);
  expect("its session starts", !dec_start_session(dec, NULL, fmt));
  failCreate = true;
  d0 = destroys;
  expect(
      "a second start whose session cannot be made fails",
      dec_start_session(dec, NULL, fmt) == kVTVideoDecoderNotAvailableNowErr &&
          destroys == d0 + 1);
  failCreate = false;
  expect("after a start that failed, the format is not accepted",
         !dec_can_accept_format(dec, fmt));
  e = emits;
  OSStatus r3 = dec_decode_frame(dec, NULL, shown, 0, NULL);
  expect("and a frame is refused as an invalid session",
         !r3 && emits == e + 1 && lastStatus == kVTInvalidSessionErr);
  base_invalidate((CMBaseObjectRef)dec);
  base_finalize((CMBaseObjectRef)dec);
  free(dec);

  expect("a decoder for the start and invalidate test",
         !CREATE(CODEC, NULL, &dec) && dec);
  int c0 = creates;
  d0 = destroys;
  invalidateInCreate = dec;
  OSStatus ri = dec_start_session(dec, NULL, fmt);
  invalidateInCreate = NULL;
  expect("a start that invalidate overtakes closes its new session",
         ri == kVTInvalidSessionErr && creates == c0 + 1 &&
             destroys == d0 + 1 && !dec_can_accept_format(dec, fmt));
  base_finalize((CMBaseObjectRef)dec);
  free(dec);

#if defined(TEST_AV1)
  // av1C: marker/version, profile 0 level 0, 8-bit 4:2:0, then one config byte.
  static const uint8_t av1C[] = {0x81, 0x00, 0x0c, 0x00, 0x0a};
  CMVideoFormatDescriptionRef withCfg = format(CODEC, av1C, sizeof(av1C));
  expect("a decoder for the av1C test", !CREATE(CODEC, NULL, &dec) && dec);
  configureResult = VCNAV1_UNSUPPORTED;
  int d = destroys;
  expect("an unsupported sequence header in av1C refuses the session",
         dec_start_session(dec, NULL, withCfg) ==
                 kVTVideoDecoderUnsupportedDataFormatErr &&
             destroys == d + 1);
  configureResult = -1;
  expect("unreadable configOBUs are logged, the session still starts",
         !dec_start_session(dec, NULL, withCfg));
  base_invalidate((CMBaseObjectRef)dec);
  base_finalize((CMBaseObjectRef)dec);
  free(dec);
  CFRelease(withCfg);
#endif

  // Every session is finalized by now: a plug-in that keeps a reference to a
  // picture it emitted (or never releases the last one) leaves the count
  // above 1.
  expect("every picture the plug-in took is released",
         CFGetRetainCount(picture) == 1);
  CFRelease(shown);
  CFRelease(hidden);
  CFRelease(broken);
  CFRelease(fmt);
  CVPixelBufferRelease(picture);
  return failures ? 1 : 0;
}
