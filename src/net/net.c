/*
 * net.c — WiFi station, mDNS, and NTP (DESIGN.md §9.1 / §12 step 9).
 *
 * We keep WiFi/lwIP on core 0 and off the FDC path (DESIGN.md §5.2). Credentials
 * live in our own NVS config (§7), so WiFi driver storage is set to RAM — the
 * driver never writes its own copy that could shadow ours. Connection is fully
 * event-driven: STA_START -> connect, GOT_IP -> start services + advertise,
 * DISCONNECTED -> drop the CONNECTED bit and auto-reconnect while enabled.
 *
 * A CONNECTED event bit is the single source of truth for "the link is up"; the
 * TCP console (console.c) waits on it and drops its client when it clears.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mdns.h"

#include "net.h"
#include "config.h"
#include "disk.h"
#include "ftp.h"
#include "tnfs_proto.h" /* TNFS_DEFAULT_PORT for the mDNS advert (§9.6) */
#include "tnfs_server.h"

static const char *TAG = "net";

#define NET_CONNECTED_BIT BIT0

static esp_netif_t      *s_sta_netif;
static EventGroupHandle_t s_events;
static bool s_wifi_started; /* esp_wifi_start() done, so auto-reconnect is wanted */
static bool s_sntp_inited;
static bool s_mdns_inited;
static char s_ip[16];       /* written on GOT_IP, read by net_get_status (benign race) */

/* ---- timezone (DESIGN.md §8.3) --------------------------------------------- */

void net_apply_timezone(void)
{
    const char *tz = config_get()->time_zone;
    if (!tz || !tz[0]) {
        tz = "UTC0";
    }
    setenv("TZ", tz, 1);
    tzset();
}

/* ---- services started once the link is up ---------------------------------- */

static void start_sntp(void)
{
    if (s_sntp_inited) {
        return; /* keeps running across reconnects */
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&cfg); /* default config starts it */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sntp init: %s", esp_err_to_name(err));
        return;
    }
    s_sntp_inited = true;
    ESP_LOGI(TAG, "SNTP started (pool.ntp.org)");
}

/* ---- WiFi + IP events ------------------------------------------------------- */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            /* Hostname must be set on a started netif; do it just before connect. */
            esp_netif_set_hostname(s_sta_netif, config_get()->wifi_name);
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            xEventGroupClearBits(s_events, NET_CONNECTED_BIT);
            s_ip[0] = '\0';
            /* Drop remote (tnfs://) drives to not-ready; they re-mount on GOT_IP
             * (DESIGN.md §10.1/§12 step 9). */
            disk_remote_unmount_all();
            if (s_wifi_started) {
                esp_wifi_connect(); /* auto-reconnect (DESIGN.md §9.1) */
            }
            break;
        default:
            break;
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        esp_ip4addr_ntoa(&e->ip_info.ip, s_ip, sizeof s_ip);
        ESP_LOGI(TAG, "connected, IP %s", s_ip);
        xEventGroupSetBits(s_events, NET_CONNECTED_BIT);
        start_sntp();
        /* Mount any deferred tnfs:// drives now that the link is up (§10.1/§12 step 9);
         * also re-mounts them after a reconnect. */
        disk_remote_mount_all();
    }
}

/* ---- public API ------------------------------------------------------------ */

esp_err_t net_wifi_enable(bool enabled)
{
    if (!enabled) {
        s_wifi_started = false; /* suppress auto-reconnect before we tear down */
        esp_wifi_disconnect();
        esp_wifi_stop();
        xEventGroupClearBits(s_events, NET_CONNECTED_BIT);
        s_ip[0] = '\0';
        return ESP_OK;
    }

    const config_t *cfg = config_get();
    if (cfg->wifi_ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE; /* nothing to connect to yet */
    }

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, cfg->wifi_ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, cfg->wifi_pass, sizeof wc.sta.password);
    /* Pick the strongest AP when an SSID is meshed across several (DESIGN.md §9.1). */
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    /* Minimum AP security. With a password, require WPA2 so we can't be lured onto an
     * open/WEP "evil-twin" AP advertising our SSID; a blank password means a genuinely
     * open network. Setting this explicitly also stops the driver auto-raising it from
     * OPEN and logging a warning every connect. */
    wc.sta.threshold.authmode = cfg->wifi_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        return err;
    }

    if (!s_wifi_started) {
        err = esp_wifi_start(); /* STA_START handler issues the connect */
        if (err != ESP_OK) {
            return err;
        }
        s_wifi_started = true;
    } else {
        /* Already up with new creds: reconnect. */
        esp_wifi_disconnect();
        esp_wifi_connect();
    }
    return ESP_OK;
}

void net_get_status(net_status_t *out)
{
    if (!out) {
        return;
    }
    const config_t *cfg = config_get();
    memset(out, 0, sizeof *out);
    out->enabled = cfg->wifi_enabled;
    out->connected = s_events && (xEventGroupGetBits(s_events) & NET_CONNECTED_BIT);
    strlcpy(out->ssid, cfg->wifi_ssid, sizeof out->ssid);
    if (out->connected) {
        strlcpy(out->ip, s_ip, sizeof out->ip);
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->rssi = ap.rssi;
            memcpy(out->bssid, ap.bssid, sizeof out->bssid); /* which AP we picked */
        }
    }
}

bool net_is_connected(void)
{
    return s_events && (xEventGroupGetBits(s_events) & NET_CONNECTED_BIT);
}

bool net_wait_connected(uint32_t timeout_ms)
{
    if (!s_events) {
        return false;
    }
    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t b = xEventGroupWaitBits(s_events, NET_CONNECTED_BIT, pdFALSE, pdTRUE, ticks);
    return (b & NET_CONNECTED_BIT) != 0;
}

esp_err_t net_init(void)
{
    /* Apply the saved timezone up front so `time` renders local wall-clock even
     * before NTP (DESIGN.md §8.3). */
    net_apply_timezone();

    s_events = xEventGroupCreate();
    if (!s_events) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { /* may already exist */
        return err;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        return ESP_FAIL;
    }

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&ic);
    if (err != ESP_OK) {
        return err;
    }
    /* Our config owns the credentials (§7); don't let the driver persist its own. */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, NULL, NULL));

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }

    const config_t *cfg = config_get();

    /* mDNS: advertise <wifiName>.local + the enabled services (DESIGN.md §9.1).
     * Harmless on a LAN without mDNS. A disabled service is not advertised. */
    if (mdns_init() == ESP_OK) {
        s_mdns_inited = true;
        mdns_hostname_set(cfg->wifi_name);
        mdns_instance_name_set("FDC+ Serial Disk Server");
        if (cfg->telnet_enabled) {
            mdns_service_add(NULL, "_telnet", "_tcp", NET_CONSOLE_PORT, NULL, 0);
        }
        if (cfg->ftp_enabled) {
            mdns_service_add(NULL, "_ftp", "_tcp", 21, NULL, 0); /* FTP server (§9.4) */
        }
        if (cfg->tnfsd_enabled) {
            mdns_service_add(NULL, "_tnfs", "_udp", TNFS_DEFAULT_PORT, NULL, 0); /* §9.6 */
        }
    } else {
        ESP_LOGW(TAG, "mDNS init failed (name discovery unavailable)");
    }

    /* Serve the TCP console + FTP whenever the link is up (tasks block until
     * connected). Both live on core 0, off the FDC path (DESIGN.md §5.2). Each
     * runs only when enabled in config; the flag is read once here, so a toggle
     * takes effect on the next reboot (DESIGN.md §7/§9.2). */
    if (cfg->telnet_enabled) {
        net_console_start();
    } else {
        ESP_LOGI(TAG, "Telnet console disabled in config");
    }
    if (cfg->ftp_enabled) {
        if (net_ftp_start() != ESP_OK) {
            ESP_LOGW(TAG, "FTP task not started (out of memory)");
        }
    } else {
        ESP_LOGI(TAG, "FTP server disabled in config");
    }
    if (cfg->tnfsd_enabled) {
        if (net_tnfsd_start() != ESP_OK) {
            ESP_LOGW(TAG, "TNFS server task not started (out of memory)");
        }
    } else {
        ESP_LOGI(TAG, "TNFS server disabled in config");
    }

    /* Start connecting now if enabled and configured (DESIGN.md §12 step 9). */
    if (cfg->wifi_enabled && cfg->wifi_ssid[0]) {
        err = net_wifi_enable(true);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wifi auto-start: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "WiFi idle (%s)",
                 cfg->wifi_enabled ? "no SSID set" : "disabled in config");
    }
    return ESP_OK;
}
