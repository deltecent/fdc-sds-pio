/*
 * ftp.h — project glue for the vendored FTP server (DESIGN.md §9.4).
 *
 * Thin API over lib/esp-idf-ftpServer (ftpserver.h) so the rest of the firmware
 * depends on our types, not the library's — the component can be swapped or
 * hand-rolled later without touching callers (DESIGN.md §5.3). The server serves
 * the SD mount on TCP :21 with the NVS login (ftpUser/ftpPass, default fdc/fdc),
 * and runs only while WiFi is up — like the TCP console, it lives on core 0 and
 * never touches the FDC path (DESIGN.md §5.2).
 */
#ifndef FDCSDS_FTP_H
#define FDCSDS_FTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start the FTP driver task. It blocks until the link is up, applies the current
 * credentials + SD root, then drives the server; on a WiFi drop it tears the server
 * down and waits for reconnect. Returns ESP_ERR_NO_MEM if the task can't be created.
 * Called by net_init(); non-fatal — the rest of networking keeps working.
 */
esp_err_t net_ftp_start(void);

#ifdef __cplusplus
}
#endif

#endif /* FDCSDS_FTP_H */
