/*
 * commands.c — the locked v1 command table (DESIGN.md §8.1) and its handlers.
 *
 * M1 implements help/?, version, and reboot; every other command is present in
 * the table (so `help` shows the real target CLI) but wired to a stub that
 * reports "not yet implemented". Later milestones replace stubs in place:
 * M2 the SD/config/file commands, M4 the FDC commands, M5 the network/time
 * commands, M6 FTP creds, M7 update.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

#include "cli.h"
#include "version.h"

static int cmd_help(cli_console_t *c, int argc, char **argv);
static int cmd_version(cli_console_t *c, int argc, char **argv);
static int cmd_reboot(cli_console_t *c, int argc, char **argv);
static int cmd_stub(cli_console_t *c, int argc, char **argv);

/* Order here is the order `help` prints. */
static const cli_command_t k_commands[] = {
    { "help",     "?",      "Show command help",              cmd_help    },
    { "version",  NULL,     "Show firmware version",          cmd_version },
    { "baud",     NULL,     "Set FDC+ baud rate",             cmd_stub    },
    { "dir",      "ls",     "List SD root (name + size)",     cmd_stub    },
    { "mount",    NULL,     "Show mount table / mount image", cmd_stub    },
    { "unmount",  "umount", "Unmount a drive",                cmd_stub    },
    { "stats",    NULL,     "Show statistics",                cmd_stub    },
    { "clear",    NULL,     "Zero statistics",                cmd_stub    },
    { "save",     "write",  "Persist config to NVS",          cmd_stub    },
    { "wipe",     NULL,     "Erase NVS, reload defaults",     cmd_stub    },
    { "dump",     NULL,     "Hex-dump current track buffer",  cmd_stub    },
    { "wifi",     NULL,     "Show/enable/disable WiFi",       cmd_stub    },
    { "ssid",     NULL,     "Set WiFi SSID",                  cmd_stub    },
    { "pass",     NULL,     "Set WiFi password",              cmd_stub    },
    { "hostname", NULL,     "Set device/host name",           cmd_stub    },
    { "ftpuser",  NULL,     "Set FTP username",               cmd_stub    },
    { "ftppass",  NULL,     "Set FTP password",               cmd_stub    },
    { "update",   NULL,     "OTA update (SD / github / url)", cmd_stub    },
    { "type",     "cat",    "Print a text file",              cmd_stub    },
    { "exec",     "run",    "Run a batch file of commands",   cmd_stub    },
    { "logout",   "exit",   "Disconnect network client",      cmd_stub    },
    { "delete",   "rm",     "Delete a file",                  cmd_stub    },
    { "rename",   "mv",     "Rename a file",                  cmd_stub    },
    { "copy",     "cp",     "Copy a file",                    cmd_stub    },
    { "loopback", "lb",     "FDC+ serial loopback test",      cmd_stub    },
    { "time",     "date",   "Show current time",              cmd_stub    },
    { "tz",       NULL,     "Set/show timezone (tz ? lists)", cmd_stub    },
    { "reboot",   NULL,     "Clean shutdown + restart",       cmd_reboot  },
};

#define CMD_COUNT (sizeof(k_commands) / sizeof(k_commands[0]))

const cli_command_t *cli_commands(size_t *count)
{
    if (count) {
        *count = CMD_COUNT;
    }
    return k_commands;
}

static int cmd_help(cli_console_t *c, int argc, char **argv)
{
    if (argc > 1) {
        for (size_t i = 0; i < CMD_COUNT; ++i) {
            const cli_command_t *t = &k_commands[i];
            if (strcasecmp(argv[1], t->name) == 0 ||
                (t->alias && strcasecmp(argv[1], t->alias) == 0)) {
                if (t->alias) {
                    cli_printf(c, "%s (%s) - %s\r\n", t->name, t->alias, t->help);
                } else {
                    cli_printf(c, "%s - %s\r\n", t->name, t->help);
                }
                return 0;
            }
        }
        cli_printf(c, "%s: no such command\r\n", argv[1]);
        return 1;
    }

    cli_write(c, "Commands:\r\n");
    for (size_t i = 0; i < CMD_COUNT; ++i) {
        const cli_command_t *t = &k_commands[i];
        char name[24];
        if (t->alias) {
            snprintf(name, sizeof name, "%s/%s", t->name, t->alias);
        } else {
            snprintf(name, sizeof name, "%s", t->name);
        }
        cli_printf(c, "  %-16s %s\r\n", name, t->help);
    }
    return 0;
}

static int cmd_version(cli_console_t *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    cli_printf(c, "%s %s\r\n", FDCSDS_PRODUCT, FDCSDS_VERSION_STRING);
    return 0;
}

static int cmd_reboot(cli_console_t *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    cli_write(c, "Rebooting...\r\n");
    vTaskDelay(pdMS_TO_TICKS(100)); /* let the message flush before reset */
    esp_restart();
    return 0; /* not reached */
}

static int cmd_stub(cli_console_t *c, int argc, char **argv)
{
    cli_printf(c, "%s: not yet implemented\r\n", argv[0]);
    return 0;
}
