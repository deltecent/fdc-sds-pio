/*
 * config.c — NVS-backed configuration store (DESIGN.md §7).
 *
 * One static config_t is the live copy. config_init() seeds it with defaults and
 * overlays whatever the `fdcsds` NVS namespace holds; setters mutate the copy and
 * raise a dirty flag; save/wipe are the only functions that touch flash. Keeping
 * flash writes to save/wipe (never per-keystroke) matches the "edits are in-memory
 * until an explicit save" rule and spares the NVS wear budget.
 */
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "config.h"

static const char *TAG = "config";

#define NVS_NS "fdcsds"

/* NVS keys — mirror the DESIGN.md §7 table exactly (all ≤15 chars, NVS's limit). */
#define KEY_BAUD    "baudRate"
#define KEY_WIFI_EN "wifiEnabled"
#define KEY_SSID    "wifiSSID"
#define KEY_PASS    "wifiPass"
#define KEY_NAME    "wifiName"
#define KEY_TZ      "timeZone"
#define KEY_FTPUSER "ftpUser"
#define KEY_FTPPASS "ftpPass"
#define KEY_OTAREPO "otaRepo"
static const char *KEY_DRIVE[CONFIG_MAX_DRIVE] = { "Drive0", "Drive1", "Drive2", "Drive3" };

static config_t s_cfg;
static bool     s_dirty;
static bool     s_loaded;

/* Reset the in-memory copy to the compiled-in defaults (DESIGN.md §7). */
static void set_defaults(void)
{
    memset(&s_cfg, 0, sizeof s_cfg);
    s_cfg.baud_rate = 403200;
    s_cfg.wifi_enabled = false;
    /* ssid/pass empty */
    strcpy(s_cfg.wifi_name, "FDC-SDS-ESP32");
    strcpy(s_cfg.time_zone, "UTC0");
    strcpy(s_cfg.ftp_user, "fdc");
    strcpy(s_cfg.ftp_pass, "fdc");
    /* Network OTA source repo (DESIGN.md §7/§11.1). The locked CLI set has no command
     * to set this, so it defaults to the project's own repo — `update ota` then works
     * out of the box, pulling ota/version.txt + ota/firmware.bin from its master. */
    strcpy(s_cfg.ota_repo, "deltecent/fdc-sds-pio");
    /* drives empty */
}

/* Load one string key into dst (leaving the default in place if absent). */
static void load_str(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    esp_err_t err = nvs_get_str(h, key, dst, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "read %s: %s", key, esp_err_to_name(err));
    }
}

/* Overlay any stored values onto the current (default-seeded) copy. */
static void load_from_nvs(nvs_handle_t h)
{
    uint32_t u32;
    if (nvs_get_u32(h, KEY_BAUD, &u32) == ESP_OK) {
        s_cfg.baud_rate = u32;
    }
    uint8_t u8;
    if (nvs_get_u8(h, KEY_WIFI_EN, &u8) == ESP_OK) {
        s_cfg.wifi_enabled = (u8 != 0);
    }
    load_str(h, KEY_SSID, s_cfg.wifi_ssid, sizeof s_cfg.wifi_ssid);
    load_str(h, KEY_PASS, s_cfg.wifi_pass, sizeof s_cfg.wifi_pass);
    load_str(h, KEY_NAME, s_cfg.wifi_name, sizeof s_cfg.wifi_name);
    load_str(h, KEY_TZ, s_cfg.time_zone, sizeof s_cfg.time_zone);
    load_str(h, KEY_FTPUSER, s_cfg.ftp_user, sizeof s_cfg.ftp_user);
    load_str(h, KEY_FTPPASS, s_cfg.ftp_pass, sizeof s_cfg.ftp_pass);
    load_str(h, KEY_OTAREPO, s_cfg.ota_repo, sizeof s_cfg.ota_repo);
    for (int i = 0; i < CONFIG_MAX_DRIVE; ++i) {
        load_str(h, KEY_DRIVE[i], s_cfg.drive[i], sizeof s_cfg.drive[i]);
    }
}

esp_err_t config_init(void)
{
    set_defaults();

    /* Bring up the default NVS partition; recover if it's full or a new format. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "erasing NVS (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
        return err;
    }

    nvs_handle_t h;
    err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no saved config; using defaults");
        s_loaded = true;
        s_dirty = false;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }

    load_from_nvs(h);
    nvs_close(h);

    s_loaded = true;
    s_dirty = false;
    ESP_LOGI(TAG, "config loaded (host '%s', baud %lu)",
             s_cfg.wifi_name, (unsigned long)s_cfg.baud_rate);
    return ESP_OK;
}

const config_t *config_get(void)
{
    return &s_cfg;
}

bool config_dirty(void)
{
    return s_dirty;
}

esp_err_t config_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open (rw): %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u32(h, KEY_BAUD, s_cfg.baud_rate);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_WIFI_EN, s_cfg.wifi_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_SSID, s_cfg.wifi_ssid);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_PASS, s_cfg.wifi_pass);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_NAME, s_cfg.wifi_name);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_TZ, s_cfg.time_zone);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_FTPUSER, s_cfg.ftp_user);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_FTPPASS, s_cfg.ftp_pass);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_OTAREPO, s_cfg.ota_repo);
    for (int i = 0; i < CONFIG_MAX_DRIVE && err == ESP_OK; ++i) {
        err = nvs_set_str(h, KEY_DRIVE[i], s_cfg.drive[i]);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: %s", esp_err_to_name(err));
        return err;
    }
    s_dirty = false;
    return ESP_OK;
}

esp_err_t config_wipe(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK; /* nothing stored yet — defaults already apply */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wipe: %s", esp_err_to_name(err));
        return err;
    }

    set_defaults();
    s_dirty = false;
    return ESP_OK;
}

/* ---- setters (mutate RAM + mark dirty) ------------------------------------- */

void config_set_baud(uint32_t baud)
{
    if (s_cfg.baud_rate != baud) {
        s_cfg.baud_rate = baud;
        s_dirty = true;
    }
}

void config_set_wifi_enabled(bool enabled)
{
    if (s_cfg.wifi_enabled != enabled) {
        s_cfg.wifi_enabled = enabled;
        s_dirty = true;
    }
}

/* Return the destination buffer + capacity for a string id, or NULL/0. */
static char *str_field(config_str_id_t id, size_t *cap)
{
    switch (id) {
    case CFG_STR_WIFI_SSID: *cap = sizeof s_cfg.wifi_ssid; return s_cfg.wifi_ssid;
    case CFG_STR_WIFI_PASS: *cap = sizeof s_cfg.wifi_pass; return s_cfg.wifi_pass;
    case CFG_STR_WIFI_NAME: *cap = sizeof s_cfg.wifi_name; return s_cfg.wifi_name;
    case CFG_STR_TIME_ZONE: *cap = sizeof s_cfg.time_zone; return s_cfg.time_zone;
    case CFG_STR_FTP_USER:  *cap = sizeof s_cfg.ftp_user;  return s_cfg.ftp_user;
    case CFG_STR_FTP_PASS:  *cap = sizeof s_cfg.ftp_pass;  return s_cfg.ftp_pass;
    case CFG_STR_OTA_REPO:  *cap = sizeof s_cfg.ota_repo;  return s_cfg.ota_repo;
    }
    *cap = 0;
    return NULL;
}

void config_set_str(config_str_id_t id, const char *val)
{
    size_t cap;
    char *dst = str_field(id, &cap);
    if (!dst) {
        return;
    }
    if (!val) {
        val = "";
    }
    if (strncmp(dst, val, cap) != 0) {
        strlcpy(dst, val, cap);
        s_dirty = true;
    }
}

void config_set_drive(int n, const char *filename)
{
    if (n < 0 || n >= CONFIG_MAX_DRIVE) {
        return;
    }
    if (!filename) {
        filename = "";
    }
    if (strncmp(s_cfg.drive[n], filename, sizeof s_cfg.drive[n]) != 0) {
        strlcpy(s_cfg.drive[n], filename, sizeof s_cfg.drive[n]);
        s_dirty = true;
    }
}

void config_prompt(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }
    snprintf(buf, len, "%s%s> ", s_dirty ? "* " : "", s_cfg.wifi_name);
}
