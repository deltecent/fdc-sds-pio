/*
 * cli.h — command-line interface: line editor, command table, dispatch.
 *
 * The CLI is shared across consoles (serial now; raw-TCP on :23 at M5). Each
 * console carries its OWN line-editor state and output sink (DESIGN.md §5.2:
 * "no single mutable activeConsole written from two tasks" — output is routed
 * per session, never through a shared global). The line editor accepts printable
 * ASCII plus a few control keys and silently drops everything else, which also
 * discards stray Telnet IAC (0xFF) bytes without any Telnet handling (§8/§9.2).
 */
#ifndef FDCSDS_CLI_H
#define FDCSDS_CLI_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded input buffer (~80 bytes, DESIGN.md §8) and argv slots per command. */
#define CLI_LINE_MAX 80
#define CLI_MAX_ARGS 8

/* M1 prompt is fixed. M2 replaces this with wifiName + a dirty marker. */
#define CLI_PROMPT "FDC-SDS-ESP32> "

typedef struct cli_console cli_console_t;

/*
 * Output sink for one console: write `len` bytes to that console's stream.
 * `ctx` is console-specific (the UART port for serial; a socket fd for the TCP
 * console later). Kept per-console so streams are never shared/raced by tasks.
 */
typedef void (*cli_write_fn)(void *ctx, const char *data, size_t len);

struct cli_console {
    cli_write_fn write;              /* required output sink */
    void        *ctx;                /* passed back to write() */
    bool         echo;              /* echo typed chars (serial: on; TCP: off, §9.2) */
    bool         disconnect;        /* set by logout/exit + Ctrl-D; net loop honors it */
    size_t       len;               /* bytes currently in line[] */
    bool         saw_cr;            /* last byte was CR, for CRLF collapse */
    char         line[CLI_LINE_MAX + 1];
};

/* Command handler: argv[0] is the (possibly abbreviated) token the user typed. */
typedef int (*cli_cmd_fn)(cli_console_t *c, int argc, char **argv);

typedef struct {
    const char *name;                /* canonical command name */
    const char *alias;               /* single alias, or NULL */
    const char *help;                /* one-line help text */
    cli_cmd_fn  fn;
} cli_command_t;

/* The locked v1 command table (DESIGN.md §8.1), defined in commands.c. */
const cli_command_t *cli_commands(size_t *count);

/* Module init (no-op today; reserved for shared CLI state). */
void cli_init(void);

/* Prepare a console before first use. */
void cli_console_init(cli_console_t *c, cli_write_fn write, void *ctx, bool echo);

/* Feed one received byte through the line editor; may dispatch a command. */
void cli_feed(cli_console_t *c, char ch);

/* Output helpers routed to the console's own stream. */
void cli_write(cli_console_t *c, const char *s);
void cli_writen(cli_console_t *c, const char *data, size_t len);
void cli_printf(cli_console_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Emit the prompt. */
void cli_prompt(cli_console_t *c);

/* Tokenize and dispatch a NUL-terminated line (also reused by exec/batch). */
void cli_dispatch(cli_console_t *c, char *line);

/* Start the serial (UART0) console task. */
void cli_serial_start(void);

#ifdef __cplusplus
}
#endif

#endif /* FDCSDS_CLI_H */
