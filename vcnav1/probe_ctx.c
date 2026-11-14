// Stage 1: open the AMD VA driver directly and try to create an AV1 (VCN)
// decode context. usage: probe_ctx [function_hex=0x1000000] [mode=5]
// [width=1920] [height=1080]
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avd.h"

static int parse_u32_arg(const char* s, uint32_t lo, uint32_t hi, uint32_t* out) {
  if (!s || !out) return -1;
  errno = 0;
  char* end = NULL;
  unsigned long v = strtoul(s, &end, 10);
  if (!end || end == s || *end || errno == ERANGE || v < lo || v > hi) return -1;
  *out = (uint32_t)v;
  return 0;
}

static io_service_t findAccelerator(void) {
  io_iterator_t it;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceMatching("IOAccelerator"), &it))
    return 0;
  io_service_t s, found = 0;
  while ((s = IOIteratorNext(it))) {
    CFTypeRef codec =
        IORegistryEntryCreateCFProperty(s, CFSTR("IOGVACodec"), NULL, 0);
    if (codec) {
      CFRelease(codec);
      found = s;
      break;
    }
    IOObjectRelease(s);
  }
  IOObjectRelease(it);
  return found;
}

int main(int argc, char** argv) {
  uint64_t func = AVD_FUNC_AV1;
  uint32_t mode = 5, w = 1920, h = 1080;
  if (argc > 1) {
    errno = 0;
    char* end = NULL;
    unsigned long long v = strtoull(argv[1], &end, 16);
    if (!end || end == argv[1] || *end || errno == ERANGE) {
      fprintf(stderr, "invalid function hex\n");
      return 1;
    }
    func = (uint64_t)v;
  }
  if (argc > 2 && parse_u32_arg(argv[2], 0, 255, &mode)) {
    fprintf(stderr, "invalid mode\n");
    return 1;
  }
  if (argc > 3 && parse_u32_arg(argv[3], 1, 8192, &w)) {
    fprintf(stderr, "invalid width\n");
    return 1;
  }
  if (argc > 4 && parse_u32_arg(argv[4], 1, 8192, &h)) {
    fprintf(stderr, "invalid height\n");
    return 1;
  }

  void* lib = dlopen(AVD_DRIVER_PATH, RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    printf("dlopen: %s\n", dlerror());
    return 1;
  }
  AVD_CreateAVDAccelerator_fn create =
      (AVD_CreateAVDAccelerator_fn)dlsym(lib, "AVD_CreateAVDAccelerator");
  printf("AVD_CreateAVDAccelerator=%p\n", create);

  io_service_t svc = findAccelerator();
  if (!svc) {
    fprintf(stderr, "no AMD accelerator\n");
    return 1;
  }
  uint64_t regid = 0;
  IORegistryEntryGetRegistryEntryID(svc, &regid);
  printf("accelerator service=0x%x regid=0x%llx\n", svc, regid);

  uint32_t services[8] = {svc};
  AVD_AcceleratorCreate ac = {
      .index = 0, .services = services, .validMask = 0xffffffff};
  uint8_t info[256] = {0};
  AVD_Accelerator* accel = NULL;
  int r = create(&accel, &ac, info);
  printf("CreateAVDAccelerator -> %d accel=%p name='%.16s'\n", r, accel,
         (char*)info + 0x1c);
  if (r) return 1;

  void* drv = NULL;
  AVD_DriverCreate dc = {.initFlags = 0, .accel = accel};
  r = ((AVD_CreateDriver_fn)accel->fn[AVD_FN_CreateDriver])(&drv, &dc);
  printf("CreateDriver -> %d driver=%p\n", r, drv);
  if (r) return 1;

  void* bsAddr[4] = {0};
  AVD_BitstreamPool pool = {.type = 0,
                            .size = 0x1800000,
                            .format = 0x72617762,
                            .one = 1,
                            .count = 1,
                            .addressesOut = bsAddr,
                            .eight = 8,
                            .five = 5};
  r = ((AVD_AllocateBitstreamBufferPool_fn)
           accel->fn[AVD_FN_AllocateBitstreamBufferPool])(drv, &pool);
  printf("AllocateBitstreamBufferPool -> %d buffer=%p\n", r, bsAddr[0]);

  AVD_ContextList list = {0};
  void* ctxOut[4] = {0};
  AVD_FunctionInfo* fis[1] = {&list.inlineInfo};
  list.count = 1;
  list.functions = fis;
  list.contextsOut = ctxOut;
  list.inlineInfo = (AVD_FunctionInfo){.kind = 1,
                                       .function = func,
                                       .mode = mode,
                                       .pixelRate = w * h * 30,
                                       .width = w,
                                       .height = h,
                                       .inFormat = 0x72617762,
                                       .inFormatCount = 1,
                                       .outFormat = 0x4e563132,
                                       .outFormatCount = 1};
  AVD_DriverContextInit ci = {.list = &list};
  r = ((AVD_CreateContexts_fn)accel->fn[AVD_FN_CreateContexts])(drv, &ci);
  printf("CreateContexts(func=0x%llx mode=%u %ux%u) -> %d context=%p caps=%p\n",
         func, mode, w, h, r, ctxOut[0], ci.capsOut);
  if (!r && ctxOut[0]) {
    AVD_DriverContextDelete del = {.count = 1, .contexts = ctxOut};
    printf(
        "DeleteContexts -> %d\n",
        ((AVD_DeleteContexts_fn)accel->fn[AVD_FN_DeleteContexts])(drv, &del));
  }
  ((AVD_FreeBitstreamBufferPool_fn)accel->fn[AVD_FN_FreeBitstreamBufferPool])(
      drv, 0);
  ((AVD_DeleteDriver_fn)accel->fn[AVD_FN_DeleteDriver])(drv);
  return r ? 2 : 0;
}
