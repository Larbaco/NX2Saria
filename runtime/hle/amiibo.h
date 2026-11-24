#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AMIIBO_DUMP_SIZE 540
#define AMIIBO_APP_AREA_SIZE 0xD8

typedef enum {
    AMIIBO_DEV_STATE_INITIALIZED = 0,
    AMIIBO_DEV_STATE_SEARCHING   = 1,
    AMIIBO_DEV_STATE_TAG_FOUND   = 2,
    AMIIBO_DEV_STATE_TAG_REMOVED = 3,
    AMIIBO_DEV_STATE_TAG_MOUNTED = 4,
    AMIIBO_DEV_STATE_UNAVAILABLE = 5
} AmiiboDeviceState;

typedef struct {
    uint8_t uuid[10];
    uint8_t uuid_len;
    uint8_t character_id[3];
    uint8_t series_id;
    uint16_t numbering_id;
    uint8_t nfp_type;
    uint8_t version;
    char path[512];
    char filename[128];
} AmiiboInfo;

void amiibo_init(void);

void amiibo_set_device_present(bool present);
bool amiibo_is_device_present(void);

void amiibo_start_detection(void);
void amiibo_stop_detection(void);
bool amiibo_is_detecting(void);

bool amiibo_mount(void);
void amiibo_unmount(void);
bool amiibo_is_mounted(void);

AmiiboDeviceState amiibo_get_device_state(void);
AmiiboDeviceState amiibo_get_reader_state(void);

bool amiibo_load_file(const char *path);
bool amiibo_scan_and_tap(const char *path);
void amiibo_tap_tag(void);
void amiibo_remove_tag(void);

void amiibo_tick(void);

void amiibo_set_auto_remove(bool enable);
bool amiibo_get_auto_remove(void);
void amiibo_set_auto_remove_delay_s(float seconds);
float amiibo_get_auto_remove_delay_s(void);

bool amiibo_has_tag(void);
const AmiiboInfo *amiibo_get_current_info(void);
const uint8_t *amiibo_get_raw_data(void);

void amiibo_get_tag_info(void *out_tag_info);
void amiibo_get_model_info(void *out_model_info);
void amiibo_get_common_info(void *out_common_info);
void amiibo_get_register_info(void *out_register_info);
int amiibo_get_application_area(void *out_buf, uint32_t max_size);

typedef enum {
    AMIIBO_SOURCE_NEW = 0,
    AMIIBO_SOURCE_SAVED = 1,
    AMIIBO_SOURCE_DUMP = 2
} AmiiboDataSource;

typedef struct {
    AmiiboDataSource source;
    bool decrypted;
    bool registered;
    char nickname[41];
    uint16_t write_counter;
    uint16_t setup_year;
    uint8_t setup_month, setup_day;
    uint16_t write_year;
    uint8_t write_month, write_day;
    bool has_app_area;
    uint32_t access_id;
    uint32_t app_area_size;
    uint8_t app_area[AMIIBO_APP_AREA_SIZE];
} AmiiboFigureData;

bool amiibo_get_figure_data(AmiiboFigureData *out);
bool amiibo_erase_app_area(void);
bool amiibo_reload_keys(void);

#define AMIIBO_R_DEVICE_NOT_FOUND 0x8073u
#define AMIIBO_R_AREA_NOT_CREATED 0x10073u
#define AMIIBO_R_WRONG_ACCESS_ID 0x13073u
#define AMIIBO_R_AREA_EXISTS 0x15073u

uint32_t amiibo_open_app_area(uint32_t access_id);
uint32_t amiibo_get_app_area(void *out, uint64_t cap, uint64_t *out_size);
uint32_t amiibo_set_app_area(const void *data, uint64_t size);
uint32_t amiibo_create_app_area(uint32_t access_id, const void *data, uint64_t size, bool recreate);
uint32_t amiibo_flush_figure(void);

#ifdef __cplusplus
}
#endif
