// Dump constants from the loaded AMD VA driver at IDA (unslid) addresses.
// usage: dumpconst <ida_addr_hex> <bytes> [...]
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "avd.h"
#define IDA_AVD_CREATE 0x7ffb10afe9e0ull
enum { kMaxDumpBytes = 4096 };
int main(int argc, char** argv) {
  void* lib = dlopen(AVD_DRIVER_PATH, RTLD_NOW);
  if (!lib) {
    fprintf(stderr, "dlopen: %s\n", dlerror());
    return 1;
  }
  void* sym = dlsym(lib, "AVD_CreateAVDAccelerator");
  if (!sym) {
    fprintf(stderr, "no AVD_CreateAVDAccelerator\n");
    return 1;
  }
  uintptr_t live = (uintptr_t)sym;
  intptr_t slide = (intptr_t)live - (intptr_t)IDA_AVD_CREATE;
  for (int i = 1; i + 1 < argc; i += 2) {
    errno = 0;
    char* end = NULL;
    unsigned long long a = strtoull(argv[i], &end, 16);
    if (!end || end == argv[i] || *end || errno == ERANGE) {
      fprintf(stderr, "invalid address %s\n", argv[i]);
      return 1;
    }
    end = NULL;
    errno = 0;
    unsigned long n = strtoul(argv[i + 1], &end, 10);
    if (!end || end == argv[i + 1] || *end || errno == ERANGE || n == 0 ||
        n > kMaxDumpBytes) {
      fprintf(stderr, "invalid length %s (1..%d)\n", argv[i + 1], kMaxDumpBytes);
      return 1;
    }
    uint8_t* p = (uint8_t*)(a + (unsigned long long)slide);
    printf("%s:", argv[i]);
    for (unsigned long k = 0; k < n; k++) printf(" %02x", p[k]);
    printf("\n");
  }
}
