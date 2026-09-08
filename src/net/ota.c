/*
 * ota.c — firmware update backend (DESIGN.md §11 / §11.1).
 *
 * Bare `update` reports status only — the running version, whether an SD
 * `/sd/firmware.bin` is present (and its embedded version), and, when WiFi is up, the
 * version the repo is offering — and installs nothing. Installing is an explicit verb:
 *
 * Three sources feed one writer:
 *   - SD    `update local`   via esp_ota_ops (read /sd/firmware.bin, write inactive slot)
 *   - repo  `update ota`     via esp_https_ota, after a version.txt gate
 *   - url   `update <url>`   via esp_https_ota (https), or the M9 TNFS client (tnfs)
 *
 * Network transfers pull raw files from the configured GitHub repo (`otaRepo`, §7) on
 * branch `master`. raw.githubusercontent.com serves the content directly (HTTP 200, no
 * redirect to a release-asset CDN), so certs validate against the bundled CA roots
 * (esp_crt_bundle) with nothing host-pinned.
 *
 * Every update runs on a dedicated core-0 task: esp_https_ota + mbedTLS need ~8 KB of
 * stack, far more than the 4 KB CLI tasks carry, and flashing must not run on the
 * FDC-critical core 1. The invoking CLI task blocks on a completion semaphore while the
 * OTA task streams progress to that console. A portMUX flag makes updates single-flight
 * (serial and TCP consoles can't flash the same slot at once). On success the OTA task
 * reboots into the new slot itself, so the public calls return only on failure or a
 * no-op (repo already current).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"

#include "cli.h"
#include "config.h"
#include "net.h"
#include "ota.h"
#include "sd.h"
#include "version.h"

/* Where the network paths fetch from (DESIGN.md §11.1). raw.<repo>/master/ota/... */
#define OTA_RAW_HOST "https://raw.githubusercontent.com"
#define OTA_BRANCH   "master"
#define OTA_DIR      "ota"
#define OTA_BIN      "firmware.bin"

#define OTA_SD_PATH  SD_MOUNT_POINT "/firmware.bin"
#define OTA_SD_NAME  "/firmware.bin"           /* user-facing name */

#define OTA_HTTP_TIMEOUT_MS 15000
#define OTA_SD_CHUNK        4096
#define OTA_URL_MAX         256

typedef enum { OTA_SRC_STATUS, OTA_SRC_SD, OTA_SRC_REPO, OTA_SRC_URL } ota_src_t;

typedef struct {
    cli_console_t    *c;
    ota_src_t         src;
    char              url[OTA_URL_MAX];  /* OTA_SRC_URL only */
    bool              reboot;            /* set true once a new image is committed */
    esp_err_t         result;
    SemaphoreHandle_t done;
} ota_job_t;

/* Single-flight guard: only one update task may run at a time. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool         s_busy;

/* ---- helpers --------------------------------------------------------------- */

/* Stream a transfer's percentage (or, when the size is unknown, a dot per 64 KB) to
 * the console in place. `*mark` carries the last value printed across calls. */
static void progress(cli_console_t *c, int done, int total, int *mark)
{
    if (total > 0) {
        int pct = (int)((int64_t)done * 100 / total);
        if (pct != *mark) {
            *mark = pct;
            cli_printf(c, "\r  %3d%%", pct);
        }
    } else {
        int dots = done / 65536;
        if (dots != *mark) {
            *mark = dots;
            cli_write(c, ".");
        }
    }
}

/* Accept "1.2.3" (tolerating a leading 'v' and surrounding whitespace). */
static bool parse_semver(const char *s, int *maj, int *min, int *pat)
{
    while (*s == ' ' || *s == '\t' || *s == 'v' || *s == 'V') {
        ++s;
    }
    return sscanf(s, "%d.%d.%d", maj, min, pat) == 3;
}

/* True if maj.min.pat is strictly newer than the running firmware version. */
static bool version_newer(int maj, int min, int pat)
{
    if (maj != FDCSDS_VERSION_MAJOR) return maj > FDCSDS_VERSION_MAJOR;
    if (min != FDCSDS_VERSION_MINOR) return min > FDCSDS_VERSION_MINOR;
    return pat > FDCSDS_VERSION_PATCH;
}

/* HTTP GET a small text body into `out` (NUL-terminated, truncated to cap-1). Uses
 * esp_http_client_perform so redirects are followed. Reports failures to the console. */
typedef struct { char *buf; int cap; int len; } http_text_t;

static esp_err_t http_text_evt(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data) {
        http_text_t *t = (http_text_t *)e->user_data;
        int n = e->data_len;
        if (n > t->cap - 1 - t->len) {
            n = t->cap - 1 - t->len;
        }
        if (n > 0) {
            memcpy(t->buf + t->len, e->data, (size_t)n);
            t->len += n;
        }
    }
    return ESP_OK;
}

static esp_err_t http_get_text(cli_console_t *c, const char *url, char *out, size_t cap)
{
    http_text_t ctx = { out, (int)cap, 0 };
    esp_http_client_config_t cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = OTA_HTTP_TIMEOUT_MS,
        .event_handler     = http_text_evt,
        .user_data         = &ctx,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_perform(cl);
    int status = esp_http_client_get_status_code(cl);
    esp_http_client_cleanup(cl);

    if (err != ESP_OK) {
        cli_printf(c, "fetch %s: %s\r\n", OTA_DIR "/version.txt", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        cli_printf(c, "version.txt: HTTP %d\r\n", status);
        return ESP_FAIL;
    }
    out[ctx.len] = '\0';
    return ESP_OK;
}

/* ---- the three sources ----------------------------------------------------- */

/* Read `/sd/firmware.bin` into the inactive slot, delete it, set it bootable. */
static esp_err_t do_sd(ota_job_t *j)
{
    cli_console_t *c = j->c;

    if (!sd_mounted()) {
        cli_write(c, "no SD card\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    struct stat st;
    if (stat(OTA_SD_PATH, &st) != 0) {
        cli_printf(c, "%s not found on SD\r\n", OTA_SD_NAME);
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size <= 0) {
        cli_printf(c, "%s is empty\r\n", OTA_SD_NAME);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(OTA_SD_PATH, "rb");
    if (!f) {
        cli_printf(c, "open %s: %s\r\n", OTA_SD_NAME, strerror(errno));
        return ESP_FAIL;
    }

    const esp_partition_t *tgt = esp_ota_get_next_update_partition(NULL);
    if (!tgt) {
        fclose(f);
        cli_write(c, "no OTA slot available (check partitions)\r\n");
        return ESP_FAIL;
    }

    uint8_t *buf = malloc(OTA_SD_CHUNK);
    if (!buf) {
        fclose(f);
        cli_write(c, "out of memory\r\n");
        return ESP_ERR_NO_MEM;
    }

    int total = (int)st.st_size;
    cli_printf(c, "flashing %d bytes from %s to '%s' (erasing)...\r\n",
               total, OTA_SD_NAME, tgt->label);

    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(tgt, (size_t)total, &h);
    if (err != ESP_OK) {
        free(buf);
        fclose(f);
        cli_printf(c, "ota_begin: %s\r\n", esp_err_to_name(err));
        return err;
    }

    int done = 0, mark = -1;
    size_t n;
    while ((n = fread(buf, 1, OTA_SD_CHUNK, f)) > 0) {
        err = esp_ota_write(h, buf, n);
        if (err != ESP_OK) {
            break;
        }
        done += (int)n;
        progress(c, done, total, &mark);
    }
    bool read_err = ferror(f);
    free(buf);
    fclose(f);

    if (err != ESP_OK) {
        esp_ota_abort(h);
        cli_printf(c, "\r\nota_write: %s\r\n", esp_err_to_name(err));
        return err;
    }
    if (read_err) {
        esp_ota_abort(h);
        cli_write(c, "\r\nread error on firmware.bin\r\n");
        return ESP_FAIL;
    }

    err = esp_ota_end(h); /* validates the image (magic, checksum) */
    if (err != ESP_OK) {
        cli_printf(c, "\r\nota_end (bad image?): %s\r\n", esp_err_to_name(err));
        return err;
    }
    err = esp_ota_set_boot_partition(tgt);
    if (err != ESP_OK) {
        cli_printf(c, "\r\nset_boot: %s\r\n", esp_err_to_name(err));
        return err;
    }

    unlink(OTA_SD_PATH);
    cli_printf(c, "\r\nflashed OK; boot set to '%s'\r\n", tgt->label);
    j->reboot = true;
    return ESP_OK;
}

/* Stream an https:// image straight into the inactive slot via esp_https_ota. */
static esp_err_t do_https(ota_job_t *j, const char *url)
{
    cli_console_t *c = j->c;

    if (!net_is_connected()) {
        cli_write(c, "WiFi not connected\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    esp_http_client_config_t http = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http };

    cli_printf(c, "connecting to %s ...\r\n", url);
    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        cli_printf(c, "connect failed: %s\r\n", esp_err_to_name(err));
        return err;
    }

    int total = esp_https_ota_get_image_size(handle);
    if (total > 0) {
        cli_printf(c, "downloading %d bytes...\r\n", total);
    } else {
        cli_write(c, "downloading...\r\n");
    }

    int mark = -1;
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        progress(c, esp_https_ota_get_image_len_read(handle), total, &mark);
    }

    if (err != ESP_OK) {
        cli_printf(c, "\r\ndownload failed: %s\r\n", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        return err;
    }
    if (!esp_https_ota_is_complete_data_received(handle)) {
        cli_write(c, "\r\nincomplete download\r\n");
        esp_https_ota_abort(handle);
        return ESP_FAIL;
    }

    err = esp_https_ota_finish(handle); /* validates image + sets boot partition */
    if (err != ESP_OK) {
        cli_printf(c, "\r\nfinish (bad image?): %s\r\n", esp_err_to_name(err));
        return err;
    }

    cli_write(c, "\r\ndownloaded and verified; boot set to new slot\r\n");
    j->reboot = true;
    return ESP_OK;
}

/* `update ota`: gate on version.txt, then pull the release binary if it is newer. */
static esp_err_t do_repo(ota_job_t *j)
{
    cli_console_t *c = j->c;

    if (!net_is_connected()) {
        cli_write(c, "WiFi not connected\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    const config_t *cfg = config_get();
    if (!cfg->ota_repo[0]) {
        cli_write(c, "no OTA repo configured (otaRepo empty)\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    char vurl[OTA_URL_MAX];
    snprintf(vurl, sizeof vurl, OTA_RAW_HOST "/%s/" OTA_BRANCH "/" OTA_DIR "/version.txt",
             cfg->ota_repo);

    char text[32];
    esp_err_t err = http_get_text(c, vurl, text, sizeof text);
    if (err != ESP_OK) {
        return err;
    }

    int maj, min, pat;
    if (!parse_semver(text, &maj, &min, &pat)) {
        cli_printf(c, "unrecognized version.txt: '%s'\r\n", text);
        return ESP_FAIL;
    }

    cli_printf(c, "running %s, latest %d.%d.%d\r\n", FDCSDS_VERSION_STRING, maj, min, pat);
    if (!version_newer(maj, min, pat)) {
        cli_write(c, "already up to date\r\n");
        return ESP_OK; /* no reboot: j->reboot stays false */
    }

    char burl[OTA_URL_MAX];
    snprintf(burl, sizeof burl, OTA_RAW_HOST "/%s/" OTA_BRANCH "/" OTA_DIR "/" OTA_BIN,
             cfg->ota_repo);
    return do_https(j, burl);
}

/* `update <url>`: dispatch on scheme. https now; tnfs when the M9 client lands. */
static esp_err_t do_url(ota_job_t *j)
{
    if (strncasecmp(j->url, "https://", 8) == 0) {
        return do_https(j, j->url);
    }
    if (strncasecmp(j->url, "tnfs://", 7) == 0) {
        /* The TNFS client is an M9 deliverable; wire this to it there. */
        cli_write(j->c, "tnfs:// OTA is not available yet (needs the TNFS client, M9)\r\n");
        return ESP_ERR_NOT_SUPPORTED;
    }
    cli_write(j->c, "url must start with https:// or tnfs://\r\n");
    return ESP_ERR_INVALID_ARG;
}

/* ---- status (`update` with no args) ---------------------------------------- */

/* Read the firmware version string embedded in an image FILE. The esp_app_desc_t sits
 * right after the image + first-segment headers; validate its magic before trusting it.
 * Returns false (leaving `out` untouched) if the file isn't a recognizable app image. */
static bool image_file_version(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    long off = (long)(sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    esp_app_desc_t desc;
    bool ok = (fseek(f, off, SEEK_SET) == 0) &&
              (fread(&desc, 1, sizeof desc, f) == sizeof desc) &&
              (desc.magic_word == ESP_APP_DESC_MAGIC_WORD);
    fclose(f);
    if (ok) {
        strlcpy(out, desc.version, cap);
    }
    return ok;
}

/* "local:" line — is there an installable /sd/firmware.bin, and what version is it? */
static void report_local(cli_console_t *c)
{
    if (!sd_mounted()) {
        cli_printf(c, "%-9s no SD card\r\n", "local:");
        return;
    }
    struct stat st;
    if (stat(OTA_SD_PATH, &st) != 0 || st.st_size <= 0) {
        cli_printf(c, "%-9s none (%s)\r\n", "local:", OTA_SD_NAME);
        return;
    }
    char ver[32];
    if (image_file_version(OTA_SD_PATH, ver, sizeof ver)) {
        cli_printf(c, "%-9s %s  (%d bytes, v%s)\r\n", "local:", OTA_SD_NAME,
                   (int)st.st_size, ver);
    } else {
        cli_printf(c, "%-9s %s  (%d bytes)\r\n", "local:", OTA_SD_NAME, (int)st.st_size);
    }
}

/* "ota:" line — only touches the network when WiFi is up (else just says so). */
static void report_ota(cli_console_t *c)
{
    const config_t *cfg = config_get();
    if (!cfg->ota_repo[0]) {
        cli_printf(c, "%-9s no repo configured\r\n", "ota:");
        return;
    }
    if (!net_is_connected()) {
        cli_printf(c, "%-9s %s (WiFi not connected)\r\n", "ota:", cfg->ota_repo);
        return;
    }

    char vurl[OTA_URL_MAX];
    snprintf(vurl, sizeof vurl, OTA_RAW_HOST "/%s/" OTA_BRANCH "/" OTA_DIR "/version.txt",
             cfg->ota_repo);
    char text[32];
    if (http_get_text(c, vurl, text, sizeof text) != ESP_OK) {
        return; /* http_get_text already reported the failure */
    }
    int maj, min, pat;
    if (!parse_semver(text, &maj, &min, &pat)) {
        cli_printf(c, "%-9s %s (unrecognized version.txt '%s')\r\n", "ota:",
                   cfg->ota_repo, text);
        return;
    }
    cli_printf(c, "%-9s %s -> %d.%d.%d  %s\r\n", "ota:", cfg->ota_repo, maj, min, pat,
               version_newer(maj, min, pat) ? "(update available)" : "(up to date)");
}

/* Bare `update`: report versions from all three sources; install nothing. */
static esp_err_t do_status(ota_job_t *j)
{
    cli_console_t *c = j->c;
    cli_printf(c, "%-9s %s\r\n", "running:", FDCSDS_VERSION_STRING);
    report_local(c);
    report_ota(c);
    cli_write(c, "install with 'update local' (SD) or 'update ota' (network)\r\n");
    return ESP_OK;
}

/* ---- task + public entry --------------------------------------------------- */

static void ota_task(void *arg)
{
    ota_job_t *j = (ota_job_t *)arg;

    switch (j->src) {
    case OTA_SRC_STATUS: j->result = do_status(j); break;
    case OTA_SRC_SD:     j->result = do_sd(j);     break;
    case OTA_SRC_REPO:   j->result = do_repo(j);   break;
    case OTA_SRC_URL:    j->result = do_url(j);    break;
    default:             j->result = ESP_ERR_INVALID_ARG; break;
    }

    if (j->result == ESP_OK && j->reboot) {
        cli_write(j->c, "rebooting into the new firmware...\r\n");
        vTaskDelay(pdMS_TO_TICKS(300)); /* let the console output flush first */
        esp_restart();                  /* does not return */
    }

    xSemaphoreGive(j->done);
    vTaskDelete(NULL);
}

/*
 * Run one update to completion on its own task while the calling CLI task blocks.
 * Returns the update result (ESP_OK on a flash that will reboot, or on a repo no-op).
 * Single-flight: a second concurrent update is refused.
 */
static esp_err_t ota_run(cli_console_t *c, ota_src_t src, const char *url)
{
    bool grabbed = false;
    portENTER_CRITICAL(&s_mux);
    if (!s_busy) {
        s_busy = true;
        grabbed = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!grabbed) {
        cli_write(c, "an update is already in progress\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    ota_job_t job = {
        .c = c,
        .src = src,
        .reboot = false,
        .result = ESP_FAIL,
    };
    if (url) {
        strlcpy(job.url, url, sizeof job.url);
    }
    job.done = xSemaphoreCreateBinary();
    if (!job.done) {
        s_busy = false;
        cli_write(c, "out of memory\r\n");
        return ESP_ERR_NO_MEM;
    }

    /* 8 KB stack: esp_https_ota + mbedTLS need far more than the CLI task's 4 KB. */
    BaseType_t ok = xTaskCreatePinnedToCore(ota_task, "ota", 8192, &job, 5, NULL, 0);
    if (ok != pdPASS) {
        vSemaphoreDelete(job.done);
        s_busy = false;
        cli_write(c, "cannot start OTA task (out of memory)\r\n");
        return ESP_ERR_NO_MEM;
    }

    /* Block here (the on-stack `job` stays valid) until the OTA task finishes. On a
     * successful flash the task reboots instead of signalling, so we never return. */
    xSemaphoreTake(job.done, portMAX_DELAY);
    vSemaphoreDelete(job.done);
    s_busy = false;
    return job.result;
}

esp_err_t ota_status(cli_console_t *c)
{
    return ota_run(c, OTA_SRC_STATUS, NULL);
}

esp_err_t ota_update_sd(cli_console_t *c)
{
    return ota_run(c, OTA_SRC_SD, NULL);
}

esp_err_t ota_update_repo(cli_console_t *c)
{
    return ota_run(c, OTA_SRC_REPO, NULL);
}

esp_err_t ota_update_url(cli_console_t *c, const char *url)
{
    return ota_run(c, OTA_SRC_URL, url);
}
