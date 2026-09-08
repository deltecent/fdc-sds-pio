/*
 * commands.c — the locked v1 command table (DESIGN.md §8.1) and its handlers.
 *
 * M1 implemented help/?, version, reboot. M2 added the SD/config commands:
 * dir/ls, type/cat, delete/rm, rename/mv, copy/cp, save/write, wipe, hostname,
 * and baud (store-only until the FDC UART lands at M4). M3 adds the disk-module
 * commands mount/unmount and dump, plus the `dir <spec>` glob (§8.4). The remaining
 * commands stay stubbed and are filled in by later milestones. M4 wires the FDC+
 * engine into the CLI: stats/clear, loopback, and live baud application. M5 added the
 * net/time commands (wifi/ssid/pass/time/tz/logout) and M6 the FTP credentials
 * (ftpuser/ftppass). M7 wires in `update` (SD / network OTA, §11). M8 fills in the last
 * stubbed command, `exec`/`run` (batch files, §8.2) — plus the `.bat` auto-run and
 * boot-time /autoexec.bat hooks the CLI dispatcher and serial task call into — and adds
 * `log` for the runtime console-verbosity knob (§13).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"

#include "cli.h"
#include "config.h"
#include "disk.h"
#include "fdc.h"
#include "net.h"
#include "ota.h"
#include "sd.h"
#include "version.h"
#include "wildcard.h"

static int cmd_help(cli_console_t *c, int argc, char **argv);
static int cmd_version(cli_console_t *c, int argc, char **argv);
static int cmd_reboot(cli_console_t *c, int argc, char **argv);
static int cmd_dir(cli_console_t *c, int argc, char **argv);
static int cmd_mount(cli_console_t *c, int argc, char **argv);
static int cmd_unmount(cli_console_t *c, int argc, char **argv);
static int cmd_dump(cli_console_t *c, int argc, char **argv);
static int cmd_type(cli_console_t *c, int argc, char **argv);
static int cmd_delete(cli_console_t *c, int argc, char **argv);
static int cmd_rename(cli_console_t *c, int argc, char **argv);
static int cmd_copy(cli_console_t *c, int argc, char **argv);
static int cmd_save(cli_console_t *c, int argc, char **argv);
static int cmd_wipe(cli_console_t *c, int argc, char **argv);
static int cmd_hostname(cli_console_t *c, int argc, char **argv);
static int cmd_baud(cli_console_t *c, int argc, char **argv);
static int cmd_stats(cli_console_t *c, int argc, char **argv);
static int cmd_clear(cli_console_t *c, int argc, char **argv);
static int cmd_log(cli_console_t *c, int argc, char **argv);
static int cmd_loopback(cli_console_t *c, int argc, char **argv);
static int cmd_wifi(cli_console_t *c, int argc, char **argv);
static int cmd_ssid(cli_console_t *c, int argc, char **argv);
static int cmd_pass(cli_console_t *c, int argc, char **argv);
static int cmd_time(cli_console_t *c, int argc, char **argv);
static int cmd_tz(cli_console_t *c, int argc, char **argv);
static int cmd_logout(cli_console_t *c, int argc, char **argv);
static int cmd_ftpuser(cli_console_t *c, int argc, char **argv);
static int cmd_ftppass(cli_console_t *c, int argc, char **argv);
static int cmd_update(cli_console_t *c, int argc, char **argv);
static int cmd_exec(cli_console_t *c, int argc, char **argv);

/* Order here is the order `help` prints. */
static const cli_command_t k_commands[] = {
    { "help",     "?",      "Show command help",              cmd_help     },
    { "version",  NULL,     "Show firmware version",          cmd_version  },
    { "baud",     NULL,     "Set FDC+ baud rate",             cmd_baud     },
    { "dir",      "ls",     "List SD files [glob spec]",      cmd_dir      },
    { "mount",    NULL,     "Show mount table / mount image", cmd_mount    },
    { "unmount",  "umount", "Unmount a drive",                cmd_unmount  },
    { "stats",    NULL,     "Show FDC+ statistics",           cmd_stats    },
    { "clear",    NULL,     "Zero FDC+ statistics",           cmd_clear    },
    { "log",      NULL,     "Set console log level",          cmd_log      },
    { "save",     "write",  "Persist config to NVS",          cmd_save     },
    { "wipe",     NULL,     "Erase NVS, reload defaults",     cmd_wipe     },
    { "dump",     NULL,     "Hex-dump a track buffer",        cmd_dump     },
    { "wifi",     NULL,     "Show/enable/disable WiFi",       cmd_wifi     },
    { "ssid",     NULL,     "Set WiFi SSID",                  cmd_ssid     },
    { "pass",     NULL,     "Set WiFi password",              cmd_pass     },
    { "hostname", NULL,     "Set device/host name",           cmd_hostname },
    { "ftpuser",  NULL,     "Set FTP username",               cmd_ftpuser  },
    { "ftppass",  NULL,     "Set FTP password",               cmd_ftppass  },
    { "update",   NULL,     "Firmware: status, or 'local'/'ota' to install", cmd_update },
    { "type",     "cat",    "Print a text file",              cmd_type     },
    { "exec",     "run",    "Run a batch file of commands",   cmd_exec     },
    { "logout",   "exit",   "Disconnect network client",      cmd_logout   },
    { "delete",   "rm",     "Delete a file",                  cmd_delete   },
    { "rename",   "mv",     "Rename a file",                  cmd_rename   },
    { "copy",     "cp",     "Copy a file",                    cmd_copy     },
    { "loopback", "lb",     "FDC+ serial loopback test",      cmd_loopback },
    { "time",     "date",   "Show current time",              cmd_time     },
    { "tz",       NULL,     "Set/show timezone (tz ? lists)", cmd_tz       },
    { "reboot",   NULL,     "Clean shutdown + restart",       cmd_reboot   },
};

#define CMD_COUNT (sizeof(k_commands) / sizeof(k_commands[0]))

/* FDC+ baud rates the link supports (DESIGN.md §6.1). */
static const uint32_t k_bauds[] = {
    9600, 19200, 38400, 57600, 76800, 115200, 230400, 403200, 460800,
};
#define BAUD_COUNT (sizeof(k_bauds) / sizeof(k_bauds[0]))

const cli_command_t *cli_commands(size_t *count)
{
    if (count) {
        *count = CMD_COUNT;
    }
    return k_commands;
}

/* ---- helpers --------------------------------------------------------------- */

/* Resolve an SD filename arg to an absolute path; report and return false on error. */
static bool resolve(cli_console_t *c, const char *name, char *buf, size_t len)
{
    if (!sd_mounted()) {
        cli_write(c, "no SD card\r\n");
        return false;
    }
    if (sd_path(name, buf, len) < 0) {
        cli_printf(c, "%s: invalid filename\r\n", name);
        return false;
    }
    return true;
}

/* ---- handlers -------------------------------------------------------------- */

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

static int cmd_dir(cli_console_t *c, int argc, char **argv)
{
    /* Optional glob filter, e.g. `dir *.BAT` (DESIGN.md §8.4). */
    const char *spec = (argc > 1) ? argv[1] : NULL;
    const bool spec_is_dot = spec && spec[0] == '.';

    if (!sd_mounted()) {
        cli_write(c, "no SD card\r\n");
        return 1;
    }

    DIR *d = opendir(SD_MOUNT_POINT);
    if (!d) {
        cli_printf(c, "dir: %s\r\n", strerror(errno));
        return 1;
    }

    unsigned files = 0;
    uint32_t total = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        /* Hide dotfiles (DESIGN.md §8.1) unless the spec itself starts with '.' (§8.4). */
        if (e->d_name[0] == '.' && !spec_is_dot) {
            continue;
        }
        if (spec && !wildcard_match_ci(spec, e->d_name)) {
            continue;
        }
        char path[16 + CONFIG_FILE_CAP];
        if (sd_path(e->d_name, path, sizeof path) < 0) {
            continue;
        }
        struct stat st;
        if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
            continue; /* flat root: skip directories */
        }
        cli_printf(c, "%10lu  %s\r\n", (unsigned long)st.st_size, e->d_name);
        total += (uint32_t)st.st_size;
        ++files;
    }
    closedir(d);

    cli_printf(c, "%u file(s), %lu bytes\r\n", files, (unsigned long)total);
    return 0;
}

/* Map a disk_mount() error code to a friendly console message. */
static void report_mount_err(cli_console_t *c, const char *name, esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_STATE: cli_write(c, "no SD card\r\n"); break;
    case ESP_ERR_INVALID_ARG:   cli_printf(c, "%s: invalid filename\r\n", name); break;
    case ESP_ERR_NOT_FOUND:     cli_printf(c, "%s: not found\r\n", name); break;
    case ESP_ERR_NO_MEM:        cli_write(c, "out of memory\r\n"); break;
    default: cli_printf(c, "mount failed: %s\r\n", esp_err_to_name(err)); break;
    }
}

/* Parse a drive-number arg into 0..MAX_DRIVE-1; report and return -1 on error. */
static int parse_drive(cli_console_t *c, const char *arg)
{
    char *end;
    long drive = strtol(arg, &end, 10);
    if (*end != '\0' || drive < 0 || drive >= CONFIG_MAX_DRIVE) {
        cli_printf(c, "drive must be 0..%d\r\n", CONFIG_MAX_DRIVE - 1);
        return -1;
    }
    return (int)drive;
}

static int cmd_mount(cli_console_t *c, int argc, char **argv)
{
    if (argc < 2) {
        for (int i = 0; i < CONFIG_MAX_DRIVE; ++i) {
            if (disk_is_mounted(i)) {
                cli_printf(c, "%d: %s (%lu bytes)\r\n", i, disk_name(i),
                           (unsigned long)disk_image_size(i));
            } else {
                cli_printf(c, "%d: (empty)\r\n", i);
            }
        }
        return 0;
    }
    if (argc < 3) {
        cli_write(c, "usage: mount <drive> <file>\r\n");
        return 1;
    }

    int drive = parse_drive(c, argv[1]);
    if (drive < 0) {
        return 1;
    }

    esp_err_t err = disk_mount(drive, argv[2]);
    if (err != ESP_OK) {
        report_mount_err(c, argv[2], err);
        return 1;
    }
    /* Persist the mount so `save` + reboot auto-mounts it (DESIGN.md §7/§12). */
    config_set_drive(drive, argv[2]);
    cli_printf(c, "drive %d: %s (%lu bytes)\r\n", drive, argv[2],
               (unsigned long)disk_image_size(drive));
    return 0;
}

static int cmd_unmount(cli_console_t *c, int argc, char **argv)
{
    if (argc < 2) {
        cli_write(c, "usage: unmount <drive>\r\n");
        return 1;
    }
    int drive = parse_drive(c, argv[1]);
    if (drive < 0) {
        return 1;
    }
    if (!disk_is_mounted(drive)) {
        cli_printf(c, "drive %d not mounted\r\n", drive);
        return 1;
    }
    disk_unmount(drive);
    config_set_drive(drive, NULL);
    cli_printf(c, "drive %d unmounted\r\n", drive);
    return 0;
}

#define DUMP_DEFAULT_LEN 256

/* One 16-byte hex+ASCII line (xxd-like, for eyeball cross-checks against `xxd`). */
static void dump_line(cli_console_t *c, uint32_t off, const uint8_t *p, size_t n)
{
    char line[96];
    int k = snprintf(line, sizeof line, "%08lx: ", (unsigned long)off);
    for (size_t i = 0; i < 16; ++i) {
        if (i < n) {
            k += snprintf(line + k, sizeof line - (size_t)k, "%02x ", p[i]);
        } else {
            k += snprintf(line + k, sizeof line - (size_t)k, "   ");
        }
    }
    line[k++] = ' ';
    for (size_t i = 0; i < n; ++i) {
        line[k++] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '.';
    }
    line[k++] = '\r';
    line[k++] = '\n';
    cli_writen(c, line, (size_t)k);
}

static int cmd_dump(cli_console_t *c, int argc, char **argv)
{
    /*
     * Normally the FDC READ/WRIT path fills the shared track buffer and `dump` shows
     * it. Until the FDC engine lands (M4) — and to cross-check offset math against
     * `xxd` — `dump [drive [track [len]]]` reads a track first (defaults: drive 0,
     * track 0, 256 bytes). Bare `dump` shows the current buffer, or reads the default
     * if the buffer is still empty.
     */
    if (argc > 1) {
        int drive = parse_drive(c, argv[1]);
        if (drive < 0) {
            return 1;
        }
        uint32_t track = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 0) : 0;
        size_t len = (argc > 3) ? (size_t)strtoul(argv[3], NULL, 0) : DUMP_DEFAULT_LEN;
        esp_err_t err = disk_read_track(drive, track, len);
        if (err != ESP_OK) {
            cli_printf(c, "dump: read failed: %s\r\n", esp_err_to_name(err));
            return 1;
        }
    }

    int drive;
    uint32_t track;
    size_t len;
    const uint8_t *buf = disk_track_buffer(&drive, &track, &len);
    if (len == 0) {
        /* Nothing cached and no args — read the default track from drive 0. */
        if (disk_read_track(0, 0, DUMP_DEFAULT_LEN) != ESP_OK) {
            cli_write(c, "dump: no track loaded (mount a drive first)\r\n");
            return 1;
        }
        buf = disk_track_buffer(&drive, &track, &len);
    }

    cli_printf(c, "drive %d track %lu, %lu bytes:\r\n", drive, (unsigned long)track,
               (unsigned long)len);
    for (size_t off = 0; off < len; off += 16) {
        size_t n = (len - off < 16) ? (len - off) : 16;
        dump_line(c, (uint32_t)off, buf + off, n);
    }
    return 0;
}

static int cmd_type(cli_console_t *c, int argc, char **argv)
{
    if (argc < 2) {
        cli_write(c, "usage: type <file>\r\n");
        return 1;
    }
    char path[16 + CONFIG_FILE_CAP];
    if (!resolve(c, argv[1], path, sizeof path)) {
        return 1;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        cli_printf(c, "%s: %s\r\n", argv[1], strerror(errno));
        return 1;
    }

    /* Stream to the console, normalizing bare LF to CRLF for a raw terminal. */
    char buf[256];
    size_t n;
    char prev = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        for (size_t i = 0; i < n; ++i) {
            char ch = buf[i];
            if (ch == '\n' && prev != '\r') {
                cli_writen(c, "\r\n", 2);
            } else {
                cli_writen(c, &ch, 1);
            }
            prev = ch;
        }
    }
    if (prev != 0 && prev != '\n') {
        cli_writen(c, "\r\n", 2); /* ensure output ends on a fresh line */
    }
    fclose(f);
    return 0;
}

static int cmd_delete(cli_console_t *c, int argc, char **argv)
{
    if (argc < 2) {
        cli_write(c, "usage: delete <file>\r\n");
        return 1;
    }
    char path[16 + CONFIG_FILE_CAP];
    if (!resolve(c, argv[1], path, sizeof path)) {
        return 1;
    }
    if (unlink(path) != 0) {
        cli_printf(c, "%s: %s\r\n", argv[1], strerror(errno));
        return 1;
    }
    cli_printf(c, "deleted %s\r\n", argv[1]);
    return 0;
}

static int cmd_rename(cli_console_t *c, int argc, char **argv)
{
    if (argc < 3) {
        cli_write(c, "usage: rename <old> <new>\r\n");
        return 1;
    }
    char from[16 + CONFIG_FILE_CAP];
    char to[16 + CONFIG_FILE_CAP];
    if (!resolve(c, argv[1], from, sizeof from) ||
        !resolve(c, argv[2], to, sizeof to)) {
        return 1;
    }
    if (rename(from, to) != 0) {
        cli_printf(c, "rename: %s\r\n", strerror(errno));
        return 1;
    }
    cli_printf(c, "renamed %s -> %s\r\n", argv[1], argv[2]);
    return 0;
}

static int cmd_copy(cli_console_t *c, int argc, char **argv)
{
    if (argc < 3) {
        cli_write(c, "usage: copy <src> <dst>\r\n");
        return 1;
    }
    char src[16 + CONFIG_FILE_CAP];
    char dst[16 + CONFIG_FILE_CAP];
    if (!resolve(c, argv[1], src, sizeof src) ||
        !resolve(c, argv[2], dst, sizeof dst)) {
        return 1;
    }

    FILE *in = fopen(src, "rb");
    if (!in) {
        cli_printf(c, "%s: %s\r\n", argv[1], strerror(errno));
        return 1;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        cli_printf(c, "%s: %s\r\n", argv[2], strerror(errno));
        fclose(in);
        return 1;
    }

    /*
     * Chunked copy. When the FDC engine + shared SD mutex land (M3/M4), long
     * copies must release the mutex per chunk so the ~1 s FDC timeout is never
     * breached (DESIGN.md §5.2, replacing the old fdc-pump-during-copy hack).
     */
    char buf[512];
    uint32_t copied = 0;
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            cli_printf(c, "copy: write error: %s\r\n", strerror(errno));
            ok = false;
            break;
        }
        copied += (uint32_t)n;
    }
    if (ok && ferror(in)) {
        cli_printf(c, "copy: read error: %s\r\n", strerror(errno));
        ok = false;
    }
    fclose(in);
    if (fclose(out) != 0 && ok) {
        cli_printf(c, "copy: close error: %s\r\n", strerror(errno));
        ok = false;
    }
    if (ok) {
        cli_printf(c, "copied %lu bytes\r\n", (unsigned long)copied);
    }
    return ok ? 0 : 1;
}

static int cmd_save(cli_console_t *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (config_save() != ESP_OK) {
        cli_write(c, "save failed\r\n");
        return 1;
    }
    cli_write(c, "configuration saved\r\n");
    return 0;
}

static int cmd_wipe(cli_console_t *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (config_wipe() != ESP_OK) {
        cli_write(c, "wipe failed\r\n");
        return 1;
    }
    cli_write(c, "NVS erased; defaults restored\r\n");
    return 0;
}

static int cmd_hostname(cli_console_t *c, int argc, char **argv)
{
    if (argc < 2) {
        cli_printf(c, "%s\r\n", config_get()->wifi_name);
        return 0;
    }
    if (strlen(argv[1]) >= CONFIG_NAME_CAP) {
        cli_printf(c, "hostname too long (max %d)\r\n", CONFIG_NAME_CAP - 1);
        return 1;
    }
    config_set_str(CFG_STR_WIFI_NAME, argv[1]);
    cli_printf(c, "hostname set to %s\r\n", argv[1]);
    return 0;
}

static int cmd_baud(cli_console_t *c, int argc, char **argv)
{
    if (argc < 2) {
        cli_printf(c, "baud %lu\r\n", (unsigned long)config_get()->baud_rate);
        return 0;
    }

    char *end;
    unsigned long rate = strtoul(argv[1], &end, 10);
    bool valid = (*end == '\0');
    if (valid) {
        valid = false;
        for (size_t i = 0; i < BAUD_COUNT; ++i) {
            if (k_bauds[i] == rate) {
                valid = true;
                break;
            }
        }
    }
    if (!valid) {
        cli_write(c, "unsupported baud; use one of:\r\n ");
        for (size_t i = 0; i < BAUD_COUNT; ++i) {
            cli_printf(c, " %lu", (unsigned long)k_bauds[i]);
        }
        cli_write(c, "\r\n");
        return 1;
    }

    config_set_baud((uint32_t)rate);
    fdc_set_baud((uint32_t)rate); /* apply live to the running FDC+ link (§6.1) */
    cli_printf(c, "baud set to %lu (applies to FDC+ link)\r\n", rate);
    return 0;
}

static int cmd_stats(cli_console_t *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    fdc_stats_t s;
    fdc_get_stats(&s);
    cli_printf(c, "FDC+ link: %lu baud\r\n", (unsigned long)fdc_baud());
    cli_printf(c, "  STAT     %lu\r\n", (unsigned long)s.stat);
    cli_printf(c, "  READ     %lu\r\n", (unsigned long)s.read);
    cli_printf(c, "  WRIT     %lu\r\n", (unsigned long)s.writ);
    cli_printf(c, "  not-rdy  %lu\r\n", (unsigned long)s.not_ready);
    cli_printf(c, "  csum-err %lu\r\n", (unsigned long)s.csum_err);
    cli_printf(c, "  timeout  %lu\r\n", (unsigned long)s.timeouts);
    cli_printf(c, "  unknown  %lu\r\n", (unsigned long)s.unknown);
    cli_printf(c, "  last     %s\r\n", s.last_op[0] ? s.last_op : "(none)");
    return 0;
}

static int cmd_clear(cli_console_t *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    fdc_clear_stats();
    cli_write(c, "statistics cleared\r\n");
    return 0;
}

/* ---- M8: console log verbosity (`log`, DESIGN.md §13) ---------------------- */

/* Level names <-> esp_log_level_t. Default is `warn` (quiet); INFO/DEBUG are opt-in. */
static const struct {
    const char     *name;
    esp_log_level_t level;
} k_log_levels[] = {
    { "none",    ESP_LOG_NONE    },
    { "error",   ESP_LOG_ERROR   },
    { "warn",    ESP_LOG_WARN    },
    { "info",    ESP_LOG_INFO    },
    { "debug",   ESP_LOG_DEBUG   },
    { "verbose", ESP_LOG_VERBOSE },
};
#define LOG_LEVEL_COUNT (sizeof(k_log_levels) / sizeof(k_log_levels[0]))

static const char *log_level_name(uint8_t level)
{
    for (size_t i = 0; i < LOG_LEVEL_COUNT; ++i) {
        if (k_log_levels[i].level == (esp_log_level_t)level) {
            return k_log_levels[i].name;
        }
    }
    return "?";
}

static int cmd_log(cli_console_t *c, int argc, char **argv)
{
    if (argc < 2) {
        cli_printf(c, "log level: %s\r\n", log_level_name(config_get()->log_level));
        return 0;
    }

    /* A level name, or the friendly aliases off (=none) / on (=info). */
    esp_log_level_t level = ESP_LOG_NONE;
    bool matched = false;
    if (strcasecmp(argv[1], "off") == 0) {
        level = ESP_LOG_NONE;
        matched = true;
    } else if (strcasecmp(argv[1], "on") == 0) {
        level = ESP_LOG_INFO;
        matched = true;
    } else {
        for (size_t i = 0; i < LOG_LEVEL_COUNT; ++i) {
            if (strcasecmp(argv[1], k_log_levels[i].name) == 0) {
                level = k_log_levels[i].level;
                matched = true;
                break;
            }
        }
    }
    if (!matched) {
        cli_write(c, "usage: log none|error|warn|info|debug|verbose\r\n");
        return 1;
    }

    config_set_log_level((uint8_t)level);
    esp_log_level_set("*", level); /* apply live to every tag */
    cli_printf(c, "log level set to %s (save to persist)\r\n", log_level_name((uint8_t)level));
    return 0;
}

static int cmd_loopback(cli_console_t *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    cli_write(c, "loopback: jumper FDC+ TX<->RX, sending 256-byte pattern...\r\n");
    fdc_loopback_t r;
    esp_err_t err = fdc_loopback(&r);
    if (err != ESP_OK) {
        cli_printf(c, "loopback: %s\r\n", esp_err_to_name(err));
        return 1;
    }
    if (r.mismatches == 0) {
        cli_printf(c, "loopback OK: %lu/%lu bytes matched\r\n",
                   (unsigned long)r.received, (unsigned long)r.sent);
        return 0;
    }
    cli_printf(c, "loopback FAILED: %lu/%lu received, %lu mismatch(es), first at %lu\r\n",
               (unsigned long)r.received, (unsigned long)r.sent,
               (unsigned long)r.mismatches, (unsigned long)r.first_bad);
    cli_write(c, "  (check TX<->RX jumper, wiring, and baud)\r\n");
    return 1;
}

/* ---- M5: networking + time (DESIGN.md §9.1 / §8.3) ------------------------- */

static int cmd_wifi(cli_console_t *c, int argc, char **argv)
{
    if (argc > 1) {
        if (strcasecmp(argv[1], "on") == 0) {
            config_set_wifi_enabled(true);
            esp_err_t err = net_wifi_enable(true);
            if (err == ESP_ERR_INVALID_STATE) {
                cli_write(c, "set an SSID first (ssid <name>)\r\n");
                return 1;
            }
            if (err != ESP_OK) {
                cli_printf(c, "wifi on: %s\r\n", esp_err_to_name(err));
                return 1;
            }
            cli_write(c, "WiFi enabled; connecting...\r\n");
            return 0;
        }
        if (strcasecmp(argv[1], "off") == 0) {
            config_set_wifi_enabled(false);
            net_wifi_enable(false);
            cli_write(c, "WiFi disabled\r\n");
            return 0;
        }
        cli_write(c, "usage: wifi [on|off]\r\n");
        return 1;
    }

    net_status_t s;
    net_get_status(&s);
    cli_printf(c, "WiFi:  %s\r\n", s.enabled ? "enabled" : "disabled");
    cli_printf(c, "SSID:  %s\r\n", s.ssid[0] ? s.ssid : "(unset)");
    if (s.connected) {
        cli_printf(c, "State: connected  IP %s  RSSI %d dBm\r\n", s.ip, s.rssi);
    } else {
        cli_printf(c, "State: %s\r\n", s.enabled ? "connecting/disconnected" : "idle");
    }
    return 0;
}

static int cmd_ssid(cli_console_t *c, int argc, char **argv)
{
    if (argc < 2) {
        const char *ssid = config_get()->wifi_ssid;
        cli_printf(c, "%s\r\n", ssid[0] ? ssid : "(unset)");
        return 0;
    }
    if (strlen(argv[1]) >= CONFIG_SSID_CAP) {
        cli_printf(c, "SSID too long (max %d)\r\n", CONFIG_SSID_CAP - 1);
        return 1;
    }
    config_set_str(CFG_STR_WIFI_SSID, argv[1]);
    cli_printf(c, "SSID set to %s\r\n", argv[1]);
    return 0;
}

static int cmd_pass(cli_console_t *c, int argc, char **argv)
{
    if (argc < 2) {
        /* Never echo the stored password back. */
        cli_printf(c, "WiFi password is %s\r\n",
                   config_get()->wifi_pass[0] ? "set" : "unset");
        return 0;
    }
    if (strlen(argv[1]) >= CONFIG_PASS_CAP) {
        cli_printf(c, "password too long (max %d)\r\n", CONFIG_PASS_CAP - 1);
        return 1;
    }
    config_set_str(CFG_STR_WIFI_PASS, argv[1]);
    cli_write(c, "WiFi password set\r\n");
    return 0;
}

/* ---- M6: FTP credentials (DESIGN.md §8.1 / §9.4) --------------------------- */

static int cmd_ftpuser(cli_console_t *c, int argc, char **argv)
{
    if (argc < 2) {
        const char *user = config_get()->ftp_user;
        cli_printf(c, "%s\r\n", user[0] ? user : "(unset)");
        return 0;
    }
    if (strlen(argv[1]) >= CONFIG_FTP_CAP) {
        cli_printf(c, "FTP username too long (max %d)\r\n", CONFIG_FTP_CAP - 1);
        return 1;
    }
    config_set_str(CFG_STR_FTP_USER, argv[1]);
    cli_printf(c, "FTP username set to %s (save; applies on next connect)\r\n", argv[1]);
    return 0;
}

static int cmd_ftppass(cli_console_t *c, int argc, char **argv)
{
    if (argc < 2) {
        /* Never echo the stored password back. */
        cli_printf(c, "FTP password is %s\r\n",
                   config_get()->ftp_pass[0] ? "set" : "unset");
        return 0;
    }
    if (strlen(argv[1]) >= CONFIG_FTP_CAP) {
        cli_printf(c, "FTP password too long (max %d)\r\n", CONFIG_FTP_CAP - 1);
        return 1;
    }
    config_set_str(CFG_STR_FTP_PASS, argv[1]);
    cli_write(c, "FTP password set (save; applies on next connect)\r\n");
    return 0;
}

static int cmd_time(cli_console_t *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    /* Pre-2016 means the RTC was never set — NTP hasn't synced yet (needs WiFi). */
    if (tm.tm_year < (2016 - 1900)) {
        cli_write(c, "time not set (waiting for NTP; needs WiFi)\r\n");
        return 0;
    }
    char buf[64];
    strftime(buf, sizeof buf, "%a %Y-%m-%d %H:%M:%S %Z", &tm);
    cli_printf(c, "%s\r\n", buf);
    return 0;
}

/* US timezone names -> POSIX TZ strings (DESIGN.md §8.3). */
static const struct {
    const char *name;
    const char *tz;
} k_tzs[] = {
    { "UTC",      "UTC0" },
    { "Eastern",  "EST5EDT,M3.2.0,M11.1.0" },
    { "Central",  "CST6CDT,M3.2.0,M11.1.0" },
    { "Mountain", "MST7MDT,M3.2.0,M11.1.0" },
    { "Arizona",  "MST7" },
    { "Pacific",  "PST8PDT,M3.2.0,M11.1.0" },
    { "Alaska",   "AKST9AKDT,M3.2.0,M11.1.0" },
    { "Hawaii",   "HST10" },
};
#define TZ_COUNT (sizeof(k_tzs) / sizeof(k_tzs[0]))

static int cmd_tz(cli_console_t *c, int argc, char **argv)
{
    if (argc < 2) {
        cli_printf(c, "timezone: %s\r\n", config_get()->time_zone);
        return 0;
    }
    if (strcmp(argv[1], "?") == 0) {
        cli_write(c, "Timezones (name -> POSIX TZ):\r\n");
        for (size_t i = 0; i < TZ_COUNT; ++i) {
            cli_printf(c, "  %-9s %s\r\n", k_tzs[i].name, k_tzs[i].tz);
        }
        cli_write(c, "Or pass a raw POSIX TZ string.\r\n");
        return 0;
    }

    /* Accept a US name (case-insensitive) or a raw POSIX TZ string (§8.3). */
    const char *posix = NULL;
    for (size_t i = 0; i < TZ_COUNT; ++i) {
        if (strcasecmp(argv[1], k_tzs[i].name) == 0) {
            posix = k_tzs[i].tz;
            break;
        }
    }
    if (!posix) {
        posix = argv[1];
    }
    if (strlen(posix) >= CONFIG_TZ_CAP) {
        cli_printf(c, "timezone too long (max %d)\r\n", CONFIG_TZ_CAP - 1);
        return 1;
    }
    config_set_str(CFG_STR_TIME_ZONE, posix);
    net_apply_timezone(); /* apply live so `time` reflects it immediately */
    cli_printf(c, "timezone set to %s\r\n", posix);
    return 0;
}

static int cmd_logout(cli_console_t *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    cli_write(c, "Goodbye\r\n");
    c->disconnect = true; /* honored by the TCP console; the serial console ignores it */
    return 0;
}

/* ---- M7: firmware update / OTA (DESIGN.md §11 / §11.1) --------------------- */

static int cmd_update(cli_console_t *c, int argc, char **argv)
{
    if (argc < 2) {
        /* Bare `update` reports versions (running / SD / OTA) and installs nothing. */
        return ota_status(c) == ESP_OK ? 0 : 1;
    }
    if (strcasecmp(argv[1], "local") == 0) {
        /* Flash /sd/firmware.bin (the primary, offline path, §11). */
        return ota_update_sd(c) == ESP_OK ? 0 : 1;
    }
    if (strcasecmp(argv[1], "ota") == 0) {
        /* Network OTA from the configured repo, gated on version.txt (§11.1). */
        return ota_update_repo(c) == ESP_OK ? 0 : 1;
    }
    /* Undocumented: `update <url>` flashes an explicit https:// image now (tnfs:// once
     * the M9 client lands, §11.1). Deliberately absent from `help`. */
    return ota_update_url(c, argv[1]) == ESP_OK ? 0 : 1;
}

/* ---- M8: batch files (exec/run, .bat auto-run, /autoexec.bat) — §8.2 -------- */

/* Cap batch nesting so a batch file that exec's itself can't recurse forever. */
#define BATCH_MAX_DEPTH 4

static bool file_is_regular(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/*
 * Resolve a batch name to an absolute SD path: try `name` as given, then `name.bat`
 * (unless it already ends in .bat). Fills buf and returns true when a regular file
 * exists; returns false (no SD, bad name, or not found) otherwise.
 */
static bool batch_path(const char *name, char *buf, size_t len)
{
    if (!sd_mounted()) {
        return false;
    }
    if (sd_path(name, buf, len) >= 0 && file_is_regular(buf)) {
        return true;
    }
    size_t nl = strlen(name);
    bool has_bat = (nl >= 4) && strcasecmp(name + nl - 4, ".bat") == 0;
    if (!has_bat) {
        char withext[CONFIG_FILE_CAP + 8];
        snprintf(withext, sizeof withext, "%s.bat", name);
        if (sd_path(withext, buf, len) >= 0 && file_is_regular(buf)) {
            return true;
        }
    }
    return false;
}

/*
 * Feed a resolved batch file to the parser one line at a time. Blank lines are
 * skipped; '#' lines echo as comments (§8.2); every command line is echoed before it
 * runs so the transcript (and the boot autoexec) shows what happened. cli_dispatch
 * tokenizes in place, so each line is copied out of the file into a scratch buffer.
 */
static void run_batch_file(cli_console_t *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        cli_printf(c, "exec: %s: %s\r\n", path, strerror(errno));
        return;
    }
    char buf[CLI_LINE_MAX + 2];
    while (fgets(buf, sizeof buf, f)) {
        size_t n = strlen(buf);
        bool complete = (n > 0 && buf[n - 1] == '\n');
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
            buf[--n] = '\0';
        }
        if (!complete) {
            /* Line longer than the editor buffer: drop the remainder and warn. */
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n') {
                /* discard */
            }
            cli_write(c, "exec: line too long, skipped\r\n");
            continue;
        }
        char *p = buf;
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (*p == '\0') {
            continue; /* blank line */
        }
        cli_printf(c, "%s\r\n", p); /* echo the line (comment or command) */
        if (*p == '#') {
            continue; /* comment: echoed above, nothing to run */
        }
        cli_dispatch(c, p);
    }
    fclose(f);
}

/* Run a resolved path under the nesting guard. */
static void batch_exec_path(cli_console_t *c, const char *path)
{
    if (c->exec_depth >= BATCH_MAX_DEPTH) {
        cli_write(c, "exec: batch files nested too deep\r\n");
        return;
    }
    c->exec_depth++;
    run_batch_file(c, path);
    c->exec_depth--;
}

int cli_exec_batch(cli_console_t *c, const char *name)
{
    char path[16 + CONFIG_FILE_CAP];
    if (!batch_path(name, path, sizeof path)) {
        cli_printf(c, "%s: not found\r\n", name);
        return 1;
    }
    batch_exec_path(c, path);
    return 0;
}

bool cli_try_autorun(cli_console_t *c, const char *name)
{
    char path[16 + CONFIG_FILE_CAP];
    if (!batch_path(name, path, sizeof path)) {
        return false; /* not a batch file — let the caller report "unknown command" */
    }
    batch_exec_path(c, path);
    return true;
}

void cli_run_autoexec(cli_console_t *c)
{
    char path[16 + CONFIG_FILE_CAP];
    if (!sd_mounted() || sd_path("autoexec.bat", path, sizeof path) < 0 ||
        !file_is_regular(path)) {
        return; /* silent when absent (§8.2 "if present") */
    }
    cli_write(c, "running /autoexec.bat\r\n");
    batch_exec_path(c, path);
}

static int cmd_exec(cli_console_t *c, int argc, char **argv)
{
    if (argc < 2) {
        cli_write(c, "usage: exec <file>\r\n");
        return 1;
    }
    return cli_exec_batch(c, argv[1]) == 0 ? 0 : 1;
}
