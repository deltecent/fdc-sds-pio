/*
 * ota.h — firmware update (OTA), DESIGN.md §11 / §11.1.
 *
 * Three sources, all sharing one esp_ota writer + set-boot + reboot backend:
 *   - SD    `/sd/firmware.bin`  (primary, offline)        -> ota_update_sd()
 *   - repo  version.txt check + release binary          -> ota_update_repo()  (`update ota`)
 *   - url   arbitrary https:// (tnfs:// once M9 lands)   -> ota_update_url()   (undocumented)
 *
 * The network paths pull raw files straight from the configured GitHub repo
 * (`otaRepo`, §7) at branch `master`:
 *   https://raw.githubusercontent.com/<otaRepo>/master/ota/version.txt
 *   https://raw.githubusercontent.com/<otaRepo>/master/ota/firmware.bin
 * `update ota` fetches version.txt, compares it to the running version, and only
 * downloads the binary when it is newer.
 *
 * Each update runs on its own core-0 task — TLS needs far more stack than the CLI
 * task carries — and streams progress to the invoking console. On success the device
 * reboots into the freshly written slot, so these calls return only on failure or when
 * there is nothing to do (repo already current). One update runs at a time.
 */
#ifndef FDCSDS_OTA_H
#define FDCSDS_OTA_H

#include "esp_err.h"

#include "cli.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bare `update`: report the running version, any installable SD `/sd/firmware.bin`
 * (with its embedded version), and — only when WiFi is up — the version the repo offers.
 * Installs nothing (DESIGN.md §11). */
esp_err_t ota_status(cli_console_t *c);

/* `update local`: flash `/sd/firmware.bin` into the inactive slot, delete it, reboot
 * (DESIGN.md §11). */
esp_err_t ota_update_sd(cli_console_t *c);

/* Network OTA from the configured repo: compare version.txt to the running version
 * and, when newer, stream the release binary in (DESIGN.md §11.1, `update ota`). */
esp_err_t ota_update_repo(cli_console_t *c);

/* Network OTA from an explicit URL — https:// now, tnfs:// once the M9 client lands
 * (the undocumented `update <url>` form). */
esp_err_t ota_update_url(cli_console_t *c, const char *url);

#ifdef __cplusplus
}
#endif

#endif /* FDCSDS_OTA_H */
