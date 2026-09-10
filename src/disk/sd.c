/*
 * sd.c — microSD mount over SPI (DESIGN.md §2.1/§10).
 *
 * VSPI bus + sdspi at 4 MHz (§2.1), FAT mounted read/write at /sd, ≥8 open files
 * (image handles for 4 drives + FTP/CLI). We do NOT format on mount failure — a
 * card the owner filled with images must never be silently wiped; a mount failure
 * is reported and the server continues (file commands then say "no SD card").
 */
#include <string.h>

#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"

#include "sd.h"
#include "pins.h"

static const char *TAG = "sd";

static sdmmc_card_t *s_card;
static bool          s_mounted;

esp_err_t sd_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 4000; /* 4 MHz init (DESIGN.md §2.1) */

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return err;
    }

    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = PIN_SD_CS;
    dev.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &dev, &mount, &s_card);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "mount failed (no FAT filesystem?)");
        } else {
            ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        }
        spi_bus_free(host.slot);
        return err;
    }

    s_mounted = true;

    /* Card size = sectors * sector bytes; the 8 MB images need 32-bit math. */
    uint64_t bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    ESP_LOGI(TAG, "mounted %s: '%s', %llu MB", SD_MOUNT_POINT,
             s_card->cid.name, bytes / (1024ULL * 1024ULL));
    return ESP_OK;
}

bool sd_mounted(void)
{
    return s_mounted;
}

int sd_path(const char *name, char *buf, size_t len)
{
    if (!name || !*name) {
        return -1;
    }
    /*
     * Subdirectory paths are allowed under the SD root (DESIGN.md §10): '/' separates
     * path components. We still refuse anything that could escape the root or confuse
     * FATFS — a '\\' separator, a leading '/' (every name is root-relative), an empty
     * component ("//" or a trailing '/'), and any ".." component (parent traversal).
     */
    if (strchr(name, '\\') || name[0] == '/') {
        return -1;
    }
    for (const char *p = name; *p; ) {
        const char *seg = p;
        while (*p && *p != '/') {
            ++p;
        }
        size_t seglen = (size_t)(p - seg);
        if (seglen == 0) {                                    /* empty component */
            return -1;
        }
        if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {  /* parent traversal */
            return -1;
        }
        if (*p == '/') {
            ++p;                                              /* step over the separator */
        }
    }
    int n = snprintf(buf, len, "%s/%s", SD_MOUNT_POINT, name);
    if (n < 0 || (size_t)n >= len) {
        return -1;
    }
    return n;
}
