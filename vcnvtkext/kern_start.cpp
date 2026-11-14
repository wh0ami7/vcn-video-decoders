//
//  kern_start.cpp — VCNVTLoader, a Lilu plugin.
//
//  Lets the AMD VCN AV1/VP9 VideoToolbox plugins load from
//  /Library/Video/Plug-Ins instead of the sealed /System volume. Two
//  independent barriers are removed:
//
//   * AMFI library validation (kernel) — kern_amfi.cpp. Scoped to the two
//     allow-listed plugin cdhashes. Self-contained without AMFIPass; if
//     AMFIPass (or amfi_get_out_of_my_way) is present the hook never fires.
//   * CoreMedia figRegistry code-signature check (userspace) —
//     kern_coremedia.cpp, only inside com.apple.WebKit.GPU /
//     VTDecoderXPCService. That patch accepts any /Library plug-in those
//     processes discover; trust still depends on AMFI for mapping.
//
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/plugin_start.hpp>

#include "kern_amfi.hpp"
#include "kern_coremedia.hpp"

static const char* bootargOff[]{"-vcnvtoff"};
static const char* bootargDebug[]{"-vcnvtdbg"};
static const char* bootargBeta[]{"-vcnvtbeta"};

PluginConfiguration ADDPR(config){
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::Sequoia,  // 15.x (24)  — Izzy
    KernelVersion::Tahoe,    // 26.x (25)  — user 26.7.1 / 26.7.2
    []() {
      SYSLOG("vcnvt", "pluginStart running (kernel major %d)",
             getKernelVersion());
      AmfiBypass::init();       // register AMFI kext route (library validation
                                // override)
      CoreMediaBypass::init();  // register userspace figRegistry patch
    }};
