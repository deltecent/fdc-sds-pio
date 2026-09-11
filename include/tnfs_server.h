/*
 * tnfs_server.h — TNFS server: serve the SD card over the network (DESIGN.md §9.6).
 *
 * The opposite role from the TNFS client (tnfs.h, §9.5): this listens on UDP :16384
 * and lets a desktop TNFS client read/write files on the SD card, so images can be
 * staged to/from the card without a heavy FTP client. It is additive — it runs
 * alongside the FTP server (§9.4), not instead of it.
 *
 * There is NO authentication (the protocol's optional MOUNT password is plaintext and
 * almost never sent): the server answers any client that reaches the port, the same
 * no-password posture as the Telnet console (§9.2). The security control is the
 * `tnfsdEnabled` flag (§7) plus a trusted LAN — turn it off on an untrusted network.
 *
 * Like FTP it lives on core 0, off the latency-critical FDC path (§5.2), and does its
 * file I/O straight through the FATFS VFS (FATFS reentrancy handles contention with
 * fdc track I/O). It serves only while WiFi is up.
 */
#ifndef FDCSDS_TNFS_SERVER_H
#define FDCSDS_TNFS_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start the TNFS server task. It blocks until the link is up, binds the UDP socket,
 * and serves the SD root read/write; on a WiFi drop it drops all sessions and waits
 * for reconnect. Returns ESP_ERR_NO_MEM if the task can't be created. Called by
 * net_init() only when tnfsdEnabled (§7); non-fatal — the rest of networking keeps
 * working.
 */
esp_err_t net_tnfsd_start(void);

#ifdef __cplusplus
}
#endif

#endif /* FDCSDS_TNFS_SERVER_H */
