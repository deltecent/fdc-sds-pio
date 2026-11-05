/*
 * fdc_tcp.h — FDC+ serial drive protocol over a TCP socket (DESIGN.md §9.7).
 *
 * A listener for a network FDC+ (e.g. the altairsim `fdcplus` board with
 * `connect = "socket:HOST:PORT"`). The socket carries exactly the §6 byte stream —
 * no header, handshake, or Telnet layer. One client at a time; while it is attached
 * the fdc task serves it and ignores the UART, then falls back to the UART when it
 * disconnects.
 */
#ifndef FDCSDS_FDC_TCP_H
#define FDCSDS_FDC_TCP_H

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start the listener task on `port`. It blocks until the link is up, then accepts
 * clients and hands each to the fdc task (fdc_tcp_attach). Called by net_init() only
 * when fdcTcpPort (§7) is nonzero. Returns ESP_ERR_NO_MEM if the task can't be created.
 */
esp_err_t net_fdc_tcp_start(uint16_t port);

#ifdef __cplusplus
}
#endif

#endif /* FDCSDS_FDC_TCP_H */
