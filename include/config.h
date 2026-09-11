/*
 * config.h — persistent configuration in NVS (DESIGN.md §7).
 *
 * v1 starts clean: namespace `fdcsds`, its own key set (no migration from the old
 * Arduino device). One in-memory copy is the source of truth; edits stay in RAM
 * and are marked DIRTY until an explicit `save`, which is when they reach NVS.
 * `wipe` erases the namespace and reloads the compiled-in defaults.
 *
 * Writes are confined to the CLI task for now (M2); when the fdc/net tasks begin
 * mutating config (M3+) the setters here are the single choke point to guard.
 */
#ifndef FDCSDS_CONFIG_H
#define FDCSDS_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_MAX_DRIVE 4 /* drives 0..3 (DESIGN.md §1) */

/* Field capacities include room for the NUL (DESIGN.md §7 lists the char limits). */
#define CONFIG_SSID_CAP 81  /* wifiSSID  ≤80 */
#define CONFIG_PASS_CAP 81  /* wifiPass  ≤80 */
#define CONFIG_NAME_CAP 41  /* wifiName  ≤40 */
#define CONFIG_TZ_CAP   41  /* timeZone  ≤40 */
#define CONFIG_FTP_CAP  33  /* ftpUser/ftpPass ≤32 */
#define CONFIG_REPO_CAP 65  /* otaRepo   ≤64 */
#define CONFIG_FILE_CAP 129 /* Drive<n>: SD filename or tnfs:// URL ≤128 (DESIGN.md §7/§10.1) */

/* In-memory configuration (mirrors the NVS schema in DESIGN.md §7). */
typedef struct {
    uint32_t baud_rate;                       /* FDC+ baud */
    uint8_t  log_level;                       /* console log verbosity, esp_log_level_t (§13) */
    bool     wifi_enabled;                    /* WiFi on/off */
    bool     telnet_enabled;                  /* Telnet console (:23) on/off (§9.2) */
    bool     ftp_enabled;                     /* FTP server (:21) on/off (§9.4) */
    bool     tnfsd_enabled;                   /* TNFS server (:16384) on/off (§9.6) */
    char     wifi_ssid[CONFIG_SSID_CAP];      /* SSID */
    char     wifi_pass[CONFIG_PASS_CAP];      /* password */
    char     wifi_name[CONFIG_NAME_CAP];      /* hostname / device name / prompt */
    char     time_zone[CONFIG_TZ_CAP];        /* POSIX TZ string */
    char     ftp_user[CONFIG_FTP_CAP];        /* FTP username */
    char     ftp_pass[CONFIG_FTP_CAP];        /* FTP password */
    char     ota_repo[CONFIG_REPO_CAP];       /* GitHub owner/repo for network OTA */
    char     drive[CONFIG_MAX_DRIVE][CONFIG_FILE_CAP]; /* mounted image filenames */
} config_t;

/* String fields addressable by the generic setter (keeps setters to one per shape). */
typedef enum {
    CFG_STR_WIFI_SSID,
    CFG_STR_WIFI_PASS,
    CFG_STR_WIFI_NAME,
    CFG_STR_TIME_ZONE,
    CFG_STR_FTP_USER,
    CFG_STR_FTP_PASS,
    CFG_STR_OTA_REPO,
} config_str_id_t;

/*
 * Bring up NVS and load the config (DESIGN.md §12 step 4). Missing keys fall back
 * to defaults; a corrupt/incompatible NVS partition is erased and re-created.
 * Safe to read via config_get() even before this runs (returns defaults).
 */
esp_err_t config_init(void);

/* Read-only view of the live config. Never NULL. */
const config_t *config_get(void);

/* True if there are unsaved changes (drives the leading "* " in the prompt). */
bool config_dirty(void);

/* Persist the whole config to NVS and clear the dirty flag. */
esp_err_t config_save(void);

/* Erase the namespace, reload defaults into RAM, clear the dirty flag. */
esp_err_t config_wipe(void);

/* Setters copy the value in and mark the config dirty. */
void config_set_baud(uint32_t baud);
void config_set_wifi_enabled(bool enabled);
void config_set_telnet_enabled(bool enabled);
void config_set_ftp_enabled(bool enabled);
void config_set_tnfsd_enabled(bool enabled);
void config_set_log_level(uint8_t level);  /* esp_log_level_t (§13) */
void config_set_str(config_str_id_t id, const char *val);

/* Set (filename) or clear (NULL/"") the image mounted on drive n (0..3). */
void config_set_drive(int n, const char *filename);

/* Build the CLI prompt ("[* ]<wifiName>> ") into buf (DESIGN.md §7/§8). */
void config_prompt(char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* FDCSDS_CONFIG_H */
