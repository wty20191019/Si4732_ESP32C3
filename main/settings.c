#include "settings.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "SETTINGS";

#define KEY_BAND    "band"
#define KEY_FREQ    "freq"
#define KEY_VOLUME  "vol"
#define KEY_MUTE    "mute"

#define SETTINGS_DEFAULT_BAND       0           /* FM */
#define SETTINGS_DEFAULT_FREQ_KHZ   101100      /* 101.1 MHz */
#define SETTINGS_DEFAULT_VOLUME     30

void settings_defaults(radio_settings_t *s)
{
    s->band = SETTINGS_DEFAULT_BAND;
    s->freq_khz = SETTINGS_DEFAULT_FREQ_KHZ;
    s->volume = SETTINGS_DEFAULT_VOLUME;
    s->mute = false;
}

bool settings_load(radio_settings_t *s)
{
    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    uint8_t band = s->band;
    uint32_t freq = s->freq_khz;
    uint8_t volume = s->volume;
    uint8_t mute = s->mute ? 1 : 0;
    bool found = false;

    if (nvs_get_u8(handle, KEY_BAND, &band) == ESP_OK) {
        found = true;
    }
    if (nvs_get_u32(handle, KEY_FREQ, &freq) == ESP_OK) {
        found = true;
    }
    if (nvs_get_u8(handle, KEY_VOLUME, &volume) == ESP_OK) {
        found = true;
    }
    if (nvs_get_u8(handle, KEY_MUTE, &mute) == ESP_OK) {
        found = true;
    }

    nvs_close(handle);

    /* 简单合法性检查，避免 NVS 里的旧数据把芯片设到非法范围 */
    if (band >= 4) {
        band = SETTINGS_DEFAULT_BAND;
    }
    if (volume > 63) {
        volume = SETTINGS_DEFAULT_VOLUME;
    }
    if (freq == 0) {
        freq = SETTINGS_DEFAULT_FREQ_KHZ;
    }

    s->band = band;
    s->freq_khz = freq;
    s->volume = volume;
    s->mute = (mute != 0);

    if (found) {
        ESP_LOGI(TAG, "读取设置: band=%u freq=%lukHz vol=%u mute=%d",
                 (unsigned)s->band, (unsigned long)s->freq_khz,
                 (unsigned)s->volume, (int)s->mute);
    }
    return found;
}

bool settings_save(const radio_settings_t *s)
{
    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    esp_err_t err = nvs_set_u8(handle, KEY_BAND, s->band);
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, KEY_FREQ, s->freq_khz);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_VOLUME, s->volume);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_MUTE, s->mute ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err == ESP_OK;
}
