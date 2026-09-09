/*
 * console.c — CLI console on TCP :23, a minimal Telnet server (DESIGN.md §9.2).
 *
 * NOT a raw byte stream: real `telnet` clients (macOS/BSD, PuTTY, …) open by
 * sending option negotiation as IAC (0xFF) sequences, and some option numbers
 * fall in printable ASCII (e.g. AUTHENTICATION 37 = '%', TSPEED 32 = ' ',
 * LINEMODE 34 = '"'). A byte-at-a-time filter that only drops 0xFF lets those
 * option bytes leak into the command buffer, corrupting the FIRST line typed —
 * the "first command is always ignored" bug. So we parse Telnet properly: a
 * small IAC state machine consumes every negotiation sequence, and on connect we
 * offer WILL ECHO + WILL SUPPRESS-GO-AHEAD to put the client in character-at-a-
 * time mode. Server echo turns on ONLY once the client accepts our WILL ECHO
 * (DO ECHO); the CLI (cli.c) then drives echo and line editing, so backspace
 * erases on screen instead of showing "^H". `nc host 23` still works: it sends
 * no IAC, so echo stays off and its own cooked-mode local echo shows typing (no
 * doubling) — it just lacks the server-driven line editing a telnet client gets.
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

/* ---- Telnet (RFC 854/855) ------------------------------------------------- */
/* Just enough of the protocol to negotiate character mode and strip everything
 * the client sends back; we never depend on any option being accepted. */
#define TEL_SE   240   /* end of subnegotiation */
#define TEL_SB   250   /* begin subnegotiation */
#define TEL_WILL 251
#define TEL_WONT 252
#define TEL_DO   253
#define TEL_DONT 254
#define TEL_IAC  255   /* Interpret As Command (escape) */

#define TELOPT_ECHO 1  /* RFC 857 */
#define TELOPT_SGA  3  /* RFC 858 suppress-go-ahead → character-at-a-time mode */

typedef enum { TN_DATA, TN_IAC, TN_OPT, TN_SB, TN_SB_IAC } tn_state_t;

typedef struct {
    tn_state_t state;
    uint8_t    cmd;    /* the WILL/WONT/DO/DONT awaiting its option byte in TN_OPT */
} telnet_t;

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

/* Send one 3-byte Telnet command (IAC cmd opt). */
static void telnet_cmd(int fd, uint8_t cmd, uint8_t opt)
{
    uint8_t seq[3] = { TEL_IAC, cmd, opt };
    sock_write((void *)(intptr_t)fd, (const char *)seq, sizeof seq);
}

/* Offer to echo and to suppress go-ahead: the client stops local echo and sends
 * each keystroke immediately, so the server drives echo and line editing (§9.2). */
static void telnet_start(int fd)
{
    telnet_cmd(fd, TEL_WILL, TELOPT_ECHO);
    telnet_cmd(fd, TEL_WILL, TELOPT_SGA);
}

/*
 * Feed one received byte through the Telnet parser. Data bytes go on to the CLI
 * line editor; IAC negotiation is consumed here (and answered so a well-behaved
 * client isn't left waiting): we refuse every option the peer offers or asks us
 * to enable except the ECHO/SGA we ourselves offered, whose acceptance needs no
 * reply. Subnegotiation payloads (IAC SB … IAC SE) are discarded wholesale.
 */
static void telnet_feed(telnet_t *tn, int fd, cli_console_t *con, uint8_t b)
{
    switch (tn->state) {
    case TN_DATA:
        if (b == TEL_IAC) {
            tn->state = TN_IAC;
            return;
        }
        cli_feed(con, (char)b);
        return;

    case TN_IAC:
        switch (b) {
        case TEL_IAC:                     /* escaped 0xFF → one literal data byte */
            cli_feed(con, (char)b);
            tn->state = TN_DATA;
            return;
        case TEL_WILL: case TEL_WONT:
        case TEL_DO:   case TEL_DONT:
            tn->cmd = b;
            tn->state = TN_OPT;           /* option byte follows */
            return;
        case TEL_SB:
            tn->state = TN_SB;
            return;
        default:                          /* NOP/GA/etc. (2-byte command): consume */
            tn->state = TN_DATA;
            return;
        }

    case TN_OPT: {
        uint8_t opt = b;
        if (opt == TELOPT_ECHO && (tn->cmd == TEL_DO || tn->cmd == TEL_DONT)) {
            /* Turn server echo on ONLY when the client accepts our WILL ECHO (replies
             * DO ECHO) — a real telnet client does, and having agreed we echo it stops
             * its own local echo. `nc` never replies, so echo stays off and its cooked-
             * mode local echo shows typing with no doubling. (DONT ECHO → turn back off.) */
            con->echo = (tn->cmd == TEL_DO);
        } else if (tn->cmd == TEL_DO && opt != TELOPT_SGA) {
            telnet_cmd(fd, TEL_WONT, opt);   /* refuse options we didn't offer */
        } else if (tn->cmd == TEL_WILL) {
            telnet_cmd(fd, TEL_DONT, opt);   /* we don't need anything from the client */
        }
        /* DO SGA (accepting our offer) and other WONT/DONT need no reply. */
        tn->state = TN_DATA;
        return;
    }

    case TN_SB:
        if (b == TEL_IAC) {
            tn->state = TN_SB_IAC;
        }
        return;                           /* discard subnegotiation payload */

    case TN_SB_IAC:
        tn->state = (b == TEL_SE) ? TN_DATA : TN_SB;
        return;
    }
}

/* Serve one connected client until it disconnects or the link drops. */
static void serve_client(int fd)
{
    /* 1 s recv timeout so we can re-check the link between reads. */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    cli_console_t con;
    /* Echo starts OFF; telnet_feed() flips it on if the client accepts our WILL ECHO
     * (DO ECHO). So a real telnet client gets server-side echo + backspace editing,
     * while `nc` (which never negotiates) keeps its own local echo — no double echo. */
    cli_console_init(&con, sock_write, (void *)(intptr_t)fd, false);
    con.is_network = true; /* logout/exit + Ctrl-D may close this connection */

    telnet_t tn = { .state = TN_DATA, .cmd = 0 };
    telnet_start(fd); /* offer WILL ECHO + WILL SGA before the banner */

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
                telnet_feed(&tn, fd, &con, buf[i]);
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
    /* 8 KB: a command dispatched here (e.g. TNFS copy) runs the same deep network +
     * file-I/O paths as the serial CLI (M9). */
    xTaskCreatePinnedToCore(console_task, "netcon", 8192, NULL, 5, NULL, 0);
}
