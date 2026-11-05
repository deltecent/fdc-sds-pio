/*
 * fdc_tcp.c — FDC+ serial drive protocol over TCP: the listener (DESIGN.md §9.7).
 *
 * This task only accepts. A connected socket is handed to the fdc task
 * (fdc_tcp_attach), which does all protocol I/O on it and closes it when the client
 * goes away. One client at a time: a second connection while one is attached is
 * accepted and closed at once (counted as `refused`). On a WiFi drop the attached
 * client is dropped and the listener re-opens once the link returns.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_log.h"

#include "fdc.h"
#include "fdc_tcp.h"
#include "net.h"

static const char *TAG = "fdctcp";

/* Stall limit for a send to the client: the §6.4 data window. */
#define FDC_TCP_SEND_TIMEOUT_S 7

static uint16_t s_port;

/* Socket options for an accepted client: low latency, and notice a vanished peer. */
static void tune_client(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    int idle = 10, intvl = 5, cnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
    struct timeval tv = { .tv_sec = FDC_TCP_SEND_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

static void fdc_tcp_task(void *arg)
{
    (void)arg;
    for (;;) {
        fdc_tcp_set_state(FDC_TCP_OFF, s_port);
        net_wait_connected(0); /* block until the link is up */

        int lfd = socket(AF_INET, SOCK_STREAM, 0);
        if (lfd < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        int opt = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(s_port);
        if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(lfd, 1) < 0) {
            ESP_LOGW(TAG, "listen :%u failed: %d", s_port, errno);
            close(lfd);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        /* 1 s accept timeout so a link drop breaks us out to re-open the socket. */
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(lfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        fdc_tcp_set_state(FDC_TCP_LISTENING, s_port);
        ESP_LOGI(TAG, "FDC+ TCP listening on :%u", s_port);

        while (net_is_connected()) {
            struct sockaddr_in peer;
            socklen_t plen = sizeof peer;
            int cfd = accept(lfd, (struct sockaddr *)&peer, &plen);
            if (cfd < 0) {
                continue; /* timeout/transient: re-check the link */
            }
            char who[24];
            snprintf(who, sizeof who, "%s:%u", inet_ntoa(peer.sin_addr),
                     ntohs(peer.sin_port));
            tune_client(cfd);
            if (!fdc_tcp_attach(cfd, who)) {
                ESP_LOGI(TAG, "refused %s: a client is already attached", who);
                close(cfd);
            }
        }
        fdc_tcp_detach(); /* link down: the fdc task drops the client */
        close(lfd);
    }
}

esp_err_t net_fdc_tcp_start(uint16_t port)
{
    s_port = port;
    BaseType_t ok = xTaskCreatePinnedToCore(fdc_tcp_task, "fdctcp", 4096, NULL, 5, NULL, 0);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
