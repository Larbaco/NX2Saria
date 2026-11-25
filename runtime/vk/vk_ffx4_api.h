#pragma once
#include <stdint.h>

#include "third_party/ffx_sdk/api/include/ffx_api.h"
#include "third_party/ffx_sdk/api/include/vk/ffx_api_vk.h"

extern "C" {
int vk_ffx4_api_versions(uint64_t create_type, uint64_t *ids, const char **names, int cap);
int vk_ffx4_api_file(const char *name);
ffxReturnCode_t vk_ffx4_api_create(ffxContext *ctx, ffxCreateContextDescHeader *desc);
ffxReturnCode_t vk_ffx4_api_destroy(ffxContext *ctx);
ffxReturnCode_t vk_ffx4_api_configure(ffxContext *ctx, const ffxConfigureDescHeader *desc);
ffxReturnCode_t vk_ffx4_api_query(ffxContext *ctx, ffxQueryDescHeader *desc);
ffxReturnCode_t vk_ffx4_api_dispatch(ffxContext *ctx, const ffxDispatchDescHeader *desc);
}
