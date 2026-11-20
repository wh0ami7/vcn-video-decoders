// What the AV1 and the VP9 session share about Apple's AMD video driver: which
// accelerator to hand it, and which build of it is loaded.

#ifndef VCNCOMMON_VCNDRIVER_H_
#define VCNCOMMON_VCNDRIVER_H_

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdbool.h>

#include "avd.h"

// True when an accelerator's IOGVACodec property names an AMD codec. AMD's
// names start with "AMD" (Navi 21: "AMDVCN2"); Intel's integrated graphics
// publish one too ("Gen95").
bool vcn_codec_is_amd(CFTypeRef codec);

// The first IOAccelerator whose codec is AMD's, or 0. The caller releases it.
io_service_t vcn_find_accelerator(void);

// Writes the LC_UUID of the AMDRadeonVADriver2 image loaded in this process as
// 32 upper-case hex digits. False when the driver is not loaded.
bool vcn_driver_uuid(char out[33]);

// How long one decode command may take before the session gives up on it. A 4K
// frame takes 5-60 ms on Navi 21, even at the video engine's lowest clock.
enum { VCN_COMMAND_TIMEOUT_MS = 1000 };

typedef enum {
  VCN_WAIT_DONE = 0,     // finished; *hwStatus = firmware result, 0 = no error
  VCN_WAIT_TIMEOUT = 1,  // still running after timeoutMs: the GPU is stuck
  VCN_WAIT_FAILED = -1,  // the query failed; *driverStatus says how
} VcnWaitResult;

// Waits for one command that ScheduleAVDCommands accepted (refcon is what it
// wrote back), at most timeoutMs. The driver's own blocking wait has no
// timeout in user space, so this polls without blocking, then makes one
// blocking call once the command is done; that call returns at once with the
// firmware's result. On VCN_WAIT_FAILED, *driverStatus is the driver's return
// value, or, when the driver returned 0, the unexpected status it reported.
VcnWaitResult vcn_wait_command(AVD_QueryAVDCommandStatus_fn query, void* driver,
                               void* refcon, unsigned timeoutMs,
                               uint32_t* hwStatus, int* driverStatus);

#endif  // VCNCOMMON_VCNDRIVER_H_
