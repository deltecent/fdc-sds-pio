/*
 * console.c — raw-TCP CLI console on port 23 (DESIGN.md §9.2).
 *
 * A plain byte-stream socket, NOT the Telnet protocol: no IAC/option negotiation.
 * The line editor (cli.c) drops stray IAC (0xFF) bytes, so an ordinary `telnet`
 * client works; `nc host 23` works identically. Server echo is OFF (§8) — the
 * client echoes locally.
 *
 * One long-lived task owns the listener: it waits for the link, accepts a single
 * client at a time (multiple simultaneous clients are out of v1 scope, §1), and
 * serves it through the shared CLI with a per-socket output sink — no shared
 * "active console" global (DESIGN.md §5.2). Short socket timeouts let the loop
 * notice a WiFi drop and tear the client down promptly.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"

#include "cli.h"
#include "net.h"
#include "version.h"

static const char *TAG = "netcon";

/* Output sink for one client: write all bytes to its socket (ctx = fd). */
static void sock_write(void *ctx, const char *data, size_t len)
{
    int fd = (int)(intptr_t)ctx;
    size_t off = 0;
    while (off < len) {
        int n = send(fd, data + off, len - off, 0);
        if (n <= 0) {
            return; /* peer gone; the read loop will notice and close */
        }
        off += (size_t)n;
    }
}

/* Serve one connected client until it disconnects or the link drops. */
static void serve_client(int fd)
{
    /* 1 s recv timeout so we can re-check the link between reads. */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    cli_console_t con;
    cli_console_init(&con, sock_write, (void *)(intptr_t)fd, false); /* echo off, §9.2 */

    char banner[96];
    int n = snprintf(banner, sizeof banner, "\r\n%s %s\r\n\r\n",
                     FDCSDS_PRODUCT, FDCSDS_VERSION_STRING);
    sock_write((void *)(intptr_t)fd, banner, (size_t)n);
    cli_prompt(&con);

    uint8_t buf[64];
    while (net_is_connected() && !con.disconnect) {
        int r = recv(fd, buf, sizeof buf, 0);
        if (r > 0) {
            for (int i = 0; i < r; ++i) {
                cli_feed(&con, (char)buf[i]);
                if (con.disconnect) {
                    break;
                }
            }
        } else if (r == 0) {
            break; /* client closed */
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break; /* real error (not the recv timeout) */
        }
    }
    close(fd);
}

static void console_task(void *arg)
{
    (void)arg;
    for (;;) {
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
        addr.sin_port = htons(NET_CONSOLE_PORT);
        if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(lfd, 1) < 0) {
            ESP_LOGW(TAG, "listen :%d failed: %d", NET_CONSOLE_PORT, errno);
            close(lfd);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        /* 1 s accept timeout so a link drop breaks us out to re-open the socket. */
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(lfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        ESP_LOGI(TAG, "console listening on :%d", NET_CONSOLE_PORT);

        while (net_is_connected()) {
            int cfd = accept(lfd, NULL, NULL);
            if (cfd < 0) {
                continue; /* timeout/transient: re-check the link */
            }
            ESP_LOGI(TAG, "console client connected");
            serve_client(cfd);
            ESP_LOGI(TAG, "console client disconnected");
        }
        close(lfd);
    }
}

void net_console_start(void)
{
    xTaskCreatePinnedToCore(console_task, "netcon", 4096, NULL, 5, NULL, 0);
}
