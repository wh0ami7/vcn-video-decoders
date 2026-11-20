# Decoder Core

`vcnav1` and `vcnvp9` run on the CPU and produce VCN decode messages. `vtav1` and `vtvp9` adapt those cores to VideoToolbox decoder bundles and public-API test drivers. AV1 uses capabilities already present in AMDRadeonVADriver2; VP9 message layouts are implemented locally following Mesa `ac_vcn_dec.h`.

- Parser state survives across frames; reference-slot updates belong only after the corresponding hardware decode.
- `vtav1/build.sh` compiles its plug-in directly with `vcnav1.c`, `av1parse.c` and `vcncommon/vcndriver.c`; `vtvp9/build.sh` does the same with the VP9 core. `vcncommon/` holds the driver interface (`avd.h`) and the code both cores share; only an accelerator whose `IOGVACodec` starts with "AMD" is used. Test-tool builds share these sources, so changes affect both direct and VideoToolbox paths.
- Firmware structures/default tables are ABI data, not ordinary editable configuration. Keep their layout and provenance intact.
- Functional verification uses external AOM AV1 vectors and FFmpeg FATE VP9 references. Browser videos and Apple driver binaries are intentionally absent.
- AV1 instance-0 pinning, validated driver layouts, GPU opt-ins, malformed-input handling and build hardening are mandatory; details are in `mem:security`.
