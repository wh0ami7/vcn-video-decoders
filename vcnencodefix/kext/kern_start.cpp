//
//  kern_start.cpp - VCNEncodeFloor, a Lilu plugin (part of VCNEncodeFix).
//
//  Every hardware encode session on Navi 21 asks PowerPlay for clocks once, at session start, through the
//  CWDDEPM escape 0xC00053 (PEM_CWDDEPM_PMRequest). The request has a VCN clock floor field (payload +60,
//  "VclkFloor", 10 kHz units) that Apple's encoder (AMDRadeonVADriver2, VAUveEncoder+0x530) always leaves at 0,
//  so the SMU firmware starts the VCN at its lowest level (VCLK 362 / DCLK 316 MHz) and keeps it there while the
//  application feeds CPU-filled frames. This plugin fills that field for encoder requests. The floor becomes an
//  SMU soft minimum for the life of the session and goes away when the session's power client is released.
//
//  Which requests are changed: VCN client type 28 or 33 (VCN instance 0 or 1), VclkFloor 0, and a non-zero
//  minMclk (payload +36). Only the encoder fills minMclk (VAUveEncoder+0x534, 40000 on Navi 21); the VCN decoder
//  sends its request without the clock-request struct, so minMclk is 0 there and decode is left alone.
//
//  macOS ships two Navi 21 power libraries, AMDRadeonX6000HWLibs and AMDRadeonX6800HWLibs; AMDRadeonX6000HWServices
//  picks one per GPU (natively X6000 for the RX 6800 / 6800 XT / 6900 XT; setups with NootRX load X6800). Their
//  PEM_CWDDEPM_PMRequest is the same code, so the plugin hooks whichever loads. It only routes the function when the
//  library is one of the builds the payload layout was read from in IDA (LC_UUID, kX6000Builds / kX6800Builds). On
//  26.7.3 and 15.8.x the function is found by its symbol; Sonoma 14.8.9 ships both libraries stripped, so there it is
//  routed at its offset from the image header after its first 32 bytes are compared.
//  Its state is published as the property "VCNEncodeFloor" on IOResources
//  (ioreg -rd1 -c IOResources | grep VCNEncodeFloor).
//
//  Boot args: -vcnflooroff disables it; vcnfloor=N sets the floor (default 132297); -vcnfloorall also changes
//  requests with minMclk 0 (decode); -vcnfloordbg / -vcnfloorbeta are Lilu's usual debug / beta switches.
//
#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOService.h>
#include <mach-o/loader.h>

// 132297 in 10 kHz units: VCLK soft minimum 1322 MHz, which the firmware rounds up to its 1400 MHz level; the
// DCLK request of PEM_CWDDEPM_PMRequest is 0, so the DCLK soft minimum is the raw value, of which the SMU message
// keeps 16 bits: 132297 & 0xFFFF = 1225 MHz, the top DCLK level. This exact value was tested; others were not.
static constexpr uint32_t kDefaultFloor = 132297;
static constexpr uint32_t kClientVcn0 = 28;
static constexpr uint32_t kClientVcn1 = 33;
static constexpr size_t kPayloadSize = 112;       // PP_Cwdde's input size for escape 0x53
static constexpr size_t kOffClientType = 16;
static constexpr size_t kOffMinMclk = 36;
static constexpr size_t kOffVclkFloor = 60;
static constexpr int kMaxLogs = 32;

// Validated builds. In each, PEM_CWDDEPM_PMRequest reads the same payload fields at the same offsets as
// AMDRadeonX6000HWLibs of 26.7.3 (compared in Hex-Rays: 15.8.x is identical line for line, and X6800 differs from X6000
// only in the source line numbers its log calls pass).
struct HWLibsBuild {
	const char *macos;
	uint8_t uuid[16];
	size_t offset;          // 0: route by symbol; else the function's offset from the image header (stripped kext)
	uint8_t prologue[32];   // the function's first bytes when routed by offset
};
static const HWLibsBuild kX6000Builds[] {
	{ "26.7.3 (25G342)",
	  { 0x5c, 0xd4, 0xbc, 0x0c, 0xc2, 0x25, 0x36, 0x5f, 0xa8, 0x72, 0xc8, 0x85, 0x57, 0x5b, 0x9a, 0x19 }, 0, {} },
	{ "15.8.2 (24H232), 15.8.3 (24H241)",
	  { 0x9e, 0x9a, 0x8a, 0x30, 0x56, 0xf9, 0x35, 0x56, 0x8e, 0x40, 0x37, 0xbb, 0xfe, 0x44, 0x42, 0x45 }, 0, {} },
	{ "14.8.9 (23J631)",
	  { 0x66, 0x24, 0x82, 0x9b, 0xbd, 0x01, 0x30, 0x35, 0x88, 0x04, 0x3b, 0x1a, 0x9a, 0xee, 0xba, 0x18 }, 0xf3536,
	  { 0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
	    0x08, 0x02, 0x00, 0x00, 0x49, 0x89, 0xd6, 0x49, 0x89, 0xf7, 0x49, 0x89, 0xfc, 0x48, 0x89, 0x7d } },
};
static const HWLibsBuild kX6800Builds[] {
	{ "26.7.3 (25G342)",
	  { 0x27, 0x7b, 0x80, 0x59, 0x8c, 0x8e, 0x3a, 0x34, 0x8b, 0x72, 0x19, 0x4a, 0xe9, 0x37, 0xaa, 0xe1 }, 0, {} },
	{ "15.8.2 (24H232), 15.8.3 (24H241)",
	  { 0x07, 0x01, 0x2a, 0x5e, 0x6c, 0x6e, 0x38, 0xf2, 0x8b, 0x35, 0x05, 0xad, 0xdf, 0xd3, 0x2f, 0x1f }, 0, {} },
	{ "14.8.9 (23J631)",
	  { 0x96, 0x84, 0x96, 0xe1, 0xbd, 0x86, 0x3a, 0x39, 0x9a, 0x0a, 0x2f, 0x13, 0xe8, 0x89, 0xed, 0xd8 }, 0x364fe,
	  { 0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
	    0x08, 0x02, 0x00, 0x00, 0x49, 0x89, 0xd6, 0x49, 0x89, 0xf7, 0x49, 0x89, 0xfc, 0x48, 0x89, 0x7d } },
};

static uint32_t gFloor = kDefaultFloor;
static bool gAllClients = false;
static int gLogs = 0;

static const char *kX6000Paths[] {
	"/System/Library/Extensions/AMDRadeonX6000HWServices.kext/Contents/PlugIns/AMDRadeonX6000HWLibs.kext/Contents/MacOS/AMDRadeonX6000HWLibs"
};
static const char *kX6800Paths[] {
	"/System/Library/Extensions/AMDRadeonX6000HWServices.kext/Contents/PlugIns/AMDRadeonX6800HWLibs.kext/Contents/MacOS/AMDRadeonX6800HWLibs"
};
static KernelPatcher::KextInfo kHWLibs[] {
	{ "com.apple.kext.AMDRadeonX6000HWLibs", kX6000Paths, arrsize(kX6000Paths), {}, {}, KernelPatcher::KextInfo::Unloaded },
	{ "com.apple.kext.AMDRadeonX6800HWLibs", kX6800Paths, arrsize(kX6800Paths), {}, {}, KernelPatcher::KextInfo::Unloaded },
};
// Per entry of kHWLibs: its short name and validated builds.
struct HWLibsKind {
	const char *name;
	const HWLibsBuild *builds;
	size_t count;
};
static const HWLibsKind kKinds[] {
	{ "AMDRadeonX6000HWLibs", kX6000Builds, arrsize(kX6000Builds) },
	{ "AMDRadeonX6800HWLibs", kX6800Builds, arrsize(kX6800Builds) },
};
static_assert(arrsize(kKinds) == arrsize(kHWLibs), "one kind per library");

static void publishState(const char *state) {
	IOService *res = IOService::getResourceService();
	if (res)
		res->setProperty("VCNEncodeFloor", state);
}

static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, sizeof(v)); return v; }
static inline void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof(v)); }

// PEM_CWDDEPM_PMRequest(PEM *, const payload *in, payload *out); returns 0 on success.
using t_pmRequest = int64_t (*)(void *pem, uint8_t *in, uint8_t *out);
// One original per library: a Mac with two Navi 21 cards could load both, and each route has its own trampoline.
static t_pmRequest orgPMRequest[arrsize(kHWLibs)] {};

static int64_t floorPMRequest(t_pmRequest org, void *pem, uint8_t *in, uint8_t *out) {
	if (in != nullptr) {
		uint32_t type = rd32(in + kOffClientType);
		uint32_t floor = rd32(in + kOffVclkFloor);
		uint32_t minMclk = rd32(in + kOffMinMclk);
		if ((type == kClientVcn0 || type == kClientVcn1) && floor == 0 && (minMclk != 0 || gAllClients)) {
			// Work on a copy so the caller's buffer is never changed.
			uint8_t copy[kPayloadSize];
			memcpy(copy, in, sizeof(copy));
			wr32(copy + kOffVclkFloor, gFloor);
			int64_t result = org(pem, copy, out);
			if (gLogs < kMaxLogs) {
				SYSLOG("vcnfloor", "client type %u minMclk %u: VclkFloor 0 -> %u, result %lld", type, minMclk, gFloor, result);
				gLogs++;
			}
			return result;
		}
	}
	return org(pem, in, out);
}

template <size_t K>
static int64_t wrapPMRequest(void *pem, uint8_t *in, uint8_t *out) {
	return floorPMRequest(orgPMRequest[K], pem, in, out);
}
static const t_pmRequest kWrappers[] { wrapPMRequest<0>, wrapPMRequest<1> };
static_assert(arrsize(kWrappers) == arrsize(kHWLibs), "one wrapper per library");

// Returns true when the Mach-O image at `address` has the LC_UUID `want`.
static bool imageHasUuid(mach_vm_address_t address, size_t size, const uint8_t want[16]) {
	if (address == 0 || size < sizeof(mach_header_64))
		return false;
	auto mh = reinterpret_cast<const mach_header_64 *>(address);
	if (mh->magic != MH_MAGIC_64 || mh->sizeofcmds > size - sizeof(mach_header_64))
		return false;
	auto p = reinterpret_cast<const uint8_t *>(mh + 1);
	auto end = p + mh->sizeofcmds;
	for (uint32_t i = 0; i < mh->ncmds && p + sizeof(load_command) <= end; i++) {
		auto lc = reinterpret_cast<const load_command *>(p);
		if (lc->cmdsize < sizeof(load_command) || p + lc->cmdsize > end)
			return false;
		if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(uuid_command))
			return memcmp(reinterpret_cast<const uuid_command *>(lc)->uuid, want, 16) == 0;
		p += lc->cmdsize;
	}
	return false;
}

static void onHWLibsLoad(void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	size_t k = 0;
	while (k < arrsize(kHWLibs) && kHWLibs[k].loadIndex != index)
		k++;
	if (k == arrsize(kHWLibs))
		return;
	const HWLibsKind &kind = kKinds[k];

	const HWLibsBuild *build = nullptr;
	for (size_t i = 0; i < kind.count; i++)
		if (imageHasUuid(address, size, kind.builds[i].uuid))
			build = &kind.builds[i];
	if (build == nullptr) {
		SYSLOG("vcnfloor", "%s is not a validated build, doing nothing", kind.name);
		publishState(k == 0 ? "off: AMDRadeonX6000HWLibs build not validated" : "off: AMDRadeonX6800HWLibs build not validated");
		return;
	}
	bool routed = false;
	if (build->offset == 0) {
		KernelPatcher::RouteRequest requests[] {
			{"_PEM_CWDDEPM_PMRequest", kWrappers[k], orgPMRequest[k]},
		};
		routed = patcher.routeMultiple(index, requests, address, size);
	} else {
		mach_vm_address_t from = address + build->offset;
		if (build->offset + sizeof(build->prologue) > size ||
		    memcmp(reinterpret_cast<const void *>(from), build->prologue, sizeof(build->prologue)) != 0) {
			SYSLOG("vcnfloor", "PEM_CWDDEPM_PMRequest not at the expected place, doing nothing");
			publishState("off: function not found");
			return;
		}
		// routeMultiple only takes symbols, so the stripped build uses the address-based call Lilu marks deprecated.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		auto wrapper = patcher.routeFunction(from, reinterpret_cast<mach_vm_address_t>(kWrappers[k]), true);
#pragma clang diagnostic pop
		if (patcher.getError() == KernelPatcher::Error::NoError && wrapper != 0) {
			orgPMRequest[k] = reinterpret_cast<t_pmRequest>(wrapper);
			routed = true;
		}
	}
	if (routed) {
		SYSLOG("vcnfloor", "routed %s on %s, VCN encode clock floor %u%s", kind.name, build->macos, gFloor,
		       gAllClients ? " (all VCN clients)" : "");
		publishState(gAllClients ? "active: encode and decode" : "active: encode");
	} else {
		SYSLOG("vcnfloor", "route failed (%d), driver left as it is", patcher.getError());
		patcher.clearError();
		publishState("off: route failed");
	}
}

static const char *bootargOff[]   { "-vcnflooroff" };
static const char *bootargDebug[] { "-vcnfloordbg" };
static const char *bootargBeta[]  { "-vcnfloorbeta" };

PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal,
	bootargOff,   arrsize(bootargOff),
	bootargDebug, arrsize(bootargDebug),
	bootargBeta,  arrsize(bootargBeta),
	KernelVersion::Sonoma,
	KernelVersion::Tahoe,
	[]() {
		uint32_t requested = 0;
		if (PE_parse_boot_argn("vcnfloor", &requested, sizeof(requested))) {
			if (requested != 0)
				gFloor = requested;
			else
				SYSLOG("vcnfloor", "ignoring vcnfloor=0, using %u", gFloor);
		}
		gAllClients = checkKernelArgument("-vcnfloorall");
		publishState("waiting for AMDRadeonX6000HWLibs or AMDRadeonX6800HWLibs");
		lilu.onKextLoadForce(kHWLibs, arrsize(kHWLibs), onHWLibsLoad, nullptr);
	}
};
