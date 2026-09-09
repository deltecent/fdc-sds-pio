/*
 * net.h — networking: WiFi STA, Telnet console (:23), mDNS, and NTP (DESIGN.md §9).
 *
 * The `net` task family lives on core 0 (DESIGN.md §5.2) and must never touch the
 * latency-critical FDC path. WiFi connects only when enabled with an SSID set;
 * on connect it starts the TCP console, SNTP, and advertises <wifiName>.local.
 *
 * The console on port 23 is a minimal Telnet server (DESIGN.md §9.2): it negotiates
 * character-at-a-time mode with server-side echo and parses/strips IAC option
 * bytes so they never leak into the command line (console.c). `nc host 23` also
 * works — it sends no IAC and simply lacks local line editing.
 */
#ifndef FDCSDS_NET_H
#define FDCSDS_NET_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Telnet console port (DESIGN.md §9.2). The standard telnet port. */
#define NET_CONSOLE_PORT 23

/* Link-state snapshot for the `wifi` command (DESIGN.md §8.1 / §9.1). */
typedef struct {
    bool   enabled;   /* config wifiEnabled */
    bool   connected; /* STA currently has an IP */
    char   ssid[33];  /* configured SSID (WiFi max 32), or "" */
    char   ip[16];    /* dotted IPv4, or "" when not connected */
    int8_t rssi;      /* last-seen RSSI in dBm, 0 if unknown */
    uint8_t bssid[6]; /* MAC of the associated AP, all-zero if unknown */
} net_status_t;

/*
 * Bring up networking (DESIGN.md §12 step 9): netif, event loop, WiFi driver,
 * mDNS, and the raw-TCP console task; apply the saved timezone. If WiFi is enabled
 * in config with an SSID set, start connecting; otherwise stay idle until `wifi on`.
 * Returns an error only on a hard init failure — the caller keeps the serial CLI.
 */
esp_err_t net_init(void);

/*
 * Enable/disable the WiFi station live (also invoked by `wifi on|off`). Enabling
 * with no SSID configured returns ESP_ERR_INVALID_STATE. Connection completes
 * asynchronously via the event handler.
 */
esp_err_t net_wifi_enable(bool enabled);

/* Fill a status snapshot for the `wifi` command. */
void net_get_status(net_status_t *out);

/* True once the STA holds an IP (used by the TCP console task). */
bool net_is_connected(void);

/* Block until connected; timeout_ms == 0 waits forever. Returns true if connected. */
bool net_wait_connected(uint32_t timeout_ms);

/* Re-apply the timezone from config (setenv TZ + tzset). Called at boot and by `tz`. */
void net_apply_timezone(void);

/* Start the raw-TCP console listener task (:23). Called by net_init(). */
void net_console_start(void);

#ifdef __cplusplus
}
#endif

#endif /* FDCSDS_NET_H */
