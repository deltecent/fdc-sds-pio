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
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"

#include "cli.h"
#include "config.h"

static const char *TAG = "cli";

/*
 * Serializes every writer to the UART0 console — line-editor echo, prompts and
 * command output (all via serial_write) plus ESP_LOG (via console_log_vprintf) —
 * so a keystroke echo can never land *inside* a log line. That splice was the
 * character-drop bug: an echo byte dropped into the middle of a log line's ANSI
 * color escape (\033[..m) was swallowed by the terminal's escape parser, so the
 * byte still reached the command buffer (the command ran) but never appeared on
 * screen. newlib's per-FILE lock only guards a single fwrite call, not a whole
 * log line, so it couldn't prevent this; this mutex makes each line atomic.
 */
static SemaphoreHandle_t s_tx_lock;

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

    /*
     * Tokenize in place on runs of spaces/tabs. A "double-quoted" run keeps its
     * spaces and the quotes are stripped, so a filename with a space works both
     * interactively and in batch files — e.g. `mount 1 "BLANK 8MB.DSK"` (§8.2).
     * Each token is compacted onto itself (dst never runs ahead of the read cursor
     * p, since stripping quotes only makes dst lag), so no extra buffer is needed.
     */
    char *p = line;
    while (*p && argc < CLI_MAX_ARGS) {
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (!*p) {
            break;
        }
        char *dst = p;
        argv[argc++] = dst;
        bool in_quote = false;
        while (*p) {
            if (*p == '"') {
                in_quote = !in_quote;
                ++p;
                continue;
            }
            if (!in_quote && (*p == ' ' || *p == '\t')) {
                break;
            }
            *dst++ = *p++;
        }
        if (*p) {
            ++p; /* step past the delimiter before the next token */
        }
        *dst = '\0';
    }

    if (argc == 0) {
        return; /* blank line: caller re-prompts */
    }

    const cli_command_t *cmd = cli_match(argv[0]);
    if (cmd == NULL) {
        /* Unknown token: auto-run it if it names an SD batch file (DESIGN.md §8.2). */
        if (cli_try_autorun(c, argv[0])) {
            return;
        }
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

    /* Accept printable ASCII only; silently drop all other control bytes. (The
     * network console strips Telnet IAC upstream, so none reach here — console.c.) */
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
    (void)ctx;
    /* Write through stdout (NOT uart_write_bytes) so line-ending handling matches
     * printf/ESP_LOG, and hold s_tx_lock across the whole write so this output is
     * atomic against a concurrent log line (see s_tx_lock). */
    if (s_tx_lock) {
        xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    }
    fwrite(data, 1, len, stdout);
    fflush(stdout);
    if (s_tx_lock) {
        xSemaphoreGive(s_tx_lock);
    }
}

/*
 * ESP_LOG sink: emit each whole log line under s_tx_lock so it can never be
 * interleaved with (and splice a byte out of) the line-editor echo. Registered
 * with esp_log_set_vprintf() once the console is up.
 */
static int console_log_vprintf(const char *fmt, va_list ap)
{
    if (s_tx_lock) {
        xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    }
    int n = vprintf(fmt, ap);
    if (s_tx_lock) {
        xSemaphoreGive(s_tx_lock);
    }
    return n;
}

static void cli_serial_task(void *arg)
{
    (void)arg;
    static cli_console_t con;
    cli_console_init(&con, serial_write, (void *)(intptr_t)UART_NUM_0, true);

    /* §12 step 8: run /autoexec.bat (if present) before the first prompt. */
    cli_run_autoexec(&con);

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
     * Install a driver so we can read raw bytes via uart_read_bytes, give it a TX
     * ring buffer, and route stdout/stderr through it (uart_vfs_dev_use_driver).
     * All console output — our prompt/echo (serial_write goes via stdout) AND
     * printf/ESP_LOG — then flows through the one stdout FILE, whose lock serialises
     * writers, into one driver-drained TX stream. Line-buffered so whole log lines
     * flush at once (less lock churn) while echo flushes immediately via fflush.
     * Without this, a log burst landing next to a keystroke could drop the echo. */
    if (!uart_is_driver_installed(UART_NUM_0)) {
        /* Generous rings: the cli task (prio 5, core 0) can be briefly starved by the
         * net/wifi tasks during a connect burst, so size RX to hold type-ahead without
         * overflowing and TX so a log burst never stalls keystroke echo. */
        ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 2048, 2048, 0, NULL, 0));
        uart_vfs_dev_use_driver(UART_NUM_0);
        setvbuf(stdout, NULL, _IOLBF, 256);
    }
    if (!s_tx_lock) {
        s_tx_lock = xSemaphoreCreateMutex();
        /* Route ESP_LOG through the same console lock as our echo/prompt output so
         * a keystroke echo can never be spliced into a log line (see s_tx_lock). */
        esp_log_set_vprintf(console_log_vprintf);
    }
    /* 8 KB: the CLI now runs deep paths (TNFS copy over the network stack, file I/O)
     * that the original line editor never needed (M9). */
    xTaskCreatePinnedToCore(cli_serial_task, "cli", 8192, NULL, 5, NULL, 0);
}

void cli_init(void)
{
    ESP_LOGI(TAG, "CLI init");
}
