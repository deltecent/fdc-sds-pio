/*
 * disk.h — disk-image mount table + whole-track I/O (DESIGN.md §6/§10, plan M3).
 *
 * The FDC engine (M4) asks for whole tracks by (drive, track, length); this module
 * owns the 4-drive mount table, the open image handles, the shared 8192-byte track
 * buffer + a 1-entry (drive,track,len) cache, and the offset arithmetic. All SD access
 * runs under a single mutex so the CLI/FTP paths and the fdc task never collide on the
 * card (DESIGN.md §5.2).
 *
 * Each mount goes through a small internal "backing" interface (size/read_at/write_at/
 * close). v1 implements only the LOCAL (SD file, opened `r+`) backing; the TNFS backing
 * for `tnfs://` mounts (DESIGN.md §9.5/§10.1) drops in behind the same interface at M9
 * with no change to the FDC path or these callers.
 */
#ifndef FDCSDS_DISK_H
#define FDCSDS_DISK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "config.h"   /* CONFIG_MAX_DRIVE, CONFIG_FILE_CAP */
#include "protocol.h" /* DISK_TRACK_BUF_SIZE + geometry helpers */

#ifdef __cplusplus
extern "C" {
#endif

#define DISK_MAX_DRIVE CONFIG_MAX_DRIVE /* drives 0..3 (DESIGN.md §1) */

/* Create the SD/track mutex. Call once at boot before disk_automount() (§12 step 5). */
esp_err_t disk_init(void);

/*
 * Mount image `name` (an SD-root filename in v1) on `drive` 0..DISK_MAX_DRIVE-1,
 * opening it read/write and remembering its size. Re-mounts if the drive was in use.
 * Returns ESP_ERR_INVALID_ARG (bad drive / name), ESP_ERR_INVALID_STATE (no SD),
 * ESP_ERR_NOT_FOUND (open failed), or ESP_ERR_NO_MEM.
 */
esp_err_t disk_mount(int drive, const char *name);

/* Unmount `drive` (closes its handle). No-op if the drive is not mounted. */
esp_err_t disk_unmount(int drive);

/* True if `drive` currently has an image mounted and ready to serve tracks. A remote
 * (tnfs://) drive is "mounted" only while its session is live; it reads false while
 * configured-but-not-ready (WiFi down / mount pending). */
bool disk_is_mounted(int drive);

/* True if `drive` is configured as a remote (tnfs://) image, whether or not its
 * session is currently up. Lets the CLI show a not-ready remote drive distinctly. */
bool disk_is_remote(int drive);

/* True if `drive` is mounted read-only (a remote image the server only allowed to be
 * opened O_RDONLY); WRIT to it fails cleanly. Always false for local SD mounts. */
bool disk_is_readonly(int drive);

/* Mounted/configured image name for `drive`: the SD filename or the tnfs:// URL (kept
 * for a not-ready remote drive so it can be re-mounted). Empty string if none. */
const char *disk_name(int drive);

/* Size in bytes of the image on `drive` (0 if not mounted). */
uint32_t disk_image_size(int drive);

/* STAT mount bitmap: bit n set = drive n mounted (DESIGN.md §6.3). */
uint16_t disk_status_bitmap(void);

/*
 * Read `len` bytes of `track` from `drive` into the shared track buffer (served by a
 * cache hit when the last read matched). `len` must be 1..DISK_TRACK_BUF_SIZE and the
 * whole track must fit inside the image. Returns ESP_ERR_INVALID_STATE if not mounted,
 * ESP_ERR_INVALID_SIZE if out of range/too big, ESP_FAIL on an I/O error.
 */
esp_err_t disk_read_track(int drive, uint32_t track, size_t len);

/*
 * Write `len` bytes of `data` to `track` on `drive`, then refresh the track buffer +
 * cache to match. Same validation/return contract as disk_read_track().
 */
esp_err_t disk_write_track(int drive, uint32_t track, size_t len, const void *data);

/*
 * Borrow the shared track buffer (what `dump` shows and the FDC READ path sends). On
 * return drive_out and track_out identify the cached track and len_out its length (0 if
 * nothing has been read yet). The pointer is valid until the next track I/O call.
 */
const uint8_t *disk_track_buffer(int *drive_out, uint32_t *track_out, size_t *len_out);

/*
 * Boot auto-mount (DESIGN.md §12 step 5): mount each drive whose config Drive<n> is a
 * plain SD filename. A `tnfs://` value is only *registered* here (the slot shows as a
 * not-ready remote drive); the actual session is opened when WiFi connects via
 * disk_remote_mount_all() (DESIGN.md §10.1/§12 step 9). Missing images are logged.
 */
void disk_automount(void);

/*
 * WiFi-up hook (DESIGN.md §12 step 9): open a TNFS session for every configured remote
 * drive that is not already mounted. Non-blocking — the sessions are established on the
 * disk-I/O worker task (core 0, off the fdc path, §5.2/§10.1); each drive flips to ready
 * as its mount completes. Safe to call again on every reconnect.
 */
void disk_remote_mount_all(void);

/*
 * WiFi-down hook (DESIGN.md §12 step 9): drop every remote drive to not-ready (close its
 * session) while keeping it configured so disk_remote_mount_all() re-mounts it on
 * reconnect. Non-blocking (serviced on the worker task). Local drives are untouched.
 */
void disk_remote_unmount_all(void);

#ifdef __cplusplus
}
#endif

#endif /* FDCSDS_DISK_H */
