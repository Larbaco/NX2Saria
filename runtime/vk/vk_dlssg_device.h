#pragma once
#include <stdint.h>

#define DLSSG_VENDOR_NVIDIA 0x10deu

static inline int dlssg_card_skipped(int interposed, int chosen, uint32_t vendor) {
    return interposed && chosen && vendor != DLSSG_VENDOR_NVIDIA;
}

typedef int (*DlssgCreate)(void *user);

enum { DLSSG_DEV_LOADER, DLSSG_DEV_INTERPOSER, DLSSG_DEV_NOT_NVIDIA, DLSSG_DEV_REFUSED };

static inline int dlssg_device_create(int interposed, uint32_t vendor, DlssgCreate via_interposer, DlssgCreate via_loader,
                                      void *user, int *how, int *refused) {
    *refused = 0;
    if (!interposed) return *how = DLSSG_DEV_LOADER, via_loader(user);
    if (vendor != DLSSG_VENDOR_NVIDIA) return *how = DLSSG_DEV_NOT_NVIDIA, via_loader(user);
    int r = via_interposer(user);
    if (!r) return *how = DLSSG_DEV_INTERPOSER, 0;
    *refused = r;
    return *how = DLSSG_DEV_REFUSED, via_loader(user);
}
