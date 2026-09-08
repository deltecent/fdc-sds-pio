/*
 * cli.c — hand-rolled line editor, command matching/dispatch, and the serial
 * (UART0) console task.
 *
 * We hand-roll the editor rather than use esp_console's REPL because that REPL
 * is single-stream (stdio/UART) and can't serve the dual serial + raw-TCP
 * "active console" model (DESIGN.md §5.3). Each console owns its buffer and
 * output sink, so nothing is shared across tasks.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

#include "cli.h"
#include "config.h"

static const char *TAG = "cli";

/* Sentinel returned by cli_match() when a token matches more than one command. */
#define CLI_AMBIGUOUS ((const cli_command_t *)-1)

/* ---- output helpers -------------------------------------------------------- */

void cli_writen(cli_console_t *c, const char *data, size_t len)
{
    if (c->write && len) {
        c->write(c->ctx, data, len);
    }
}

void cli_write(cli_console_t *c, const char *s)
{
    cli_writen(c, s, strlen(s));
}

void cli_printf(cli_console_t *c, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    size_t len = (n < (int)sizeof buf) ? (size_t)n : sizeof buf - 1;
    cli_writen(c, buf, len);
}

void cli_prompt(cli_console_t *c)
{
    char prompt[CLI_PROMPT_MAX];
    config_prompt(prompt, sizeof prompt);
    cli_write(c, prompt);
}

/* ---- command matching ------------------------------------------------------ */

/*
 * Match one token to a command: an exact (case-insensitive) name/alias match
 * wins; otherwise a unique case-insensitive prefix matches (abbreviations,
 * DESIGN.md §8). Returns NULL if nothing matches, CLI_AMBIGUOUS if >1 prefix.
 */
static const cli_command_t *cli_match(const char *tok)
{
    size_t n;
    const cli_command_t *t = cli_commands(&n);

    for (size_t i = 0; i < n; ++i) {
        if (strcasecmp(tok, t[i].name) == 0) {
            return &t[i];
        }
        if (t[i].alias && strcasecmp(tok, t[i].alias) == 0) {
            return &t[i];
        }
    }

    const cli_command_t *hit = NULL;
    int count = 0;
    size_t len = strlen(tok);
    for (size_t i = 0; i < n; ++i) {
        if (strncasecmp(tok, t[i].name, len) == 0 ||
            (t[i].alias && strncasecmp(tok, t[i].alias, len) == 0)) {
            hit = &t[i];
            ++count;
        }
    }
    if (count == 1) {
        return hit;
    }
    if (count > 1) {
        return CLI_AMBIGUOUS;
    }
    return NULL;
}

void cli_dispatch(cli_console_t *c, char *line)
{
    char *argv[CLI_MAX_ARGS];
    int argc = 0;

    /* Tokenize in place on runs of spaces/tabs. */
    char *p = line;
    while (*p && argc < CLI_MAX_ARGS) {
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (!*p) {
            break;
        }
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') {
            ++p;
        }
        if (*p) {
            *p++ = '\0';
        }
    }

    if (argc == 0) {
        return; /* blank line: caller re-prompts */
    }

    const cli_command_t *cmd = cli_match(argv[0]);
    if (cmd == NULL) {
        cli_printf(c, "%s: unknown command (try 'help')\r\n", argv[0]);
        return;
    }
    if (cmd == CLI_AMBIGUOUS) {
        cli_printf(c, "%s: ambiguous command (try 'help')\r\n", argv[0]);
        return;
    }
    cmd->fn(c, argc, argv);
}

/* ---- line editor ----------------------------------------------------------- */

void cli_console_init(cli_console_t *c, cli_write_fn write, void *ctx, bool echo)
{
    memset(c, 0, sizeof *c);
    c->write = write;
    c->ctx = ctx;
    c->echo = echo;
}

void cli_feed(cli_console_t *c, char ch)
{
    unsigned char b = (unsigned char)ch;

    /* End of line on CR or LF; collapse a CRLF (or LFCR) pair into one. */
    if (b == '\r' || b == '\n') {
        if (b == '\n' && c->saw_cr) {
            c->saw_cr = false;
            return;
        }
        c->saw_cr = (b == '\r');
        cli_writen(c, "\r\n", 2);          /* always move output to a fresh line */
        c->line[c->len] = '\0';
        cli_dispatch(c, c->line);
        c->len = 0;
        if (!c->disconnect) {
            cli_prompt(c);
        }
        return;
    }
    c->saw_cr = false;

    /* Backspace / DEL: erase the last buffered character. */
    if (b == '\b' || b == 0x7F) {
        if (c->len > 0) {
            c->len--;
            if (c->echo) {
                cli_writen(c, "\b \b", 3);
            }
        }
        return;
    }

    /* Ctrl-D requests disconnect (honored by the network console; serial ignores). */
    if (b == 0x04) {
        c->disconnect = true;
        return;
    }

    /* Accept printable ASCII only; silently drop all else (incl. IAC 0xFF). */
    if (b >= 0x20 && b <= 0x7E) {
        if (c->len < CLI_LINE_MAX) {
            c->line[c->len++] = (char)b;
            if (c->echo) {
                cli_writen(c, (const char *)&b, 1);
            }
        }
        /* buffer full: drop the excess byte */
        return;
    }
    /* every other byte is dropped */
}

/* ---- serial console task --------------------------------------------------- */

static void serial_write(void *ctx, const char *data, size_t len)
{
    uart_port_t port = (uart_port_t)(intptr_t)ctx;
    uart_write_bytes(port, data, len);
}

static void cli_serial_task(void *arg)
{
    (void)arg;
    static cli_console_t con;
    cli_console_init(&con, serial_write, (void *)(intptr_t)UART_NUM_0, true);

    cli_prompt(&con);

    for (;;) {
        uint8_t b;
        int n = uart_read_bytes(UART_NUM_0, &b, 1, portMAX_DELAY);
        if (n == 1) {
            cli_feed(&con, (char)b);
        }
    }
}

void cli_serial_start(void)
{
    /* UART0 is already the console (configured by the bootloader/menuconfig).
     * Install a driver so we can read raw bytes via uart_read_bytes; printf and
     * ESP_LOG keep writing over the same port. RX buffer only (no TX ring). */
    if (!uart_is_driver_installed(UART_NUM_0)) {
        ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0));
    }
    xTaskCreatePinnedToCore(cli_serial_task, "cli", 4096, NULL, 5, NULL, 0);
}

void cli_init(void)
{
    ESP_LOGI(TAG, "CLI init");
}
