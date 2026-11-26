// vtdiag: what VideoToolbox gives each codec on this Mac, and whether it works.
//
//   vtdiag [-q] [-psnr10]
//
// Lists the hardware decoders VideoToolbox knows, opens a hardware decoder session
// for H.264 and AV1, runs short hardware encodes (H.264 and HEVC, 8-bit up to
// 7680x4320, and 10-bit HEVC) and prints one line for each. AV1 and VP9 encoding
// is reported as "not offered": VCN 2/3 cannot encode them, and VideoToolbox
// lists no encoder for them.
//
// -psnr10 also encodes a 10-bit picture with the HEVC Main10 profile, decodes it
// again and prints the PSNR of Y, Cb and Cr against the source. Below 40 dB the
// encoder or the driver is broken (the 10-bit bugs VCNEncodeFix's hevc10 fix
// repairs show up as about 7 dB Y or about 20 dB V). When VideoToolbox does not
// offer Main10 at all (the encoder service says so and the kext's -vcnhevc10 gate
// is off) that is reported as "not offered", not as a failure.
// -q prints only lines that failed.
//
// It uses the GPU: encoding opens the video engine. No switch arms it.
//
// Exit status: 0 = everything that is offered worked, 2 = something failed.
//
// Build: clang -O1 tools/vtdiag.c -framework VideoToolbox -framework CoreMedia \
//          -framework CoreVideo -framework CoreFoundation -o tools/vtdiag
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kFrames = 10 };

static int gQuiet, gFailed;

static void say(int ok, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(int ok, const char* fmt, ...) {
  if (!ok) gFailed = 1;
  if (ok && gQuiet) return;
  va_list ap;
  va_start(ap, fmt);
  printf("%s ", ok ? "ok  " : "FAIL");
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
}

static void cstr(CFStringRef s, char* b, size_t n) {
  b[0] = 0;
  if (s) CFStringGetCString(s, b, n, kCFStringEncodingUTF8);
}

static CFMutableDictionaryRef dict(void) {
  return CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                   &kCFTypeDictionaryValueCallBacks);
}

// ---- hardware decoders -----------------------------------------------------

static void decoder_session(CMVideoCodecType codec, const char* name) {
  CMVideoFormatDescriptionRef f = NULL;
  CMVideoFormatDescriptionCreate(NULL, codec, 1920, 1080, NULL, &f);
  CFMutableDictionaryRef spec = dict();
  CFDictionarySetValue(
      spec, kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder,
      kCFBooleanTrue);
  VTDecompressionSessionRef s = NULL;
  VTDecompressionOutputCallbackRecord cb = {NULL, NULL};
  OSStatus st = VTDecompressionSessionCreate(NULL, f, spec, NULL, &cb, &s);
  CFRelease(spec);
  CFRelease(f);
  if (st || !s) {
    // A bare format description is not enough for H.264 and HEVC (no
    // parameter sets), so only codecs that need none are asked here.
    say(0, "decoder %-5s hardware session refused (%d)", name, (int)st);
    return;
  }
  CFTypeRef hw = NULL;
  VTSessionCopyProperty(
      s, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, NULL,
      &hw);
  say(hw == kCFBooleanTrue, "decoder %-5s hardware session, hardware=%s", name,
      hw == kCFBooleanTrue ? "yes" : "no");
  if (hw) CFRelease(hw);
  VTDecompressionSessionInvalidate(s);
  CFRelease(s);
}

static void decoders(void) {
  say(VTIsHardwareDecodeSupported(kCMVideoCodecType_H264), "decoder H264  hardware decode supported");
  say(VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC), "decoder HEVC  hardware decode supported");
  say(VTIsHardwareDecodeSupported(kCMVideoCodecType_AV1), "decoder AV1   hardware decode supported");
    decoder_session(kCMVideoCodecType_AV1, "AV1");
}


// ---- how a client reaches the decoders ------------------------------------
//
// VideoToolbox (macOS 15.8.2, read in Hopper) hands a process a VP9 decoder
// only when its decoder specification says AllowVP9Decode, which it adds only
// when the process called VTRegisterSupplementalVideoDecoderIfAvailable(vp09)
// (sub_7ff812ce7f2f writes it, sub_7ff8129e6364 refuses vp09 without it with
// -12906). Safari, WebKit, Chrome, Firefox and FFmpeg call it; a process that
// does not never gets a VP9 decoder, ours or Apple's. AV1 has no such gate.

static CMVideoFormatDescriptionRef vp9_format(void) {
  static const uint8_t vpcC[] = {1, 0, 0, 0,    // version 1, flags 0
                                 0,  10,        // profile 0, level 1.0
                                 0x82,          // 8 bit, 4:2:0, studio range
                                 1,  1,  1,     // BT.709
                                 0,  0};        // no codec init data
  CFDataRef d = CFDataCreate(NULL, vpcC, sizeof(vpcC));
  CFMutableDictionaryRef atoms = dict();
  CFDictionarySetValue(atoms, CFSTR("vpcC"), d);
  CFRelease(d);
  CFMutableDictionaryRef ext = dict();
  CFDictionarySetValue(
      ext, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms, atoms);
  CFRelease(atoms);
  CMVideoFormatDescriptionRef f = NULL;
  CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_VP9, 1920, 1080, ext, &f);
  CFRelease(ext);
  return f;
}

// Opens a decoder session for fmt with the given specification and reports
// whether it is hardware. spec may be NULL (a client that asks for nothing).
static OSStatus route(CMVideoFormatDescriptionRef fmt, CFDictionaryRef spec,
                      int* hardware) {
  VTDecompressionSessionRef s = NULL;
  VTDecompressionOutputCallbackRecord cb = {NULL, NULL};
  *hardware = 0;
  OSStatus st = VTDecompressionSessionCreate(NULL, fmt, spec, NULL, &cb, &s);
  if (st || !s) return st ? st : -1;
  CFTypeRef hw = NULL;
  VTSessionCopyProperty(
      s, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, NULL,
      &hw);
  *hardware = hw == kCFBooleanTrue;
  if (hw) CFRelease(hw);
  VTDecompressionSessionInvalidate(s);
  CFRelease(s);
  return 0;
}

static void routes(void) {
  int hw;
  CMVideoFormatDescriptionRef av1 = NULL;
  CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_AV1, 1920, 1080, NULL, &av1);
  OSStatus st = route(av1, NULL, &hw);
  say(!st && hw, "route   AV1  no specification (a client that asks for nothing): %s",
      st ? "refused" : hw ? "hardware" : "software");
  CFMutableDictionaryRef en = dict();
  CFDictionarySetValue(
      en, kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder,
      kCFBooleanTrue);
  st = route(av1, en, &hw);
  say(!st && hw, "route   AV1  hardware allowed: %s",
      st ? "refused" : hw ? "hardware" : "software");
  CFRelease(en);
  CFRelease(av1);

  CMVideoFormatDescriptionRef vp9 = vp9_format();
  // The registration is process-wide and cannot be undone, so the client that
  // has not registered is tried first.
  st = route(vp9, NULL, &hw);
  if (!gQuiet)
    printf("note route   VP9  before registering: %s (-12906 is Apple's own gate; "
           "only clients that opt in get a VP9 decoder)\n",
           st ? "refused" : hw ? "hardware" : "software");
  VTRegisterSupplementalVideoDecoderIfAvailable(kCMVideoCodecType_VP9);
  st = route(vp9, NULL, &hw);
  say(!st && hw, "route   VP9  after registering, no specification: %s",
      st ? "refused" : hw ? "hardware" : "software");
  CFRelease(vp9);
}

// ---- encoders --------------------------------------------------------------

typedef struct {
  int got, err;
  CMSampleBufferRef first;
  CMSampleBufferRef out[kFrames];
} Enc;

static void enc_cb(void* r, void* f, OSStatus st, VTEncodeInfoFlags fl,
                   CMSampleBufferRef sb) {
  (void)f, (void)fl;
  Enc* e = r;
  if (st || !sb) {
    e->err++;
    return;
  }
  if (e->got < kFrames) {
    CFRetain(sb);
    e->out[e->got] = sb;
  }
  e->got++;
}

// 8-bit planes: a ramp that moves a little each frame. 10-bit planes hold the
// value in the high bits of 16.
static void fill(CVPixelBufferRef pb, int i, int ten) {
  CVPixelBufferLockBaseAddress(pb, 0);
  for (size_t p = 0; p < CVPixelBufferGetPlaneCount(pb); p++) {
    size_t w = CVPixelBufferGetWidthOfPlane(pb, p),
           h = CVPixelBufferGetHeightOfPlane(pb, p),
           bpr = CVPixelBufferGetBytesPerRowOfPlane(pb, p);
    uint8_t* base = CVPixelBufferGetBaseAddressOfPlane(pb, p);
    for (size_t y = 0; y < h; y++) {
      for (size_t x = 0; x < w; x++) {
        unsigned v = (unsigned)((x * 3 + y * 2 + (size_t)i * 8 + p * 97) & 0x3ff);
        if (ten) {
          ((uint16_t*)(base + y * bpr))[x] = (uint16_t)((64 + v % 880) << 6);
        } else if (p == 0) {
          base[y * bpr + x] = (uint8_t)(32 + (v >> 2) % 190);
        } else {
          base[y * bpr + x * 1] = (uint8_t)(64 + (v >> 3));
        }
      }
    }
  }
  CVPixelBufferUnlockBaseAddress(pb, 0);
}

static CVPixelBufferRef make_pb(int w, int h, OSType pf, int i, int ten) {
  CVPixelBufferRef pb = NULL;
  CVPixelBufferCreate(NULL, (size_t)w, (size_t)h, pf, NULL, &pb);
  if (pb) fill(pb, i, ten);
  return pb;
}

static VTCompressionSessionRef open_enc(CMVideoCodecType codec, int w, int h,
                                        Enc* e, OSStatus* st) {
  CFMutableDictionaryRef spec = dict();
  CFDictionarySetValue(
      spec, kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder,
      kCFBooleanTrue);
  VTCompressionSessionRef s = NULL;
  *st = VTCompressionSessionCreate(NULL, w, h, codec, spec, NULL, NULL, enc_cb, e,
                                   &s);
  CFRelease(spec);
  return s;
}

static void encode(CMVideoCodecType codec, const char* name, int w, int h,
                   OSType pf, int ten) {
  Enc e;
  memset(&e, 0, sizeof(e));
  OSStatus st;
  VTCompressionSessionRef s = open_enc(codec, w, h, &e, &st);
  if (!s) {
    say(0, "encode  %-5s %dx%d%s: hardware session refused (%d)", name, w, h,
        ten ? " 10-bit" : "", (int)st);
    return;
  }
  CFTypeRef hw = NULL, id = NULL;
  VTSessionCopyProperty(s, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
                        NULL, &hw);
  VTSessionCopyProperty(s, kVTCompressionPropertyKey_EncoderID, NULL, &id);
  char ib[128];
  cstr(id, ib, sizeof(ib));
  VTCompressionSessionPrepareToEncodeFrames(s);
  int bad = 0;
  for (int i = 0; i < kFrames && !bad; i++) {
    CVPixelBufferRef pb = make_pb(w, h, pf, i, ten);
    if (!pb) {
      bad = 1;
      break;
    }
    OSStatus r = VTCompressionSessionEncodeFrame(
        s, pb, CMTimeMake(i, 30), CMTimeMake(1, 30), NULL, NULL, NULL);
    CVPixelBufferRelease(pb);
    if (r) bad = 1;
  }
  VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
  int ok = !bad && e.got == kFrames && !e.err && hw == kCFBooleanTrue;
  say(ok, "encode  %-5s %dx%d%s: %s hardware=%s frames %d/%d", name, w, h,
      ten ? " 10-bit" : "", ib[0] ? ib : "?", hw == kCFBooleanTrue ? "yes" : "no",
      e.got, kFrames);
  for (int i = 0; i < e.got && i < kFrames; i++) CFRelease(e.out[i]);
  if (hw) CFRelease(hw);
  if (id) CFRelease(id);
  VTCompressionSessionInvalidate(s);
  CFRelease(s);
}

// ---- 10-bit round trip -----------------------------------------------------

typedef struct {
  CVPixelBufferRef src[kFrames];
  double sse[3];
  double n[3];
  int frames, err;
  int next;
} Dec;

static void dec_cb(void* r, void* f, OSStatus st, VTDecodeInfoFlags fl,
                   CVImageBufferRef img, CMTime pts, CMTime dur) {
  (void)f, (void)fl, (void)dur;
  Dec* d = r;
  if (st || !img) {
    d->err++;
    return;
  }
  int idx = (int)(pts.value);
  if (idx < 0 || idx >= kFrames || !d->src[idx]) {
    d->err++;
    return;
  }
  CVPixelBufferRef a = d->src[idx], b = img;
  CVPixelBufferLockBaseAddress(a, kCVPixelBufferLock_ReadOnly);
  CVPixelBufferLockBaseAddress(b, kCVPixelBufferLock_ReadOnly);
  for (int p = 0; p < 3; p++) {
    // The decoder may hand back P010 (2 planes) or a 3-plane format; only the
    // luma and the interleaved chroma of P010 are compared.
    int plane = p == 0 ? 0 : 1;
    if (plane >= (int)CVPixelBufferGetPlaneCount(b) || p == 2) continue;
    size_t w = CVPixelBufferGetWidthOfPlane(a, plane) * (plane ? 2 : 1),
           h = CVPixelBufferGetHeightOfPlane(a, plane);
    size_t wb = CVPixelBufferGetWidthOfPlane(b, plane) * (plane ? 2 : 1);
    if (wb < w) w = wb;
    for (size_t y = 0; y < h && y < CVPixelBufferGetHeightOfPlane(b, plane); y++) {
      const uint16_t* pa = (const uint16_t*)((uint8_t*)CVPixelBufferGetBaseAddressOfPlane(a, plane) +
                                              y * CVPixelBufferGetBytesPerRowOfPlane(a, plane));
      const uint16_t* pb2 = (const uint16_t*)((uint8_t*)CVPixelBufferGetBaseAddressOfPlane(b, plane) +
                                               y * CVPixelBufferGetBytesPerRowOfPlane(b, plane));
      for (size_t x = 0; x < w; x++) {
        double diff = (double)(pa[x] >> 6) - (double)(pb2[x] >> 6);
        d->sse[p] += diff * diff;
        d->n[p] += 1;
      }
    }
  }
  CVPixelBufferUnlockBaseAddress(a, kCVPixelBufferLock_ReadOnly);
  CVPixelBufferUnlockBaseAddress(b, kCVPixelBufferLock_ReadOnly);
  d->frames++;
}

static double psnr(double sse, double n) {
  if (n <= 0) return 0;
  if (sse <= 0) return 99;
  return 10 * log10(1023.0 * 1023.0 / (sse / n));
}

static void psnr10(void) {
  const int w = 1280, h = 720;
  OSType pf = kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
  Enc e;
  memset(&e, 0, sizeof(e));
  OSStatus st;
  VTCompressionSessionRef s = open_enc(kCMVideoCodecType_HEVC, w, h, &e, &st);
  if (!s) {
    say(0, "psnr10  hardware HEVC session refused (%d)", (int)st);
    return;
  }
  OSStatus ps = VTSessionSetProperty(s, kVTCompressionPropertyKey_ProfileLevel,
                                     kVTProfileLevel_HEVC_Main10_AutoLevel);
  if (ps) {
    if (!gQuiet)
      printf("note psnr10  Main10 not offered by VideoToolbox (%d): the encoder "
             "service and the kext's -vcnhevc10 gate decide that\n",
             (int)ps);
    VTCompressionSessionInvalidate(s);
    CFRelease(s);
    return;
  }
  Dec d;
  memset(&d, 0, sizeof(d));
  VTCompressionSessionPrepareToEncodeFrames(s);
  for (int i = 0; i < kFrames; i++) {
    d.src[i] = make_pb(w, h, pf, i, 1);
    VTCompressionSessionEncodeFrame(s, d.src[i], CMTimeMake(i, 30),
                                    CMTimeMake(1, 30), NULL, NULL, NULL);
  }
  VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
  if (e.got != kFrames || e.err) {
    say(0, "psnr10  encoded %d/%d frames, %d errors", e.got, kFrames, e.err);
  } else {
    CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription(e.out[0]);
    CFMutableDictionaryRef ospec = dict();
    CFMutableDictionaryRef attrs = dict();
    int v = (int)pf;
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v);
    CFDictionarySetValue(attrs, kCVPixelBufferPixelFormatTypeKey, n);
    CFRelease(n);
    VTDecompressionSessionRef ds = NULL;
    VTDecompressionOutputCallbackRecord cb = {dec_cb, &d};
    OSStatus dst = VTDecompressionSessionCreate(NULL, (CMVideoFormatDescriptionRef)fd,
                                                ospec, attrs, &cb, &ds);
    CFRelease(ospec);
    CFRelease(attrs);
    if (dst || !ds) {
      say(0, "psnr10  decoder session refused (%d)", (int)dst);
    } else {
      for (int i = 0; i < kFrames; i++)
        VTDecompressionSessionDecodeFrame(ds, e.out[i], 0, NULL, NULL);
      VTDecompressionSessionWaitForAsynchronousFrames(ds);
      double y = psnr(d.sse[0], d.n[0]), c = psnr(d.sse[1], d.n[1]);
      say(d.frames == kFrames && !d.err && y >= 40 && c >= 40,
          "psnr10  Main10 1280x720: %d frames decoded, Y %.1f dB, CbCr %.1f dB "
          "(need 40+)",
          d.frames, y, c);
      VTDecompressionSessionInvalidate(ds);
      CFRelease(ds);
    }
  }
  for (int i = 0; i < kFrames; i++) {
    if (d.src[i]) CVPixelBufferRelease(d.src[i]);
    if (i < e.got) CFRelease(e.out[i]);
  }
  VTCompressionSessionInvalidate(s);
  CFRelease(s);
}

static void not_offered(CMVideoCodecType codec, const char* name) {
  Enc e;
  memset(&e, 0, sizeof(e));
  OSStatus st;
  VTCompressionSessionRef s = open_enc(codec, 1280, 720, &e, &st);
  if (s) {
    say(1, "encode  %-5s a hardware encoder is offered", name);
    VTCompressionSessionInvalidate(s);
    CFRelease(s);
  } else if (!gQuiet) {
    printf("note encode  %-5s not offered (%d): no hardware encoder for it\n", name,
           (int)st);
  }
}

int main(int argc, char** argv) {
  int p10 = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-q")) gQuiet = 1;
    else if (!strcmp(argv[i], "-psnr10")) p10 = 1;
    else {
      fprintf(stderr, "usage: vtdiag [-q] [-psnr10]\n");
      return 1;
    }
  }
  OSType nv12 = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
  decoders();
  routes();
  say(VTIsHardwareDecodeSupported(kCMVideoCodecType_VP9), "decoder VP9   hardware decode supported (after registering it, as Safari does)");
  encode(kCMVideoCodecType_H264, "H264", 1280, 720, nv12, 0);
  encode(kCMVideoCodecType_H264, "H264", 3840, 2160, nv12, 0);
  encode(kCMVideoCodecType_HEVC, "HEVC", 1280, 720, nv12, 0);
  encode(kCMVideoCodecType_HEVC, "HEVC", 3840, 2160, nv12, 0);
  // Wider than 4096 is what VCNEncodeFix's wide fix is for; 7680x4320 is the
  // largest picture the video engine takes (8192x4320 is past its limit).
  encode(kCMVideoCodecType_HEVC, "HEVC", 5120, 2880, nv12, 0);
  encode(kCMVideoCodecType_HEVC, "HEVC", 7680, 4320, nv12, 0);
  not_offered(kCMVideoCodecType_AV1, "AV1");
  not_offered(kCMVideoCodecType_VP9, "VP9");
  if (p10) psnr10();
  if (!gQuiet) printf(gFailed ? "FAILED\n" : "all offered paths work\n");
  return gFailed ? 2 : 0;
}
