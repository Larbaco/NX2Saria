#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t *menu_asset_load(const char *rel, size_t *n);
uint8_t *menu_asset_font(const char *rel, size_t *n);
int menu_asset_texture(const uint8_t *bntx, size_t n, const char *name, uint8_t **rgba, uint32_t *w, uint32_t *h);

#ifdef __cplusplus
}
#endif
