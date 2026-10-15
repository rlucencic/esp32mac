#include "esp_log.h"
#include "nvs.h"
#include "settings.h"

#define NAMESPACE   "mac"

static const char *TAG = "settings";

settings_t settings;

void settings_load(void)
{
    settings.model = MAC_MODEL_PLUS;
    settings.ram_kb = 4096;

    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t u8;
    uint16_t u16;
    if (nvs_get_u8(h, "model", &u8) == ESP_OK && u8 <= MAC_MODEL_CLASSIC) {
        settings.model = u8;
    }
    if (nvs_get_u16(h, "ram_kb", &u16) == ESP_OK && (u16 == 1024 || u16 == 2048 || u16 == 4096)) {
        settings.ram_kb = u16;
    }
    nvs_close(h);
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "can't open NVS");
        return;
    }
    nvs_set_u8(h, "model", settings.model);
    nvs_set_u16(h, "ram_kb", settings.ram_kb);
    nvs_commit(h);
    nvs_close(h);
}

bool settings_load_pram(uint8_t *pram)
{
    nvs_handle_t h;
    size_t size = MAC_PRAM_SIZE;
    bool ok = false;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_blob(h, "pram", pram, &size) == ESP_OK && size == MAC_PRAM_SIZE;
        nvs_close(h);
    }
    return ok;
}

void settings_save_pram(const uint8_t *pram)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "pram", pram, MAC_PRAM_SIZE);
        nvs_commit(h);
        nvs_close(h);
    }
}
