/*
 * disk.c — disk-image mount table + whole-track I/O (DESIGN.md §6/§10, plan M3).
 *
 * Owns the 4-drive mount table, the shared 8192-byte track buffer and its 1-entry
 * (drive,track,len) cache, and the track-offset arithmetic (all 32-bit — the old
 * firmware's signed-int bug is called out in DESIGN.md §6.5/§15). A single mutex
 * serializes every SD touch so the CLI/FTP paths and the fdc task (M4) never collide
 * on the card (DESIGN.md §5.2).
 *
 * Each mount is opened through a small "backing" interface so the FDC path is unaware
 * whether bytes come from an SD file or (at M9) a TNFS server. v1 ships only the local
 * SD-file backing; the remote one drops in behind the same vtable (DESIGN.md §10.1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "disk.h"
#include "sd.h"

static const char *TAG = "disk";

/* URL scheme that marks a remote (TNFS) mount, deferred to M9 (DESIGN.md §10.1). */
#define TNFS_SCHEME "tnfs://"

/* ---- backing interface (local SD file today; TNFS at M9) ------------------- */

typedef struct disk_backing disk_backing_t;
struct disk_backing {
    esp_err_t (*read_at)(disk_backing_t *b, uint32_t off, void *buf, size_t len);
    esp_err_t (*write_at)(disk_backing_t *b, uint32_t off, const void *buf, size_t len);
    uint32_t  (*size)(disk_backing_t *b);
    void      (*close)(disk_backing_t *b);
};

/* Local backing: a FATFS file opened r+ under /sd (base MUST be the first member). */
typedef struct {
    disk_backing_t base;
    FILE          *fp;
    uint32_t       size;
} local_backing_t;

static esp_err_t local_read_at(disk_backing_t *b, uint32_t off, void *buf, size_t len)
{
    local_backing_t *lb = (local_backing_t *)b;
    if (fseek(lb->fp, (long)off, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    return fread(buf, 1, len, lb->fp) == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t local_write_at(disk_backing_t *b, uint32_t off, const void *buf, size_t len)
{
    local_backing_t *lb = (local_backing_t *)b;
    if (fseek(lb->fp, (long)off, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    if (fwrite(buf, 1, len, lb->fp) != len) {
        return ESP_FAIL;
    }
    return fflush(lb->fp) == 0 ? ESP_OK : ESP_FAIL;
}

static uint32_t local_size(disk_backing_t *b)
{
    return ((local_backing_t *)b)->size;
}

static void local_close(disk_backing_t *b)
{
    local_backing_t *lb = (local_backing_t *)b;
    if (lb->fp) {
        fclose(lb->fp);
    }
    free(lb);
}

/* Open an SD-root file as a local backing. Returns NOT_FOUND if it can't be opened. */
static esp_err_t local_open(const char *name, disk_backing_t **out)
{
    char path[16 + CONFIG_FILE_CAP];
    if (sd_path(name, path, sizeof path) < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *fp = fopen(path, "r+b"); /* r+ = read/write, must already exist (§10) */
    if (!fp) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return ESP_FAIL;
    }

    local_backing_t *lb = calloc(1, sizeof *lb);
    if (!lb) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }
    lb->base.read_at = local_read_at;
    lb->base.write_at = local_write_at;
    lb->base.size = local_size;
    lb->base.close = local_close;
    lb->fp = fp;
    lb->size = (uint32_t)sz;
    *out = &lb->base;
    return ESP_OK;
}

/* ---- mount table + shared track buffer ------------------------------------- */

typedef struct {
    bool            mounted;
    char            name[CONFIG_FILE_CAP];
    disk_backing_t *backing;
} drive_t;

static drive_t s_drv[DISK_MAX_DRIVE];

static uint8_t s_track_buf[DISK_TRACK_BUF_SIZE];
static struct {
    bool     valid;
    int      drive;
    uint32_t track;
    size_t   len;
} s_cache;

static SemaphoreHandle_t s_lock;

static inline bool drive_ok(int drive)
{
    return drive >= 0 && drive < DISK_MAX_DRIVE;
}

static inline void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void unlock(void) { xSemaphoreGive(s_lock); }

esp_err_t disk_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

/* Drop the shared-buffer cache if it refers to `drive` (called on unmount/remount). */
static void cache_drop_drive(int drive)
{
    if (s_cache.valid && s_cache.drive == drive) {
        s_cache.valid = false;
    }
}

esp_err_t disk_mount(int drive, const char *name)
{
    if (!drive_ok(drive) || !name || !*name) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sd_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    disk_backing_t *b = NULL;
    esp_err_t err = local_open(name, &b);
    if (err != ESP_OK) {
        return err;
    }

    lock();
    if (s_drv[drive].mounted) {
        s_drv[drive].backing->close(s_drv[drive].backing);
        cache_drop_drive(drive);
    }
    s_drv[drive].backing = b;
    s_drv[drive].mounted = true;
    strlcpy(s_drv[drive].name, name, sizeof s_drv[drive].name);
    uint32_t sz = b->size(b);
    unlock();

    ESP_LOGI(TAG, "drive %d mounted '%s' (%lu bytes)", drive, name, (unsigned long)sz);
    return ESP_OK;
}

esp_err_t disk_unmount(int drive)
{
    if (!drive_ok(drive)) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    if (s_drv[drive].mounted) {
        s_drv[drive].backing->close(s_drv[drive].backing);
        s_drv[drive].backing = NULL;
        s_drv[drive].mounted = false;
        s_drv[drive].name[0] = '\0';
        cache_drop_drive(drive);
    }
    unlock();
    return ESP_OK;
}

bool disk_is_mounted(int drive)
{
    return drive_ok(drive) && s_drv[drive].mounted;
}

const char *disk_name(int drive)
{
    return drive_ok(drive) ? s_drv[drive].name : "";
}

uint32_t disk_image_size(int drive)
{
    if (!disk_is_mounted(drive)) {
        return 0;
    }
    lock();
    uint32_t sz = s_drv[drive].backing->size(s_drv[drive].backing);
    unlock();
    return sz;
}

uint16_t disk_status_bitmap(void)
{
    bool mounted[DISK_MAX_DRIVE];
    lock();
    for (int i = 0; i < DISK_MAX_DRIVE; ++i) {
        mounted[i] = s_drv[i].mounted;
    }
    unlock();
    return disk_status_bitmap_of(mounted, DISK_MAX_DRIVE);
}

/* Validate the (drive,track,len) triple against the mount + image size. Call locked. */
static esp_err_t check_track(int drive, uint32_t track, size_t len)
{
    if (!drive_ok(drive)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0 || len > DISK_TRACK_BUF_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_drv[drive].mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t size = s_drv[drive].backing->size(s_drv[drive].backing);
    if (!disk_track_in_bounds(track, (uint32_t)len, size)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t disk_read_track(int drive, uint32_t track, size_t len)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    esp_err_t err = check_track(drive, track, len);
    if (err != ESP_OK) {
        unlock();
        return err;
    }

    /* Cache hit: the shared buffer already holds this exact track. */
    if (s_cache.valid && s_cache.drive == drive && s_cache.track == track &&
        s_cache.len == len) {
        unlock();
        return ESP_OK;
    }

    disk_backing_t *b = s_drv[drive].backing;
    err = b->read_at(b, disk_track_offset(track, (uint32_t)len), s_track_buf, len);
    if (err == ESP_OK) {
        s_cache.valid = true;
        s_cache.drive = drive;
        s_cache.track = track;
        s_cache.len = len;
    } else {
        s_cache.valid = false;
    }
    unlock();
    return err;
}

esp_err_t disk_write_track(int drive, uint32_t track, size_t len, const void *data)
{
    if (!s_lock || !data) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    esp_err_t err = check_track(drive, track, len);
    if (err != ESP_OK) {
        unlock();
        return err;
    }

    disk_backing_t *b = s_drv[drive].backing;
    err = b->write_at(b, disk_track_offset(track, (uint32_t)len), data, len);
    if (err == ESP_OK) {
        /* Keep the shared buffer + cache consistent with what we just wrote. */
        memcpy(s_track_buf, data, len);
        s_cache.valid = true;
        s_cache.drive = drive;
        s_cache.track = track;
        s_cache.len = len;
    } else {
        cache_drop_drive(drive);
    }
    unlock();
    return err;
}

const uint8_t *disk_track_buffer(int *drive_out, uint32_t *track_out, size_t *len_out)
{
    lock();
    if (drive_out) {
        *drive_out = s_cache.valid ? s_cache.drive : -1;
    }
    if (track_out) {
        *track_out = s_cache.valid ? s_cache.track : 0;
    }
    if (len_out) {
        *len_out = s_cache.valid ? s_cache.len : 0;
    }
    unlock();
    return s_track_buf;
}

void disk_automount(void)
{
    const config_t *cfg = config_get();
    for (int i = 0; i < DISK_MAX_DRIVE; ++i) {
        const char *name = cfg->drive[i];
        if (!name[0]) {
            continue;
        }
        if (strncmp(name, TNFS_SCHEME, strlen(TNFS_SCHEME)) == 0) {
            ESP_LOGI(TAG, "drive %d '%s' deferred to WiFi-up (TNFS, M9)", i, name);
            continue; /* remote mounts happen after WiFi connects (§10.1/§12) */
        }
        esp_err_t err = disk_mount(i, name);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "drive %d auto-mount '%s' failed: %s", i, name,
                     esp_err_to_name(err));
        }
    }
}
