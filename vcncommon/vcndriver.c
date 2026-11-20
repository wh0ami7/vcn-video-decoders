#include "vcndriver.h"

#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

bool vcn_codec_is_amd(CFTypeRef codec) {
  return codec && CFGetTypeID(codec) == CFStringGetTypeID() &&
         CFStringHasPrefix((CFStringRef)codec, CFSTR("AMD"));
}

io_service_t vcn_find_accelerator(void) {
  io_iterator_t it;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceMatching("IOAccelerator"), &it))
    return 0;
  io_service_t s, found = 0;
  while ((s = IOIteratorNext(it))) {
    // Taking the first accelerator that has any codec is wrong on a machine
    // with active integrated graphics: Intel's accelerator can come first in
    // this list (the order is not the one ioreg prints), and AMD's driver then
    // refuses it (CreateAVDAccelerator returns 1).
    CFTypeRef codec =
        IORegistryEntryCreateCFProperty(s, CFSTR("IOGVACodec"), NULL, 0);
    bool amd = vcn_codec_is_amd(codec);
    if (codec) CFRelease(codec);
    if (amd) {
      found = s;
      break;
    }
    IOObjectRelease(s);
  }
  IOObjectRelease(it);
  return found;
}

bool vcn_driver_uuid(char out[33]) {
  for (uint32_t i = 0; i < _dyld_image_count(); i++) {
    const char* name = _dyld_get_image_name(i);
    if (!name || !strstr(name, "AMDRadeonVADriver2")) continue;
    const struct mach_header_64* h = (const void*)_dyld_get_image_header(i);
    if (!h || h->magic != MH_MAGIC_64) continue;
    const struct load_command* lc = (const void*)(h + 1);
    for (uint32_t k = 0; k < h->ncmds;
         k++, lc = (const void*)((const char*)lc + lc->cmdsize))
      if (lc->cmd == LC_UUID) {
        for (int j = 0; j < 16; j++)
          snprintf(out + 2 * j, 3, "%02X",
                   ((const struct uuid_command*)lc)->uuid[j]);
        return true;
      }
  }
  return false;
}

static double wait_now_ms(void) {
  static mach_timebase_info_data_t tb;
  if (!tb.denom) mach_timebase_info(&tb);
  return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

static int query_once(AVD_QueryAVDCommandStatus_fn query, void* driver,
                      void* refcon, uint32_t wait, AVD_QueryEntry* qe) {
  // +8 stays 1, the value sent before it was known to be unused.
  *qe = (AVD_QueryEntry){.refcon = refcon, .unused8 = 1, .wait = wait};
  AVD_QueryList ql = {.count = 1, .entries = qe};
  return query(driver, &ql);
}

VcnWaitResult vcn_wait_command(AVD_QueryAVDCommandStatus_fn query, void* driver,
                               void* refcon, unsigned timeoutMs,
                               uint32_t* hwStatus, int* driverStatus) {
  *hwStatus = 0;
  *driverStatus = 0;
  const double start = wait_now_ms();
  AVD_QueryEntry qe;
  for (;;) {
    int r = query_once(query, driver, refcon, 0, &qe);
    if (r) {
      *driverStatus = r;
      return VCN_WAIT_FAILED;
    }
    if (qe.status == 1) break;
    if (qe.status != 2) {
      *driverStatus = (int)qe.status;
      return VCN_WAIT_FAILED;
    }
    if (wait_now_ms() - start >= timeoutMs) return VCN_WAIT_TIMEOUT;
    // A frame takes milliseconds; a quarter of one between looks adds little
    // latency and keeps the thread asleep most of the time.
    usleep(250);
  }
  // Done: the blocking form now returns at once, runs the driver's steps after
  // a decode and reports the firmware's result.
  int r = query_once(query, driver, refcon, 1, &qe);
  if (r) {
    *driverStatus = r;
    return VCN_WAIT_FAILED;
  }
  *hwStatus = qe.status2;
  return VCN_WAIT_DONE;
}
