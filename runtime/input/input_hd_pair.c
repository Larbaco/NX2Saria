#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "input_hd_pair.h"
#include "platform/platform.h"

enum { VID_NINTENDO = 0x057E, PID_JOYCON_LEFT = 0x2006, PID_JOYCON_RIGHT = 0x2007, BLUETOOTH_REPORT = 49 };

static PlatMutex path_lock = PLAT_MUTEX_INIT;
static char *path[2];
static PlatMutex dev_lock = PLAT_MUTEX_INIT;
static SDL_hid_device *dev[2];
static unsigned number[2];
static volatile int32_t ended;

int hd_pair_find(void) {
    char *found[2] = {NULL, NULL};
    SDL_hid_device_info *list = SDL_hid_enumerate(VID_NINTENDO, 0);
    for (SDL_hid_device_info *d = list; d; d = d->next) {
        int s = d->product_id == PID_JOYCON_LEFT ? 0 : d->product_id == PID_JOYCON_RIGHT ? 1 : -1;
        if (s >= 0 && !found[s] && d->path) found[s] = strdup(d->path);
    }
    SDL_hid_free_enumeration(list);
    int both = found[0] && found[1];
    plat_mutex_lock(&path_lock);
    for (int s = 0; s < 2; s++) {
        free(path[s]);
        path[s] = both ? found[s] : NULL;
        if (!both) free(found[s]);
    }
    plat_mutex_unlock(&path_lock);
    return both;
}

static void close_devices(void) {
    for (int s = 0; s < 2; s++) {
        if (dev[s]) SDL_hid_close(dev[s]);
        dev[s] = NULL;
    }
}

static int write_side(int s, const uint8_t v[RUMBLE_HD_SIDE]) {
    uint8_t data[RUMBLE_HD_DATA], report[BLUETOOTH_REPORT];
    memcpy(data, v, RUMBLE_HD_SIDE);
    memcpy(data + RUMBLE_HD_SIDE, v, RUMBLE_HD_SIDE);
    rumble_hd_report(data, number[s]++, report, sizeof report);
    return SDL_hid_write(dev[s], report, sizeof report) == (int)sizeof report;
}

int hd_pair_write(const uint8_t data[RUMBLE_HD_DATA]) {
    int ok = 1;
    plat_mutex_lock(&dev_lock);
    if (__atomic_load_n(&ended, __ATOMIC_ACQUIRE)) {
        plat_mutex_unlock(&dev_lock);
        SDL_SetError("HD rumble has ended");
        return 0;
    }
    if (!dev[0] || !dev[1]) {
        close_devices();
        char *p[2];
        plat_mutex_lock(&path_lock);
        for (int s = 0; s < 2; s++) p[s] = path[s] ? strdup(path[s]) : NULL;
        plat_mutex_unlock(&path_lock);
        if (!p[0] || !p[1]) ok = 0, SDL_SetError("the two Joy-Con are not both listed as HID devices");
        for (int s = 0; s < 2 && ok; s++) {
            dev[s] = SDL_hid_open_path(p[s]);
            number[s] = 0;
            if (!dev[s]) ok = 0;
        }
        free(p[0]);
        free(p[1]);
    }
    for (int s = 0; s < 2 && ok; s++) ok = write_side(s, data + s * RUMBLE_HD_SIDE);
    if (!ok) close_devices();
    plat_mutex_unlock(&dev_lock);
    return ok;
}

int hd_pair_is_open(void) {
    plat_mutex_lock(&dev_lock);
    int open = dev[0] && dev[1];
    plat_mutex_unlock(&dev_lock);
    return open;
}

void hd_pair_close(int neutral, unsigned wait_ms) {
    if (!wait_ms) plat_mutex_lock(&dev_lock);
    else {
        int got = 0;
        for (unsigned i = 0; i < wait_ms && !(got = plat_mutex_trylock(&dev_lock)); i++) plat_wait_ns(1000000);
        if (!got) return;
    }
    for (int s = 0; s < 2 && neutral; s++)
        if (dev[s]) write_side(s, rumble_hd_neutral);
    close_devices();
    plat_mutex_unlock(&dev_lock);
}

void hd_pair_end(unsigned wait_ms) {
    __atomic_store_n(&ended, 1, __ATOMIC_RELEASE);
    hd_pair_close(1, wait_ms);
}
