/*
 * disk.c — disk-image mount table + whole-track I/O (DESIGN.md §6/§10, plan M3/M9).
 *
 * Owns the 4-drive mount table, the shared 8192-byte track buffer and its 1-entry
 * (drive,track,len) cache, and the track-offset arithmetic (all 32-bit — the old
 * firmware's signed-int bug is called out in DESIGN.md §6.5/§15). A single mutex
 * serializes every SD touch so the CLI/FTP paths and the fdc task (M4) never collide
 * on the card (DESIGN.md §5.2).
 *
 * Each mount is opened through a small "backing" interface so the FDC path is unaware
 * whether bytes come from an SD file or a TNFS server (DESIGN.md §10.1). Two backings
 * ship: LOCAL (a FATFS file opened r+) and REMOTE (a tnfs:// session, §9.5).
 *
 * Remote I/O must never run on the core-1 `fdc` task (§5.2/§9.5): the fdc path must not
 * block on the network. So a dedicated **disk-I/O worker task** on core 0 owns every
 * TNFS session and performs every remote read/write/mount/close; the calling task (fdc,
 * or the CLI) hands off a job and waits under the FDC ~7 s data timeout while the worker
 * does the round trips. Local track I/O still runs inline under the mutex (fast SD).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "disk.h"
#include "net.h"
#include "sd.h"
#include "tnfs.h"

static const char *TAG = "disk";

/* ---- backing interface (local SD file or remote TNFS session, §10.1) ------- */

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

/* Remote backing: a tnfs:// session (base MUST be the first member). Every method here
 * runs ONLY on the worker task (the sole owner of the session), so tnfs.c needs no
 * locking of its own. */
typedef struct {
    disk_backing_t base;
    tnfs_file_t   *tf;
} remote_backing_t;

static esp_err_t remote_read_at(disk_backing_t *b, uint32_t off, void *buf, size_t len)
{
    return tnfs_read_at(((remote_backing_t *)b)->tf, off, buf, len);
}

static esp_err_t remote_write_at(disk_backing_t *b, uint32_t off, const void *buf, size_t len)
{
    return tnfs_write_at(((remote_backing_t *)b)->tf, off, buf, len);
}

static uint32_t remote_size(disk_backing_t *b)
{
    return tnfs_size(((remote_backing_t *)b)->tf);
}

static void remote_close(disk_backing_t *b)
{
    remote_backing_t *rb = (remote_backing_t *)b;
    tnfs_close(rb->tf); /* network UMOUNT/CLOSE — safe: worker-only */
    free(rb);
}

/* Wrap an open tnfs_file_t in a remote backing (takes ownership of tf on success). */
static disk_backing_t *remote_backing_new(tnfs_file_t *tf)
{
    remote_backing_t *rb = calloc(1, sizeof *rb);
    if (!rb) {
        return NULL;
    }
    rb->base.read_at = remote_read_at;
    rb->base.write_at = remote_write_at;
    rb->base.size = remote_size;
    rb->base.close = remote_close;
    rb->tf = tf;
    return &rb->base;
}

/* ---- mount table + shared track buffer ------------------------------------- */

typedef struct {
    bool            mounted;   /* backing installed and ready to serve tracks */
    bool            remote;    /* slot is a tnfs:// URL (mounted or not-ready) */
    bool            readonly;  /* remote opened O_RDONLY only (WRIT fails cleanly) */
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

/* Drop the shared-buffer cache if it refers to `drive` (called on unmount/remount). */
static void cache_drop_drive(int drive)
{
    if (s_cache.valid && s_cache.drive == drive) {
        s_cache.valid = false;
    }
}

/* ---- disk-I/O worker (core 0): the sole owner of remote sessions ----------- *
 *
 * Remote reads/writes and TNFS mount/close never run on the fdc task (§5.2/§9.5).
 * A job is posted to s_rq and executed here; a synchronous caller waits on the job's
 * `done` semaphore. To stay safe if a caller times out first, ownership of the job is
 * transferred to the worker via the `abandoned` flag under s_job_mtx, so the worker
 * frees it and never touches caller memory after the fact.
 */
typedef enum { RJOB_READ, RJOB_WRITE, RJOB_MOUNT, RJOB_CLOSE } rjob_type_t;

typedef struct {
    rjob_type_t       type;
    int               drive;      /* READ/WRITE/MOUNT */
    uint32_t          track;      /* READ/WRITE */
    size_t            len;        /* READ/WRITE */
    void             *wdata;      /* RJOB_WRITE: owned copy of the track data */
    disk_backing_t   *bclose;     /* RJOB_CLOSE: backing to close */
    esp_err_t         result;
    bool              completed;  /* guarded by s_job_mtx */
    bool              abandoned;  /* guarded by s_job_mtx */
    SemaphoreHandle_t done;       /* binary; NULL = fire-and-forget */
} rjob_t;

#define REMOTE_QUEUE_LEN    12
#define REMOTE_QSEND_MS     1000   /* wait to enqueue a job */
#define REMOTE_OP_WAIT_MS   6000   /* fdc read/write wait (< 7 s host data timeout) */
#define REMOTE_MOUNT_WAIT_MS 12000 /* CLI mount wait (UDP probe + TCP connect + OPEN) */
#define REMOTE_WORKER_STACK 6144

static QueueHandle_t     s_rq;
static SemaphoreHandle_t s_job_mtx;
static uint8_t           s_stage[DISK_TRACK_BUF_SIZE]; /* worker's private I/O buffer */

static void rjob_free(rjob_t *j)
{
    if (j->done) {
        vSemaphoreDelete(j->done);
    }
    free(j->wdata);
    free(j);
}

/* Worker-side completion: signal a waiting caller, or free an abandoned/async job. */
static void rjob_complete(rjob_t *j, esp_err_t result)
{
    xSemaphoreTake(s_job_mtx, portMAX_DELAY);
    if (j->abandoned || j->done == NULL) {
        xSemaphoreGive(s_job_mtx);
        rjob_free(j);
        return;
    }
    j->result = result;
    j->completed = true;
    xSemaphoreGive(j->done);      /* caller may free j after this... */
    xSemaphoreGive(s_job_mtx);    /* ...but never s_job_mtx, so this is safe */
}

/* Fetch a remote track into s_track_buf + prime the cache (worker context). */
static void worker_read(rjob_t *j)
{
    disk_backing_t *b = NULL;
    lock();
    if (drive_ok(j->drive) && s_drv[j->drive].mounted && s_drv[j->drive].remote &&
        s_drv[j->drive].backing) {
        uint32_t size = s_drv[j->drive].backing->size(s_drv[j->drive].backing);
        if (disk_track_in_bounds(j->track, (uint32_t)j->len, size)) {
            b = s_drv[j->drive].backing;
        }
    }
    unlock();
    if (!b || !net_is_connected()) {
        rjob_complete(j, ESP_ERR_INVALID_STATE); /* not-ready */
        return;
    }

    esp_err_t err = b->read_at(b, disk_track_offset(j->track, (uint32_t)j->len),
                               s_stage, j->len);
    if (err == ESP_OK) {
        lock();
        if (s_drv[j->drive].mounted && s_drv[j->drive].backing == b) {
            memcpy(s_track_buf, s_stage, j->len);
            s_cache.valid = true;
            s_cache.drive = j->drive;
            s_cache.track = j->track;
            s_cache.len = j->len;
        } else {
            err = ESP_ERR_INVALID_STATE; /* unmounted while we were reading */
        }
        unlock();
    }
    rjob_complete(j, err);
}

/* Write a remote track from the owned copy, then refresh s_track_buf + cache. */
static void worker_write(rjob_t *j)
{
    disk_backing_t *b = NULL;
    lock();
    if (drive_ok(j->drive) && s_drv[j->drive].mounted && s_drv[j->drive].remote &&
        !s_drv[j->drive].readonly && s_drv[j->drive].backing) {
        uint32_t size = s_drv[j->drive].backing->size(s_drv[j->drive].backing);
        if (disk_track_in_bounds(j->track, (uint32_t)j->len, size)) {
            b = s_drv[j->drive].backing;
        }
    }
    unlock();
    if (!b || !net_is_connected()) {
        rjob_complete(j, ESP_ERR_INVALID_STATE);
        return;
    }

    esp_err_t err = b->write_at(b, disk_track_offset(j->track, (uint32_t)j->len),
                                j->wdata, j->len);
    if (err == ESP_OK) {
        lock();
        if (s_drv[j->drive].mounted && s_drv[j->drive].backing == b) {
            memcpy(s_track_buf, j->wdata, j->len);
            s_cache.valid = true;
            s_cache.drive = j->drive;
            s_cache.track = j->track;
            s_cache.len = j->len;
        }
        unlock();
    } else {
        lock();
        cache_drop_drive(j->drive);
        unlock();
    }
    rjob_complete(j, err);
}

/* Open a TNFS session for a configured-but-not-ready remote drive and install it. */
static void worker_mount(rjob_t *j)
{
    char url[CONFIG_FILE_CAP];
    lock();
    bool want = drive_ok(j->drive) && s_drv[j->drive].remote &&
                !s_drv[j->drive].mounted && s_drv[j->drive].name[0];
    if (want) {
        strlcpy(url, s_drv[j->drive].name, sizeof url);
    }
    unlock();
    if (!want) {
        rjob_complete(j, ESP_OK);
        return;
    }
    if (!net_is_connected()) {
        rjob_complete(j, ESP_ERR_INVALID_STATE);
        return;
    }

    /* Default read-write (§10.1); fall back to read-only if the server won't grant it.
     * Keep both errors: on fallback, report why write was denied; if the read-only open
     * also fails, report ITS reason (not the read-write probe's) and fail on it. */
    tnfs_file_t *tf = NULL;
    bool readonly = false;
    esp_err_t err_rw = tnfs_open(url, true, false, &tf);
    if (err_rw != ESP_OK) {
        esp_err_t err_ro = tnfs_open(url, false, false, &tf);
        if (err_ro == ESP_OK) {
            readonly = true;
            ESP_LOGW(TAG, "drive %d '%s' opened read-only (write denied: %s)",
                     j->drive, url, esp_err_to_name(err_rw));
        } else {
            ESP_LOGW(TAG, "drive %d remote mount '%s' failed: rw=%s ro=%s", j->drive, url,
                     esp_err_to_name(err_rw), esp_err_to_name(err_ro));
            rjob_complete(j, err_ro);
            return;
        }
    }

    disk_backing_t *b = remote_backing_new(tf);
    if (!b) {
        tnfs_close(tf);
        rjob_complete(j, ESP_ERR_NO_MEM);
        return;
    }

    bool installed = false;
    lock();
    if (drive_ok(j->drive) && s_drv[j->drive].remote && !s_drv[j->drive].mounted &&
        strcmp(s_drv[j->drive].name, url) == 0) {
        s_drv[j->drive].backing = b;
        s_drv[j->drive].mounted = true;
        s_drv[j->drive].readonly = readonly;
        cache_drop_drive(j->drive);
        installed = true;
    }
    unlock();
    if (installed) {
        ESP_LOGI(TAG, "drive %d remote mounted '%s'%s (%lu bytes)", j->drive, url,
                 readonly ? " read-only" : "", (unsigned long)b->size(b));
        rjob_complete(j, ESP_OK);
    } else {
        b->close(b); /* slot changed under us; discard */
        rjob_complete(j, ESP_OK);
    }
}

static void diskio_task(void *arg)
{
    (void)arg;
    for (;;) {
        rjob_t *j;
        if (xQueueReceive(s_rq, &j, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (j->type) {
        case RJOB_READ:  worker_read(j);  break;
        case RJOB_WRITE: worker_write(j); break;
        case RJOB_MOUNT: worker_mount(j); break;
        case RJOB_CLOSE:
            if (j->bclose) {
                j->bclose->close(j->bclose);
            }
            rjob_complete(j, ESP_OK);
            break;
        }
    }
}

static rjob_t *rjob_new(rjob_type_t type, int drive)
{
    rjob_t *j = calloc(1, sizeof *j);
    if (j) {
        j->type = type;
        j->drive = drive;
    }
    return j;
}

/* Build a READ/WRITE track job; RJOB_WRITE takes an owned copy of `data` so the caller's
 * buffer is safe to reuse even if it later abandons the wait. NULL on allocation failure. */
static rjob_t *rjob_new_io(rjob_type_t type, int drive, uint32_t track, size_t len,
                           const void *data)
{
    rjob_t *j = rjob_new(type, drive);
    if (!j) {
        return NULL;
    }
    j->track = track;
    j->len = len;
    if (type == RJOB_WRITE) {
        j->wdata = malloc(len);
        if (!j->wdata) {
            free(j);
            return NULL;
        }
        memcpy(j->wdata, data, len);
    }
    return j;
}

/* Post a fire-and-forget job (worker frees it). Frees + returns error on enqueue fail. */
static esp_err_t rjob_post_async(rjob_t *j)
{
    if (!j) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_rq) {
        rjob_free(j);
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_rq, &j, pdMS_TO_TICKS(REMOTE_QSEND_MS)) != pdTRUE) {
        rjob_free(j);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/* Post a job and wait up to wait_ms for it; returns the job's result. Hands the job to
 * the worker to free if the wait times out (see rjob_complete). */
static esp_err_t rjob_post_wait(rjob_t *j, uint32_t wait_ms)
{
    if (!j) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_rq) {
        rjob_free(j);
        return ESP_ERR_INVALID_STATE;
    }
    j->done = xSemaphoreCreateBinary();
    if (!j->done) {
        rjob_free(j);
        return ESP_ERR_NO_MEM;
    }
    if (xQueueSend(s_rq, &j, pdMS_TO_TICKS(REMOTE_QSEND_MS)) != pdTRUE) {
        rjob_free(j);
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(j->done, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
        esp_err_t r = j->result;
        rjob_free(j);
        return r;
    }
    /* Timed out. Either the worker just finished (we own the free) or it is still
     * running (mark abandoned; the worker frees it). */
    esp_err_t r = ESP_ERR_TIMEOUT;
    xSemaphoreTake(s_job_mtx, portMAX_DELAY);
    if (j->completed) {
        r = j->result;
        xSemaphoreGive(s_job_mtx);
        rjob_free(j);
    } else {
        j->abandoned = true;
        xSemaphoreGive(s_job_mtx);
    }
    return r;
}

/* Close an old backing safely: a remote one must be closed on the worker; a local one
 * (fclose) is safe to close directly. Caller has already removed it from the table. */
static void backing_dispose(disk_backing_t *b, bool remote)
{
    if (!b) {
        return;
    }
    if (!remote) {
        b->close(b);
        return;
    }
    rjob_t *j = rjob_new(RJOB_CLOSE, -1);
    if (j) {
        j->bclose = b;
        if (rjob_post_async(j) == ESP_OK) {
            return;
        }
    }
    /* Could not hand it to the worker; leaking beats closing a live session off-worker. */
    ESP_LOGW(TAG, "could not schedule remote session close (leaked)");
}

/* ---- init ------------------------------------------------------------------ */

esp_err_t disk_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_job_mtx) {
        s_job_mtx = xSemaphoreCreateMutex();
        if (!s_job_mtx) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_rq) {
        s_rq = xQueueCreate(REMOTE_QUEUE_LEN, sizeof(rjob_t *));
        if (!s_rq) {
            return ESP_ERR_NO_MEM;
        }
        /* Core 0, off the core-1 fdc path (§5.2). Priority above CLI/net so track
         * fetches stay prompt. */
        if (xTaskCreatePinnedToCore(diskio_task, "diskio", REMOTE_WORKER_STACK, NULL,
                                    6, NULL, 0) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

/* ---- mount / unmount ------------------------------------------------------- */

/* Configure a slot as a remote drive and, if WiFi is up, open its session now. Any old
 * backing on the slot is disposed of. Returns ESP_OK when mounted or deferred (caller
 * inspects disk_is_mounted() + link state to phrase the result). */
static esp_err_t remote_configure(int drive, const char *url)
{
    disk_backing_t *old = NULL;
    bool old_remote = false;
    lock();
    old = s_drv[drive].backing;
    old_remote = s_drv[drive].remote;
    s_drv[drive].backing = NULL;
    s_drv[drive].mounted = false;
    s_drv[drive].remote = true;
    s_drv[drive].readonly = false;
    strlcpy(s_drv[drive].name, url, sizeof s_drv[drive].name);
    cache_drop_drive(drive);
    unlock();
    backing_dispose(old, old_remote);

    if (!net_is_connected()) {
        return ESP_OK; /* deferred to WiFi-up (§10.1/§12 step 9) */
    }
    rjob_t *j = rjob_new(RJOB_MOUNT, drive);
    if (!j) {
        return ESP_ERR_NO_MEM;
    }
    return rjob_post_wait(j, REMOTE_MOUNT_WAIT_MS);
}

esp_err_t disk_mount(int drive, const char *name)
{
    if (!drive_ok(drive) || !name || !*name) {
        return ESP_ERR_INVALID_ARG;
    }

    if (tnfs_is_url(name)) {
        return remote_configure(drive, name);
    }

    /* Local SD mount: open the new image first, then swap under the lock (fast, no
     * network). If it can't be opened, still drop whatever was mounted: a failed
     * `mount` must never silently leave the old disk in place, or the operator may act
     * on the wrong disk (e.g. format it) believing the new one took. */
    if (!sd_mounted()) {
        disk_unmount(drive);
        return ESP_ERR_INVALID_STATE;
    }
    disk_backing_t *b = NULL;
    esp_err_t err = local_open(name, &b);
    if (err != ESP_OK) {
        disk_unmount(drive);
        return err;
    }

    disk_backing_t *old = NULL;
    bool old_remote = false;
    lock();
    old = s_drv[drive].backing;
    old_remote = s_drv[drive].remote;
    s_drv[drive].backing = b;
    s_drv[drive].mounted = true;
    s_drv[drive].remote = false;
    s_drv[drive].readonly = false;
    strlcpy(s_drv[drive].name, name, sizeof s_drv[drive].name);
    cache_drop_drive(drive);
    uint32_t sz = b->size(b);
    unlock();
    backing_dispose(old, old_remote);

    ESP_LOGI(TAG, "drive %d mounted '%s' (%lu bytes)", drive, name, (unsigned long)sz);
    return ESP_OK;
}

esp_err_t disk_unmount(int drive)
{
    if (!drive_ok(drive)) {
        return ESP_ERR_INVALID_ARG;
    }
    disk_backing_t *old = NULL;
    bool old_remote = false;
    lock();
    old = s_drv[drive].backing;
    old_remote = s_drv[drive].remote;
    s_drv[drive].backing = NULL;
    s_drv[drive].mounted = false;
    s_drv[drive].remote = false;
    s_drv[drive].readonly = false;
    s_drv[drive].name[0] = '\0';
    cache_drop_drive(drive);
    unlock();
    backing_dispose(old, old_remote);
    return ESP_OK;
}

bool disk_is_mounted(int drive)
{
    return drive_ok(drive) && s_drv[drive].mounted;
}

bool disk_is_remote(int drive)
{
    return drive_ok(drive) && s_drv[drive].remote;
}

bool disk_is_readonly(int drive)
{
    return drive_ok(drive) && s_drv[drive].readonly;
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
    uint32_t sz = s_drv[drive].backing ? s_drv[drive].backing->size(s_drv[drive].backing) : 0;
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

/* ---- track I/O ------------------------------------------------------------- */

/* Validate the (drive,track,len) triple against the mount + image size. Call locked. */
static esp_err_t check_track(int drive, uint32_t track, size_t len)
{
    if (!drive_ok(drive)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0 || len > DISK_TRACK_BUF_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_drv[drive].mounted || !s_drv[drive].backing) {
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

    /* Remote: hand off to the worker (core 0) so the fdc task never blocks on the
     * network under the lock (§5.2/§9.5). The worker fills s_track_buf + cache. */
    if (s_drv[drive].remote) {
        unlock();
        return rjob_post_wait(rjob_new_io(RJOB_READ, drive, track, len, NULL),
                              REMOTE_OP_WAIT_MS);
    }

    /* Local: read inline under the lock (fast SD I/O). */
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
    if (s_drv[drive].readonly) {
        unlock();
        return ESP_ERR_NOT_SUPPORTED; /* remote read-only mount -> WSTA write-err */
    }

    if (s_drv[drive].remote) {
        unlock();
        return rjob_post_wait(rjob_new_io(RJOB_WRITE, drive, track, len, data),
                              REMOTE_OP_WAIT_MS);
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

/* ---- remote lifecycle (WiFi up/down) --------------------------------------- */

void disk_automount(void)
{
    const config_t *cfg = config_get();
    for (int i = 0; i < DISK_MAX_DRIVE; ++i) {
        const char *name = cfg->drive[i];
        if (!name[0]) {
            continue;
        }
        if (tnfs_is_url(name)) {
            /* Register the slot as a not-ready remote drive; the session opens on
             * WiFi-up via disk_remote_mount_all() (§10.1/§12 step 9). */
            lock();
            s_drv[i].remote = true;
            s_drv[i].mounted = false;
            s_drv[i].readonly = false;
            strlcpy(s_drv[i].name, name, sizeof s_drv[i].name);
            unlock();
            ESP_LOGI(TAG, "drive %d '%s' deferred to WiFi-up (remote)", i, name);
            continue;
        }
        esp_err_t err = disk_mount(i, name);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "drive %d auto-mount '%s' failed: %s", i, name,
                     esp_err_to_name(err));
        }
    }
}

void disk_remote_mount_all(void)
{
    if (!s_rq) {
        return;
    }
    for (int i = 0; i < DISK_MAX_DRIVE; ++i) {
        lock();
        bool want = s_drv[i].remote && !s_drv[i].mounted && s_drv[i].name[0];
        unlock();
        if (!want) {
            continue;
        }
        rjob_t *j = rjob_new(RJOB_MOUNT, i);
        if (j && rjob_post_async(j) != ESP_OK) {
            ESP_LOGW(TAG, "drive %d remote mount not scheduled", i);
        }
    }
}

void disk_remote_unmount_all(void)
{
    /* Mark every remote drive not-ready immediately (so STAT reflects it at once), and
     * hand the live sessions to the worker to close. */
    disk_backing_t *to_close[DISK_MAX_DRIVE];
    int n = 0;
    lock();
    for (int i = 0; i < DISK_MAX_DRIVE; ++i) {
        if (s_drv[i].remote && s_drv[i].mounted) {
            to_close[n++] = s_drv[i].backing;
            s_drv[i].backing = NULL;
            s_drv[i].mounted = false;
            s_drv[i].readonly = false;
            cache_drop_drive(i);
        }
    }
    unlock();
    for (int k = 0; k < n; ++k) {
        backing_dispose(to_close[k], true);
    }
}
