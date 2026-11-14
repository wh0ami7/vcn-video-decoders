//
//  kern_start.cpp - VCNVideoProfile, a Lilu plugin.
//
//  macOS keeps Navi 21 on the COMPUTE workload profile because the
//  framebuffer's Info.plist sets PP_WorkLoadPolicyMask=16 and copies it over
//  the GPU's registry entry at start, so a device property cannot change it.
//  Linux runs VCN under the VIDEO profile instead. This plugin answers that one
//  registry read with another mask and logs what actually reaches the firmware.
//
//  Boot args: -vcnvpoff disables it, vcnwlmask=N picks the mask (default
//  kWlVideo / 4; valid 1..127). Named bits: kWlFullscreen3D, kWlPowerSaving,
//  kWlVideo, kWlVr, kWlCompute, kWlCustom, kWlW3D.
//
#include <Headers/kern_api.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/plugin_start.hpp>

// Bits from Phw_SwSmu_WorkLoadPolicyManager log strings.
enum : uint32_t {
  kWlFullscreen3D = 1u << 0,
  kWlPowerSaving = 1u << 1,
  kWlVideo = 1u << 2,
  kWlVr = 1u << 3,
  kWlCompute = 1u << 4,
  kWlCustom = 1u << 5,
  kWlW3D = 1u << 6,
};
static constexpr uint32_t kDefaultMask = kWlVideo;
static constexpr uint32_t kMaxMask =
    kWlFullscreen3D | kWlPowerSaving | kWlVideo | kWlVr | kWlCompute |
    kWlCustom | kWlW3D;
static constexpr int kValidatedKernelMinor =
    6;  // Darwin 25.6, the build these signatures were read from
static constexpr int kMaxPolicyLogs = 16;

static uint32_t gMask = kDefaultMask;

static const char* kHWLibsPaths[]{
    "/System/Library/Extensions/AMDRadeonX6000HWServices.kext/Contents/PlugIns/"
    "AMDRadeonX6000HWLibs.kext/Contents/MacOS/AMDRadeonX6000HWLibs"};
static KernelPatcher::KextInfo kextHWLibs{"com.apple.kext.AMDRadeonX6000HWLibs",
                                          kHWLibsPaths,
                                          arrsize(kHWLibsPaths),
                                          {},
                                          {},
                                          KernelPatcher::KextInfo::Unloaded};

// PECI_ReadRegistry(PECI *, const char *key, uint32_t *data, uint32_t
// defaultValue); 1 means success.
using t_readRegistry = int64_t (*)(void* peci, const char* key, uint32_t* data,
                                   uint32_t defaultValue);
static t_readRegistry orgReadRegistry{nullptr};

static int64_t wrapReadRegistry(void* peci, const char* key, uint32_t* data,
                                uint32_t defaultValue) {
  int64_t result = orgReadRegistry(peci, key, data, defaultValue);
  if (key && data && strcmp(key, "PP_WorkLoadPolicyMask") == 0) {
    uint32_t answer = __atomic_load_n(&gMask, __ATOMIC_RELAXED);
    SYSLOG("vcnvp",
           "PP_WorkLoadPolicyMask read %u, answering %u (read result %lld)",
           *data, answer, result);
    *data = answer;
  }
  return result;
}

// SW_SMUM_Dpm_SetWorkloadPolicy(SMUM *, mask); 1 means the firmware accepted
// the message.
using t_setPolicy = int64_t (*)(void* smum, uint32_t mask);
static t_setPolicy orgSetPolicy{nullptr};

static int64_t wrapSetPolicy(void* smum, uint32_t mask) {
  // Freestanding kext (-nostdinc++): no <atomic>. Use builtins so concurrent
  // SMU callbacks cannot race the dedupe state (C++ Core Guidelines CP).
  static uint32_t lastMask = UINT32_MAX;
  static int64_t lastResult = INT64_MIN;
  static int logged = 0;
  int64_t result = orgSetPolicy(smum, mask);
  uint32_t prevMask = __atomic_exchange_n(&lastMask, mask, __ATOMIC_RELAXED);
  int64_t prevResult =
      __atomic_exchange_n(&lastResult, result, __ATOMIC_RELAXED);
  if (mask != prevMask || result != prevResult) {
    int n = __atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED);
    if (n < kMaxPolicyLogs) {
      SYSLOG("vcnvp", "firmware workload policy mask %u, result %lld", mask,
             result);
    }
  }
  return result;
}

static void onHWLibsLoad(void*, KernelPatcher& patcher, size_t index,
                         mach_vm_address_t address, size_t size) {
  if (kextHWLibs.loadIndex != index) return;

  KernelPatcher::RouteRequest requests[]{
      {"_PECI_ReadRegistry", wrapReadRegistry, orgReadRegistry},
      {"_SW_SMUM_Dpm_SetWorkloadPolicy", wrapSetPolicy, orgSetPolicy},
  };
  if (patcher.routeMultiple(index, requests, address, size))
    SYSLOG("vcnvp", "routed, workload mask override %u",
           __atomic_load_n(&gMask, __ATOMIC_RELAXED));
  else {
    SYSLOG("vcnvp", "route failed (%d), driver left as it is",
           patcher.getError());
    patcher.clearError();
  }
}

static const char* bootargOff[]{"-vcnvpoff"};
static const char* bootargDebug[]{"-vcnvpdbg"};
static const char* bootargBeta[]{"-vcnvpbeta"};

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
    KernelVersion::Tahoe,
    KernelVersion::Tahoe,
    []() {
      if (getKernelMinorVersion() != kValidatedKernelMinor) {
        SYSLOG("vcnvp", "kernel minor %d is not validated, doing nothing",
               getKernelMinorVersion());
        return;
      }
      uint32_t requested = 0;
      if (PE_parse_boot_argn("vcnwlmask", &requested, sizeof(requested))) {
        if (requested >= 1 && requested <= kMaxMask)
          __atomic_store_n(&gMask, requested, __ATOMIC_RELAXED);
        else
          SYSLOG("vcnvp", "ignoring vcnwlmask=%u, using %u", requested,
                 __atomic_load_n(&gMask, __ATOMIC_RELAXED));
      }
      lilu.onKextLoadForce(&kextHWLibs, 1, onHWLibsLoad, nullptr);
    }};
