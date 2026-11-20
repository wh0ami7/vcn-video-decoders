// What the AV1 and the VP9 session share about Apple's AMD video driver: which
// accelerator to hand it, and which build of it is loaded.

#ifndef VCNCOMMON_VCNDRIVER_H_
#define VCNCOMMON_VCNDRIVER_H_

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdbool.h>

// True when an accelerator's IOGVACodec property names an AMD codec. AMD's
// names start with "AMD" (Navi 21: "AMDVCN2"); Intel's integrated graphics
// publish one too ("Gen95").
bool vcn_codec_is_amd(CFTypeRef codec);

// The first IOAccelerator whose codec is AMD's, or 0. The caller releases it.
io_service_t vcn_find_accelerator(void);

// Writes the LC_UUID of the AMDRadeonVADriver2 image loaded in this process as
// 32 upper-case hex digits. False when the driver is not loaded.
bool vcn_driver_uuid(char out[33]);

#endif  // VCNCOMMON_VCNDRIVER_H_
