// Exercise the av01 decoder through the public VideoToolbox API, as
// Safari/WebKit does.
//   vtav1test list                       -> show av01 decoders VideoToolbox
//   knows about vtav1test <file.ivf> [file.md5] [-n] -> decode with
//   VTDecompressionSession (hardware required),
//                                           compare per-frame I420 MD5, report
//                                           timing
#include <CommonCrypto/CommonDigest.h>
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../vcnav1/av1parse.h"

static double now_ms(void) {
  static mach_timebase_info_data_t tb;
  if (!tb.denom) mach_timebase_info(&tb);
  return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

#include <dlfcn.h>
static void copy_kv(const void* k, const void* v, void* dict) {
  CFDictionarySetValue(dict, k, v);
}
// Register a decoder bundle in this process only (nothing is installed on the
// system).
static int load_bundle(const char* path) {
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      NULL, (const UInt8*)path, strlen(path), true);
  CFBundleRef b = CFBundleCreate(NULL, url);
  CFRelease(url);
  if (!b || !CFBundleLoadExecutable(b)) {
    fprintf(stderr, "cannot load %s\n", path);
    return -1;
  }
  CFArrayRef impls =
      CFBundleGetValueForInfoDictionaryKey(b, CFSTR("CMClassImplementations"));
  OSStatus (*reg)(FourCharCode, CFDictionaryRef, void*) =
      dlsym(RTLD_DEFAULT, "VTRegisterVideoDecoderWithInfo");
  for (CFIndex i = 0; impls && reg && i < CFArrayGetCount(impls); i++) {
    CFDictionaryRef impl = CFArrayGetValueAtIndex(impls, i);
    CFStringRef fn = CFDictionaryGetValue(impl, CFSTR("CMFactoryFunction"));
    void* factory = CFBundleGetFunctionPointerForName(b, fn);
    CFMutableDictionaryRef info = CFDictionaryCreateMutableCopy(NULL, 0, impl);
    // Dynamic registration ignores CMMatchingInfo: lift the matching keys to
    // the top level.
    CFDictionaryRef match = CFDictionaryGetValue(impl, CFSTR("CMMatchingInfo"));
    if (match) CFDictionaryApplyFunction(match, copy_kv, info);
    CFDictionarySetValue(info, CFSTR("IsHardwareAccelerated"), kCFBooleanTrue);
    CFDictionarySetValue(
        info, CFSTR("CodecName"),
        CFDictionaryGetValue(impl, CFSTR("CMClassImplementationName")));
    OSStatus st = reg(kCMVideoCodecType_AV1, info, factory);
    CFRelease(info);
    printf("registered %p from %s -> %d\n", factory, path, (int)st);
  }
  return 0;
}
static int list(void) {
  OSStatus (*copyList)(CFDictionaryRef, CFArrayRef*) =
      dlsym(RTLD_DEFAULT, "VTCopyVideoDecoderList");
  CFArrayRef decs = NULL;
  if (copyList) copyList(NULL, &decs);
  printf("VTCopyVideoDecoderList: %s, %ld decoders\n",
         copyList ? "found" : "missing",
         decs ? (long)CFArrayGetCount(decs) : 0L);
  for (CFIndex i = 0; decs && i < CFArrayGetCount(decs); i++) {
    CFStringRef desc = CFCopyDescription(CFArrayGetValueAtIndex(decs, i));
    if (getenv("LIST_ALL") ||
        CFStringFind(desc, CFSTR("av01"), 0).location != kCFNotFound ||
        CFStringFind(desc, CFSTR("AV1"), 0).location != kCFNotFound)
      CFShow(CFArrayGetValueAtIndex(decs, i));
    CFRelease(desc);
  }
  printf("VTIsHardwareDecodeSupported(av01) = %d\n",
         VTIsHardwareDecodeSupported(kCMVideoCodecType_AV1));
  if (decs) CFRelease(decs);
  return 0;
}
typedef struct {
  FILE* md5;
  int frames, mismatches, errors, nohash;
  double *fed, *lat;  // per-frame feed time and output latency (ms), for -pace
} Ctx;

static void out_cb(void* refcon, void* frameRefcon, OSStatus st,
                   VTDecodeInfoFlags flags, CVImageBufferRef img, CMTime pts,
                   CMTime dur) {
  Ctx* c = refcon;
  if (c->lat) {
    long i = (long)frameRefcon;
    c->lat[i] = now_ms() - c->fed[i];
  }
  if (st || !img) {
    c->errors++;
    printf("frame error %d flags %x\n", (int)st, flags);
    return;
  }
  c->frames++;
  if (c->nohash) return;
  CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
  OSType fmt = CVPixelBufferGetPixelFormatType(img);
  int ten = fmt == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
  int w = (int)CVPixelBufferGetWidth(img), h = (int)CVPixelBufferGetHeight(img);
  uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(img, 0),
          *uv = CVPixelBufferGetBaseAddressOfPlane(img, 1);
  size_t ys = CVPixelBufferGetBytesPerRowOfPlane(img, 0),
         uvs = CVPixelBufferGetBytesPerRowOfPlane(img, 1);
  CC_MD5_CTX m;
  CC_MD5_Init(&m);
  int cw = (w + 1) / 2, ch = (h + 1) / 2;
  // VTAV1_DUMP_Y=<file>: append each frame's 8-bit luma (w*h bytes) to it.
  const char* dump = getenv("VTAV1_DUMP_Y");
  if (dump && !ten) {
    int ok_path = 1;
    for (const unsigned char* c = (const unsigned char*)dump; *c; c++) {
      if (*c < 0x20 || *c == 0x7f ||
          !((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
            (*c >= '0' && *c <= '9') || *c == '.' || *c == '_' || *c == '/' ||
            *c == '-')) {
        ok_path = 0;
        break;
      }
    }
    if (ok_path && !strstr(dump, "..")) {
      FILE* df = fopen(dump, "ab");
      for (int r = 0; df && r < h; r++) fwrite(y + r * ys, 1, w, df);
      if (df) fclose(df);
    }
  }
  uint16_t* row = malloc((size_t)w * 2 + 16);
  if (!row) return;
  for (int r = 0; r < h; r++) {
    if (ten) {
      uint16_t* src = (uint16_t*)(y + r * ys);
      for (int i = 0; i < w; i++) row[i] = src[i] >> 6;
      CC_MD5_Update(&m, row, w * 2);
    } else
      CC_MD5_Update(&m, y + r * ys, w);
  }
  for (int pl = 0; pl < 2; pl++)
    for (int r = 0; r < ch; r++) {
      if (ten) {
        uint16_t* src = (uint16_t*)(uv + r * uvs);
        for (int i = 0; i < cw; i++) row[i] = src[2 * i + pl] >> 6;
        CC_MD5_Update(&m, row, cw * 2);
      } else {
        uint8_t* b = (uint8_t*)row;
        for (int i = 0; i < cw; i++) b[i] = uv[r * uvs + 2 * i + pl];
        CC_MD5_Update(&m, b, cw);
      }
    }
  free(row);
  CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
  uint8_t dg[16];
  char hex[33], expect[64] = "";
  CC_MD5_Final(dg, &m);
  for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", dg[i]);
  if (c->md5) {
    char line[512];
    if (fgets(line, sizeof(line), c->md5)) sscanf(line, "%32s", expect);
    if (strcmp(expect, hex)) {
      c->mismatches++;
      printf("frame %d %dx%d %s MISMATCH expected %s\n", c->frames, w, h, hex,
             expect);
    }
  }
}

static int noop_frame(void* ctx, AV1Parser* p, AV1ParseResult k) { return 0; }

// Build an av1C record (ISO/IEC AV1-ISOBMFF 2.3) from the first temporal unit.
static CFDataRef make_av1C(const uint8_t* tu, size_t n) {
  size_t o = 0;
  while (o < n) {
    uint8_t hdr = tu[o];
    int type = (hdr >> 3) & 15, ext = (hdr >> 2) & 1, has = (hdr >> 1) & 1;
    size_t p = o + 1 + ext, sz = 0;
    for (int i = 0, sh = 0; has && i < 8; i++, sh += 7) {
      uint8_t b = tu[p++];
      sz |= (size_t)(b & 0x7f) << sh;
      if (!(b & 0x80)) break;
    }
    if (!has) sz = n - p;
    if (type == 1) {
      AV1Parser ps;
      av1_parser_init(&ps);
      av1_parse_temporal_unit(&ps, tu + o, p + sz - o, noop_frame, NULL);
      uint8_t rec[4] = {
          0x81, (uint8_t)(ps.seq.seq_profile << 5 | 13),
          (uint8_t)(ps.seq.high_bitdepth << 6 | ps.seq.twelve_bit << 5 |
                    ps.seq.mono_chrome << 4 | ps.seq.subsampling_x << 3 |
                    ps.seq.subsampling_y << 2 | ps.seq.chroma_sample_position),
          0};
      av1_parser_free(&ps);
      CFMutableDataRef d = CFDataCreateMutable(NULL, 0);
      CFDataAppendBytes(d, rec, 4);
      CFDataAppendBytes(d, tu + o, p + sz - o);
      return d;
    }
    o = p + sz;
  }
  return NULL;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: vtav1test list | file.ivf [file.md5] [-n]\n");
    return 1;
  }
  if (argc > 2 && !strcmp(argv[1], "-load")) {
    if (load_bundle(argv[2])) return 1;
    argv += 2;
    argc -= 2;
  }
  if (!strcmp(argv[1], "list")) return list();
  Ctx c = {0};
  int async = 0;
  double pace = 0;  // feed frames at this rate (fps) and report output latency
  const char* decoderID = NULL;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "-n"))
      c.nohash = 1;
    else if (!strcmp(argv[i], "-a"))
      async = 1;
    else if (!strcmp(argv[i], "-pace") && i + 1 < argc)
      pace = atof(argv[++i]);
    else if (!strcmp(argv[i], "-id") && i + 1 < argc)
      decoderID = argv[++i];
    else
      c.md5 = fopen(argv[i], "r");
  }
  FILE* fp = fopen(argv[1], "rb");
  if (!fp) {
    perror(argv[1]);
    return 1;
  }
  uint8_t hdr[32];
  if (fread(hdr, 1, 32, fp) != 32 || memcmp(hdr, "DKIF", 4)) {
    fprintf(stderr, "not IVF\n");
    return 1;
  }
  int w = hdr[12] | hdr[13] << 8, h = hdr[14] | hdr[15] << 8;
  // read all temporal units
  size_t cap = 64, cnt = 0;
  uint8_t** tus = malloc(cap * sizeof(*tus));
  size_t* sizes = malloc(cap * sizeof(*sizes));
  uint8_t fh[12];
  while (fread(fh, 1, 12, fp) == 12) {
    uint32_t sz = fh[0] | fh[1] << 8 | fh[2] << 16 | (uint32_t)fh[3] << 24;
    if (cnt == cap) {
      cap *= 2;
      tus = realloc(tus, cap * sizeof(*tus));
      sizes = realloc(sizes, cap * sizeof(*sizes));
    }
    tus[cnt] = malloc(sz);
    if (fread(tus[cnt], 1, sz, fp) != sz) break;
    sizes[cnt++] = sz;
  }
  CFDataRef av1C = make_av1C(tus[0], sizes[0]);
  if (!av1C) {
    fprintf(stderr, "no sequence header in first TU\n");
    return 1;
  }
  const void* ak[] = {CFSTR("av1C")};
  const void* av[] = {av1C};
  CFDictionaryRef atoms =
      CFDictionaryCreate(NULL, ak, av, 1, &kCFTypeDictionaryKeyCallBacks,
                         &kCFTypeDictionaryValueCallBacks);
  const void* ek[] = {
      kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms};
  const void* ev[] = {atoms};
  CFDictionaryRef ext =
      CFDictionaryCreate(NULL, ek, ev, 1, &kCFTypeDictionaryKeyCallBacks,
                         &kCFTypeDictionaryValueCallBacks);
  CMVideoFormatDescriptionRef fmt = NULL;
  OSStatus st = CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_AV1, w,
                                               h, ext, &fmt);
  if (st) {
    fprintf(stderr, "format description %d\n", (int)st);
    return 1;
  }

  CFMutableDictionaryRef spec =
      CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(
      spec, kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder,
      kCFBooleanTrue);
  if (decoderID) {  // pick one decoder when several handle av01 (e.g. installed
                    // + -load'ed build)
    CFStringRef id =
        CFStringCreateWithCString(NULL, decoderID, kCFStringEncodingUTF8);
    CFDictionarySetValue(spec, CFSTR("DecoderID"), id);
    CFRelease(id);
  }
  VTDecompressionOutputCallbackRecord cb = {out_cb, &c};
  VTDecompressionSessionRef sess = NULL;
  st = VTDecompressionSessionCreate(NULL, fmt, spec, NULL, &cb, &sess);
  printf("VTDecompressionSessionCreate(require HW) = %d\n", (int)st);
  if (st) return 1;
  CFBooleanRef hw = NULL;
  VTSessionCopyProperty(
      sess, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
      NULL, &hw);
  printf("UsingHardwareAcceleratedVideoDecoder = %s\n",
         hw == kCFBooleanTrue ? "true" : "false");

  if (pace > 0) {
    c.fed = calloc(cnt, sizeof(double));
    c.lat = calloc(cnt, sizeof(double));
  }
  double t0 = now_ms();
  for (size_t i = 0; i < cnt; i++) {
    if (pace > 0) {
      double due = t0 + i * 1000.0 / pace;
      while (now_ms() < due) usleep(200);
      c.fed[i] = due;  // lateness vs. the frame's schedule (a blocking decoder
                       // delays later frames too)
    }
    CMBlockBufferRef bb = NULL;
    CMBlockBufferCreateWithMemoryBlock(NULL, tus[i], sizes[i], kCFAllocatorNull,
                                       NULL, 0, sizes[i], 0, &bb);
    CMSampleBufferRef sb = NULL;
    CMSampleTimingInfo ti = {CMTimeMake(1, 30), CMTimeMake((int64_t)i, 30),
                             kCMTimeInvalid};
    size_t ss = sizes[i];
    CMSampleBufferCreateReady(NULL, bb, fmt, 1, 1, &ti, 1, &ss, &sb);
    VTDecodeInfoFlags info = 0;
    st = VTDecompressionSessionDecodeFrame(
        sess, sb, async ? kVTDecodeFrame_EnableAsynchronousDecompression : 0,
        (void*)(long)i, &info);
    if (st) printf("DecodeFrame %zu -> %d\n", i, (int)st);
    CFRelease(sb);
    CFRelease(bb);
  }
  VTDecompressionSessionWaitForAsynchronousFrames(sess);
  double t = now_ms() - t0;
  printf("%s: %d frames, %d mismatches, %d errors, %.1f ms (%.1f fps%s)\n",
         argv[1], c.frames, c.mismatches, c.errors, t, c.frames * 1000.0 / t,
         c.nohash ? (async ? ", async" : "") : ", incl. CPU hashing");
  if (pace > 0) {
    int late33 = 0, late50 = 0;
    double mx = 0, sum = 0;
    for (size_t i = 0; i < cnt; i++) {
      double l = c.lat[i];
      sum += l;
      if (l > mx) mx = l;
      if (l > 33.3) late33++;
      if (l > 50) late50++;
    }
    printf(
        "paced %.0f fps: latency avg %.1f ms, max %.1f ms, >2 frames late %d, "
        ">3 frames late %d (of %zu)\n",
        pace, sum / cnt, mx, late33, late50, cnt);
  }
  VTDecompressionSessionInvalidate(sess);
  CFRelease(sess);
  return c.mismatches || c.errors || c.frames != (int)cnt ? 2 : 0;
}
