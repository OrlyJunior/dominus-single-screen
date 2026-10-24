#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct DominusStatus {
    uint32_t size;
    uint32_t abi_version;
    uint32_t supported_build;
    uint32_t synthetic_host;
    uint32_t observer_enabled;
    uint32_t native_pause_validated;
    uint32_t input_path_validated;
    uint32_t context_validated;
    uint64_t presents;
    uint64_t compositions;
    uint64_t fallbacks;
    uint64_t resizes;
} DominusStatus;
typedef int (__cdecl *DominusGetStatusFn)(DominusStatus*);
// Test-only API: refused unless the caller is the separately built synthetic host.
typedef int (__cdecl *DominusSyntheticConfigureFn)(unsigned, float, float, float, float, int);
typedef int (__cdecl *DominusSyntheticShutdownFn)(void);
#ifdef __cplusplus
}
#endif
