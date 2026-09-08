/*
 * ftp.c — FTP server driver glue (DESIGN.md §9.4).
 *
 * Wraps the vendored cooperative FTP server (lib/esp-idf-ftpServer). That server
 * is a state machine pumped by ftp_run(elapsed_ms); we own the task that pumps it.
 * Like console.c, one long-lived task on core 0 waits for the link, brings the
 * server up with the NVS credentials + SD root, and pumps it while connected; a
 * WiFi drop tears it down and it re-initialises on reconnect (so credential edges
 * — change ftpuser/ftppass + save + reboot — take effect, DESIGN.md §9.4).
 *
 * The FTP server does its own file I/O straight through the FATFS VFS. It is NOT
 * on the latency-critical FDC path (§5.2); the shared-SD contention with fdc track
 * I/O is left to the FATFS/SD layer, and the fdc task's priority keeps its ~1 s
 * command deadline (its transfers chunk naturally at one buffer per ftp_run()).
 */
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "config.h"
#include "ftp.h"
#include "ftpserver.h"
#include "net.h"
#include "sd.h"

static const char *TAG = "ftp";

/* Widen the vendored 1 KB default so an 8 MB image doesn't crawl (one buffer is
 * moved per ftp_run() tick). 4 KB keeps RAM modest while cutting the tick count. */
#define FTP_XFER_BUF 4096

static uint64_t now_ms(void)
{
    return (uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void ftp_task(void *arg)
{
    (void)arg;
    for (;;) {
        net_wait_connected(0); /* block until the link is up */

        const config_t *cfg = config_get();
        ftp_set_root(SD_MOUNT_POINT);
        ftp_set_credentials(cfg->ftp_user, cfg->ftp_pass);
        ftp_set_buffer_size(FTP_XFER_BUF);

        if (!ftp_init()) {
            ESP_LOGE(TAG, "ftp_init failed (out of memory?)");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ftp_enable();
        ESP_LOGI(TAG, "FTP server ready on :21 (user '%s', root %s)",
                 cfg->ftp_user, SD_MOUNT_POINT);

        uint64_t t = now_ms();
        while (net_is_connected()) {
            uint64_t n = now_ms();
            ftp_run((uint32_t)(n - t));
            t = n;
            vTaskDelay(1);
        }

        ESP_LOGW(TAG, "link down; stopping FTP server");
        ftp_reset();
        ftp_deinit();
    }
}

esp_err_t net_ftp_start(void)
{
    /* The vendored server logs at INFO for every command and data block, which
     * floods the shared console during transfers. Cap its tag at WARN (the upstream
     * did this in the driver task we replaced). "[Ftp]" is the server's FTP_TAG. */
    esp_log_level_set("[Ftp]", ESP_LOG_WARN);

    BaseType_t ok = xTaskCreatePinnedToCore(ftp_task, "ftp", 6144, NULL, 5, NULL, 0);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
