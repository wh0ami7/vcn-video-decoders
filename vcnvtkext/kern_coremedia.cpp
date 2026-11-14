//
//  kern_coremedia.cpp — see kern_coremedia.hpp.
//
#include "kern_coremedia.hpp"

#include <Headers/kern_api.hpp>
#include <Headers/kern_user.hpp>
#include <Headers/kern_util.hpp>

// Processes whose CoreMedia copy we patch. Suffix match covers Safari's staged
// WebKit path, Safari Technology Preview, and the VideoToolbox XPC service path
// across macOS versions.
static UserPatcher::ProcInfo procInfo[]{
    {"com.apple.WebKit.GPU", 20, 1, UserPatcher::ProcInfo::MatchSuffix},
    {"VTDecoderXPCService", 19, 1, UserPatcher::ProcInfo::MatchSuffix},
};

//
//  figRegistry codesign-validation patch.
//
//  In CoreMedia's figRegistryAddResultItem(), a plug-in found under a
//  non-/System search path (e.g. /Library/Video/Plug-Ins) is validated with
//  SecStaticCodeCheckValidity against kFigRegistryAppleCodeSignRequirement; an
//  item that fails is dropped unless the AppleInternal- only global
//  sDisableRegistryCodesignValidation is set. The relevant code is:
//
//      test   r14b, r14b                 ; r14b = signature valid?
//      mov    r14, [rbp+var_40]
//      jnz    short add_item             ; valid -> add
//      test   cs:sDisableRegistryCodesignValidation, 1
//      jz     return                     ; invalid && !disable -> drop the
//      plug-in ; (disable set) -> fall through to add_item
//
//  We flip the `jnz short add_item` (75 0d) into an unconditional `jmp short
//  add_item` (eb 0d), so every /Library plug-in those processes discover is
//  added regardless of signature. Scoped by UserPatcher to only
//  com.apple.WebKit.GPU / VTDecoderXPCService (above) — not VCN-cdhash-scoped.
//  No other process's code-signing is touched, and /System plug-ins (which
//  never reach this branch) are unaffected.
//
//  The 11-byte pattern below is byte-identical in 15.8.2 (24H232), 26.7.1
//  (25G241) and 26.7.2 (25G332) — verified to occur exactly once in each
//  CoreMedia image — so one patch spans all three. It stops before the
//  RIP-relative operand of the following `test cs:…` instruction, which is the
//  only part that differs between versions.
//
static const uint8_t kFigFind[]{0x45, 0x84, 0xf6, 0x4c, 0x8b, 0x75,
                                0xc0, 0x75, 0x0d, 0xf6, 0x05};
static const uint8_t kFigReplace[]{0x45, 0x84, 0xf6, 0x4c, 0x8b, 0x75,
                                   0xc0, 0xeb, 0x0d, 0xf6, 0x05};

static UserPatcher::BinaryModPatch cmPatches[]{
    {CPU_TYPE_X86_64, 0, kFigFind, kFigReplace, arrsize(kFigFind),
     0,  // skip: patch the first match
     1,  // count: exactly one occurrence per image
     UserPatcher::FileSegment::SegmentTextText, 0},
};

static UserPatcher::BinaryModInfo binaryMod[]{
    {"/System/Library/Frameworks/CoreMedia.framework/Versions/A/CoreMedia",
     cmPatches, arrsize(cmPatches), 0, 0, 0, 0},
};

// Diagnostic: fires when a process matched by procInfo maps its main binary.
// Confirms that the user patcher is active and that our suffix match for
// WebKit.GPU / VTDecoderXPCService works.
static void onBinaryLoad(void*, UserPatcher&, vm_map_t, const char* path,
                         size_t len) {
  SYSLOG("cmbypass", "matched proc loaded: %s (len %zu)",
         path ? path : "(null)", len);
}

void CoreMediaBypass::init() {
  SYSLOG("cmbypass",
         "init: registering figRegistry patch (%zu procs, %zu mods)",
         arrsize(procInfo), arrsize(binaryMod));
  lilu.onProcLoadForce(procInfo, arrsize(procInfo), onBinaryLoad, nullptr,
                       binaryMod, arrsize(binaryMod));
}
