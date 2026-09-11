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
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_timer.h"

#include "cli.h"
#include "config.h"
#include "disk.h"
#include "fdc.h"
#include "http.h"
#include "net.h"
#include "ota.h"
#include "sd.h"
#include "tnfs.h"
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
static int cmd_mkdir(cli_console_t *c, int argc, char **argv);
static int cmd_rmdir(cli_console_t *c, int argc, char **argv);
static int cmd_rename(cli_console_t *c, int argc, char **argv);
static int cmd_copy(cli_console_t *c, int argc, char **argv);
static int cmd_save(cli_console_t *c, int argc, char **argv);
static int cmd_wipe(cli_console_t *c, int argc, char **argv);
static int cmd_hostname(cli_console_t *c, int argc, char **argv);
static int cmd_baud(cli_console_t *c, int argc, char **argv);
static int cmd_stats(cli_console_t *c, int argc, char **argv);
static int cmd_diag(cli_console_t *c, int argc, char **argv);
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

/*
 * Full per-command help, shown by `help <command>` under the one-line summary. Each
 * string picks up where the summary leaves off — extra behavior, usage, and (where
 * relevant) valid arguments — so it never just restates the summary the reader has
 * already seen. For commands whose choices come from a table (baud/log/tz), cmd_help
 * appends the live list, so help can't drift from what the command actually accepts.
 * Commands whose one-line summary already says everything leave detail NULL.
 */
static const char D_help[] =
    "usage: help [<command>]\r\n"
    "  '?' is a synonym for help  (e.g. '? mount')\r\n";
static const char D_baud[] =
    "Sets the speed of the serial link to the FDC+ board - not this USB console,\r\n"
    "which is unaffected. The FDC+ S-100 board must be set to the same rate or the\r\n"
    "two cannot talk. A new rate applies immediately; run 'save' to keep it across\r\n"
    "reboots.\r\n"
    "usage: baud [<rate>]        (with no rate, shows the current speed)\r\n";
static const char D_dir[] =
    "With a pattern, shows only matching files; with a subdirectory name, lists\r\n"
    "that folder; with a tnfs:// URL, lists a directory on a TNFS server.\r\n"
    "Subdirectories are shown with a trailing '/', listed first, then files,\r\n"
    "each in case-insensitive alphabetical order.\r\n"
    "usage: dir [<pattern> | <subdir> | tnfs://<host>/<path>]\r\n"
    "examples:\r\n"
    "  dir                 list every file\r\n"
    "  dir *.DSK           list only .DSK files\r\n"
    "  dir cpm             list the cpm/ subdirectory\r\n"
    "  dir cpm/*.DSK       list .DSK files under cpm/\r\n"
    "  dir tnfs://192.168.1.10/disks\r\n";
static const char D_mount[] =
    "With no arguments, shows what is mounted on each drive.\r\n"
    "usage: mount [<drive> <file | tnfs://host/path>]\r\n"
    "  <drive> is 0-3\r\n"
    "  <file> is an image on the SD card; a tnfs:// URL mounts it over the network\r\n"
    "examples:\r\n"
    "  mount               show the mount table\r\n"
    "  mount 0 CPM22.DSK\r\n"
    "  mount 1 tnfs://192.168.1.10/disks/GAMES.DSK\r\n";
static const char D_unmount[] =
    "usage: unmount <drive>      (<drive> is 0-3)\r\n";
static const char D_log[] =
    "A new level applies immediately; run 'save' to keep it across reboots.\r\n"
    "usage: log [<level>]        (with no level, shows the current setting)\r\n";
static const char D_save[] =
    "Covers mounts, WiFi, baud, timezone, and the rest, so the device restores\r\n"
    "them automatically after a reboot or power loss.\r\n"
    "usage: save\r\n";
static const char D_wipe[] =
    "Takes effect at once. Does not touch the SD card or its disk images.\r\n"
    "usage: wipe\r\n";
static const char D_stats[] =
    "Reports the FDC+ link speed and running counts since boot (or the last\r\n"
    "'clear'):\r\n"
    "  STAT / READ / WRIT   commands the FDC+ has issued, by type\r\n"
    "  not-rdy              requests to a drive with no image mounted\r\n"
    "  csum-err             commands rejected for a bad checksum\r\n"
    "  timeout              commands that did not complete in time\r\n"
    "  unknown              unrecognized commands\r\n"
    "  last                 the most recent command handled\r\n"
    "usage: stats\r\n";
static const char D_diag[] =
    "Prints one self-contained report - firmware/hardware, saved (NVS) settings,\r\n"
    "WiFi, SD card, drives, and FDC+ link stats - meant to be copied out of the\r\n"
    "console and pasted into a support email. Passwords are never shown, only\r\n"
    "whether one is set. Read-only: it changes nothing.\r\n"
    "usage: diag\r\n";
static const char D_dump[] =
    "16 bytes per line, like xxd, for troubleshooting. With no arguments, shows\r\n"
    "the last track the FDC+ transferred.\r\n"
    "usage: dump [<drive> [<track> [<length>]]]\r\n"
    "  <drive> is 0-3; <track> defaults to 0; <length> defaults to 256 bytes\r\n";
static const char D_wifi[] =
    "Set the network first with 'ssid' and 'pass'.\r\n"
    "usage: wifi [on | off]      (with no argument, shows status: SSID, IP, signal)\r\n";
static const char D_ssid[] =
    "usage: ssid [<network-name>]   (with no name, shows the current SSID)\r\n";
static const char D_pass[] =
    "For safety the stored password is never shown - only whether one is set.\r\n"
    "usage: pass <password>\r\n";
static const char D_hostname[] =
    "The name is used on the network (<name>.local) and shown in the prompt.\r\n"
    "usage: hostname [<name>]    (with no name, shows the current name)\r\n";
static const char D_ftpuser[] =
    "Takes effect on the next FTP connection.\r\n"
    "usage: ftpuser [<name>]     (with no name, shows the current username)\r\n";
static const char D_ftppass[] =
    "The stored password is never shown. Takes effect on the next FTP connection.\r\n"
    "usage: ftppass <password>\r\n";
static const char D_update[] =
    "usage: update [local | ota]\r\n"
    "  update            show the running, SD-card, and available versions\r\n"
    "  update local      install /firmware.bin from the SD card\r\n"
    "  update ota        download and install the latest release over WiFi\r\n";
static const char D_type[] =
    "Reads the file from the SD card.\r\n"
    "usage: type <file>\r\n";
static const char D_exec[] =
    "One command per line; lines starting with '#' are comments. The .bat\r\n"
    "extension is optional. The batch file may be in a subdirectory (exec\r\n"
    "basic/setup.bat); while it runs, bare names inside it resolve relative to\r\n"
    "the batch's own folder, so a disk-set folder is self-contained (setup.bat\r\n"
    "in basic/ writes 'mount 0 setup.dsk'). No '..' - a batch stays in its\r\n"
    "folder. The prompt itself always runs at the SD root.\r\n"
    "usage: exec <file>\r\n";
static const char D_delete[] =
    "This cannot be undone.\r\n"
    "usage: delete <file>\r\n";
static const char D_mkdir[] =
    "Creates a directory on the SD card. Any parent directories must already\r\n"
    "exist. Names may include '/' to create a folder inside an existing one.\r\n"
    "usage: mkdir <dir>\r\n";
static const char D_rmdir[] =
    "Removes a directory from the SD card. The directory must be empty.\r\n"
    "usage: rmdir <dir>\r\n";
static const char D_rename[] =
    "usage: rename <old-name> <new-name>\r\n";
static const char D_copy[] =
    "Each of <src> and <dst> may be an SD-card file or a tnfs:// URL, so this\r\n"
    "also transfers files to or from a TNFS server. <src> may also be an\r\n"
    "http:// or https:// URL to pull a file from a web server (source only).\r\n"
    "usage: copy <src> <dst>\r\n"
    "examples:\r\n"
    "  copy CPM22.DSK BACKUP.DSK\r\n"
    "  copy tnfs://192.168.1.10/disks/GAMES.DSK GAMES.DSK\r\n"
    "  copy https://example.com/disks/CPM22.DSK CPM22.DSK\r\n";
static const char D_loopback[] =
    "Sends a 256-byte pattern out the FDC+ serial port and checks it comes back.\r\n"
    "Jumper the FDC+ TX and RX pins together first.\r\n"
    "usage: loopback\r\n";
static const char D_tz[] =
    "Used by 'time'. Accepts a US zone name (listed below) or a raw POSIX TZ\r\n"
    "string. A change applies immediately.\r\n"
    "usage: tz [<zone>]          (with no zone, shows the current setting)\r\n";

/* Grouped by milestone/topic; `help` sorts this into alphabetical order for display. */
static const cli_command_t k_commands[] = {
    { "help",     "?",      "List commands, or show help for one", cmd_help,   D_help     },
    { "version",  NULL,     "Show the firmware version",           cmd_version, NULL      },
    { "baud",     NULL,     "Show or set the FDC+ link speed",     cmd_baud,   D_baud     },
    { "dir",      "ls",     "List files on SD or a TNFS server",   cmd_dir,    D_dir      },
    { "mount",    NULL,     "Mount a disk image on a drive",       cmd_mount,  D_mount    },
    { "unmount",  "umount", "Unmount a drive",                     cmd_unmount, D_unmount },
    { "stats",    NULL,     "Show FDC+ statistics",                cmd_stats,  D_stats    },
    { "diag",     NULL,     "Print a full status report for support", cmd_diag, D_diag    },
    { "clear",    NULL,     "Reset FDC+ statistics to zero",       cmd_clear,  NULL       },
    { "log",      NULL,     "Show or set console logging detail",  cmd_log,    D_log      },
    { "save",     "write",  "Save settings so they survive reboot", cmd_save,  D_save     },
    { "wipe",     NULL,     "Erase saved settings; restore defaults", cmd_wipe, D_wipe    },
    { "dump",     NULL,     "Hex-dump a disk track",               cmd_dump,   D_dump     },
    { "wifi",     NULL,     "Show WiFi status, or turn it on/off", cmd_wifi,   D_wifi     },
    { "ssid",     NULL,     "Show or set the WiFi network name",   cmd_ssid,   D_ssid     },
    { "pass",     NULL,     "Set the WiFi password",               cmd_pass,   D_pass     },
    { "hostname", NULL,     "Show or set the device name",         cmd_hostname, D_hostname },
    { "ftpuser",  NULL,     "Show or set the FTP username",        cmd_ftpuser, D_ftpuser },
    { "ftppass",  NULL,     "Set the FTP password",                cmd_ftppass, D_ftppass },
    { "update",   NULL,     "Show versions, or install an update", cmd_update, D_update   },
    { "type",     "cat",    "Print a text file",                   cmd_type,   D_type     },
    { "exec",     "run",    "Run a batch file of commands",        cmd_exec,   D_exec     },
    { "logout",   "exit",   "Disconnect this network session",     cmd_logout, NULL       },
    { "delete",   "rm",     "Delete a file from SD",               cmd_delete, D_delete   },
    { "mkdir",    "md",     "Create a directory on SD",            cmd_mkdir,  D_mkdir    },
    { "rmdir",    "rd",     "Remove an empty directory on SD",     cmd_rmdir,  D_rmdir    },
    { "rename",   "mv",     "Rename a file on SD",                 cmd_rename, D_rename   },
    { "copy",     "cp",     "Copy a file (SD/TNFS, or HTTP src)",  cmd_copy,   D_copy     },
    { "loopback", "lb",     "Run the FDC+ serial loopback test",   cmd_loopback, D_loopback },
    { "time",     "date",   "Show the current date and time",      cmd_time,   NULL       },
    { "tz",       NULL,     "Show or set the timezone",            cmd_tz,     D_tz       },
    { "reboot",   NULL,     "Restart the device",                  cmd_reboot, NULL       },
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
/* ---- batch working directory (DESIGN.md §8.2/§10) --------------------------- *
 * While a batch file runs, bare names inside it resolve relative to the batch's own
 * folder rather than the SD root, so a disk-set folder is self-contained and can be
 * relocated by dragging it elsewhere on the card. The cwd is a root-relative directory
 * ("" = root); qualify() folds it into a user-supplied name before sd_path() resolves
 * the result, so the whole model lives here in the CLI layer and sd_path() stays a
 * pure root resolver. The interactive prompt always runs at the root (cwd ""), and
 * there is no `cd` command. Not per-console: one batch runs at a time (single-user,
 * DESIGN.md §8.2), so a single global is enough. */
static char s_batch_cwd[CONFIG_FILE_CAP]; /* "" = SD root */

/*
 * Fold the batch cwd into a user-supplied name, yielding a root-relative path for
 * sd_path(). With no active cwd the name passes through unchanged. Returns false only
 * if the result would overflow buf; sd_path() still enforces the ".."/leading-'/'/'\\'
 * confinement rules on the qualified name.
 */
static bool qualify(const char *name, char *buf, size_t len)
{
    int n = s_batch_cwd[0] ? snprintf(buf, len, "%s/%s", s_batch_cwd, name)
                           : snprintf(buf, len, "%s", name);
    return n >= 0 && (size_t)n < len;
}

static bool resolve(cli_console_t *c, const char *name, char *buf, size_t len)
{
    if (!sd_mounted()) {
        cli_write(c, "no SD card\r\n");
        return false;
    }
    char qname[2 * CONFIG_FILE_CAP];
    if (!qualify(name, qname, sizeof qname) || sd_path(qname, buf, len) < 0) {
        cli_printf(c, "%s: invalid filename\r\n", name);
        return false;
    }
    return true;
}

/* ---- handlers -------------------------------------------------------------- */

/* cmd_help is defined at the end of the file so it can list the live baud/log/tz
 * value tables (which are declared lower down). */

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

/* Context for the tnfs_list_dir callback (remote `dir`). */
struct dir_remote_ctx {
    cli_console_t *c;
    const char   *spec;      /* optional glob filter, or NULL */
    bool          spec_is_dot;
    unsigned      files;
};

/* One remote directory entry: apply the same dotfile-hide + glob rules as SD dir, and
 * mark subdirectories with a trailing '/' (ls -F style). */
static void dir_remote_entry(const char *name, bool is_dir, void *vctx)
{
    struct dir_remote_ctx *ctx = vctx;
    /* Hide dotfiles incl. "." / ".." (§8.1) unless the spec starts with '.' (§8.4). */
    if (name[0] == '.' && !ctx->spec_is_dot) {
        return;
    }
    if (ctx->spec && !wildcard_match_ci(ctx->spec, name)) {
        return;
    }
    cli_printf(ctx->c, "  %s%s\r\n", name, is_dir ? "/" : "");
    ++ctx->files;
}

/* `dir tnfs://host[:port]/path/ [spec]` — list a TNFS server directory (§8.4/§10.1). */
static int cmd_dir_remote(cli_console_t *c, const char *url, const char *spec)
{
    if (!net_is_connected()) {
        cli_write(c, "dir: WiFi not connected\r\n");
        return 1;
    }

    /* Allow a wildcard glued to the URL path, e.g. `dir tnfs://host/pub/<glob>` (one
     * token, just like local `dir *.DSK`): split the trailing segment off as the glob
     * pattern and list its parent directory. An explicit spec arg takes precedence. */
    char urlbuf[16 + CONFIG_FILE_CAP];
    if (!spec) {
        const char *last = strrchr(url, '/');
        if (last && strpbrk(last + 1, "*?")) {
            spec = last + 1;                      /* the wildcard leaf */
            size_t dirlen = (size_t)(last - url); /* parent, without the trailing '/' */
            if (dirlen > 0 && dirlen < sizeof urlbuf) {
                memcpy(urlbuf, url, dirlen);
                urlbuf[dirlen] = '\0';
                url = urlbuf;                     /* a bare host re-gains '/' in tnfs_list_dir */
            }
        }
    }

    struct dir_remote_ctx ctx = {
        .c = c, .spec = spec, .spec_is_dot = spec && spec[0] == '.', .files = 0,
    };
    esp_err_t err = tnfs_list_dir(url, dir_remote_entry, &ctx);
    if (err != ESP_OK) {
        cli_printf(c, "%s: %s\r\n", url,
                   err == ESP_ERR_NOT_FOUND   ? "not found" :
                   err == ESP_ERR_TIMEOUT     ? "server unreachable" :
                   err == ESP_ERR_INVALID_ARG ? "bad URL" : "listing failed");
        return 1;
    }
    cli_printf(c, "%u item(s)\r\n", ctx.files); /* may include directories */
    return 0;
}

/* Upper bound on entries `dir` buffers for sorting, so a huge directory can't exhaust
 * the heap; past this the listing is truncated with a note. */
#define DIR_MAX_ENTS 1024

/* One collected directory entry (name is a flexible trailing array). */
struct dir_ent {
    uint32_t size;
    bool     is_dir;
    char     name[];
};

/* Sort order for `dir`: directories first, then case-insensitive alphabetical, with a
 * case-sensitive tiebreak so names differing only in case have a stable order. */
static int dir_ent_cmp(const void *pa, const void *pb)
{
    const struct dir_ent *a = *(const struct dir_ent *const *)pa;
    const struct dir_ent *b = *(const struct dir_ent *const *)pb;
    if (a->is_dir != b->is_dir) {
        return a->is_dir ? -1 : 1;
    }
    int r = strcasecmp(a->name, b->name);
    return r ? r : strcmp(a->name, b->name);
}

static int cmd_dir(cli_console_t *c, int argc, char **argv)
{
    /* A tnfs:// argument lists a remote server directory (§10.1); an optional second
     * arg is the same glob spec as the local case. */
    if (argc > 1 && tnfs_is_url(argv[1])) {
        return cmd_dir_remote(c, argv[1], (argc > 2) ? argv[2] : NULL);
    }

    if (!sd_mounted()) {
        cli_write(c, "no SD card\r\n");
        return 1;
    }

    /*
     * Split an optional argument into a subdirectory to list and a glob leaf, so a
     * bare `dir`, a root glob, a subdir name, and a glob inside a subdir all work
     * (DESIGN.md §8.4/§10):
     *   - a '/' splits the arg into <subdir>/<leaf>; the leaf, if any, is the glob;
     *   - a bare arg naming an existing directory lists that directory (no glob);
     *   - otherwise the bare arg is a glob applied to the root.
     */
    char reldir[CONFIG_FILE_CAP] = ""; /* subdirectory under the root; "" = root */
    const char *spec = NULL;           /* glob leaf, or NULL to list everything */
    const char *arg = (argc > 1) ? argv[1] : NULL;
    if (arg) {
        char probe[16 + CONFIG_FILE_CAP];
        char aq[2 * CONFIG_FILE_CAP];
        struct stat st;
        if (strlen(arg) < sizeof reldir && qualify(arg, aq, sizeof aq) &&
            sd_path(aq, probe, sizeof probe) >= 0 &&
            stat(probe, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(reldir, sizeof reldir, "%s", arg); /* the whole arg names a directory */
        } else {
            const char *slash = strrchr(arg, '/');
            if (slash) {                                /* <subdir>/<leaf glob> */
                size_t dlen = (size_t)(slash - arg);
                if (dlen >= sizeof reldir) {
                    cli_printf(c, "%s: invalid filename\r\n", arg);
                    return 1;
                }
                memcpy(reldir, arg, dlen);
                reldir[dlen] = '\0';
                spec = (slash[1] != '\0') ? slash + 1 : NULL;
            } else {
                spec = arg;                             /* a glob applied to the root */
            }
        }
    }
    const bool spec_is_dot = spec && spec[0] == '.';

    /* Absolute path of the directory to list: the cwd (root, or a running batch's own
     * folder), or a subdir named under it. */
    char dirpath[16 + CONFIG_FILE_CAP];
    if (reldir[0]) {
        char rq[2 * CONFIG_FILE_CAP];
        if (!qualify(reldir, rq, sizeof rq) || sd_path(rq, dirpath, sizeof dirpath) < 0) {
            cli_printf(c, "%s: invalid filename\r\n", reldir);
            return 1;
        }
    } else if (s_batch_cwd[0]) {
        snprintf(dirpath, sizeof dirpath, "%s/%s", SD_MOUNT_POINT, s_batch_cwd);
    } else {
        snprintf(dirpath, sizeof dirpath, "%s", SD_MOUNT_POINT);
    }

    DIR *d = opendir(dirpath);
    if (!d) {
        cli_printf(c, "%s: %s\r\n", arg ? arg : SD_MOUNT_POINT, strerror(errno));
        return 1;
    }

    /* Name the directory being listed, root-relative (root shows as '/'); a running
     * batch's cwd is folded in so the header always reads from the SD root. */
    char disp[2 * CONFIG_FILE_CAP];
    if (s_batch_cwd[0] && reldir[0]) {
        snprintf(disp, sizeof disp, "%s/%s", s_batch_cwd, reldir);
    } else if (s_batch_cwd[0]) {
        snprintf(disp, sizeof disp, "%s", s_batch_cwd);
    } else {
        snprintf(disp, sizeof disp, "%s", reldir);
    }
    if (disp[0]) {
        cli_printf(c, "Directory of /%s\r\n", disp);
    } else {
        cli_write(c, "Directory of /\r\n");
    }

    /* Collect matching entries, then sort (dirs first, A-Z ci) before printing. */
    struct dir_ent **ents = NULL;
    size_t count = 0, cap = 0;
    bool truncated = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        /* Hide dotfiles (DESIGN.md §8.1) unless the spec itself starts with '.' (§8.4). */
        if (e->d_name[0] == '.' && !spec_is_dot) {
            continue;
        }
        if (spec && !wildcard_match_ci(spec, e->d_name)) {
            continue;
        }
        /* Stat the entry through the resolved directory path we opened. */
        char path[sizeof dirpath + CONFIG_FILE_CAP];
        if ((size_t)snprintf(path, sizeof path, "%s/%s", dirpath, e->d_name) >= sizeof path) {
            continue;
        }
        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        if (count >= DIR_MAX_ENTS) { /* cap the listing so the heap can't be exhausted */
            truncated = true;
            break;
        }
        if (count == cap) {
            size_t ncap = cap ? cap * 2 : 32;
            struct dir_ent **na = realloc(ents, ncap * sizeof *na);
            if (!na) {
                truncated = true;
                break;
            }
            ents = na;
            cap = ncap;
        }
        struct dir_ent *ent = malloc(sizeof *ent + strlen(e->d_name) + 1);
        if (!ent) {
            truncated = true;
            break;
        }
        ent->size = (uint32_t)st.st_size;
        ent->is_dir = S_ISDIR(st.st_mode);
        strcpy(ent->name, e->d_name);
        ents[count++] = ent;
    }
    closedir(d);

    if (count > 1) {
        qsort(ents, count, sizeof *ents, dir_ent_cmp);
    }

    unsigned files = 0, dirs = 0;
    uint32_t total = 0;
    for (size_t i = 0; i < count; i++) {
        struct dir_ent *ent = ents[i];
        if (ent->is_dir) {
            cli_printf(c, "%10s  %s/\r\n", "<DIR>", ent->name);
            ++dirs;
        } else {
            cli_printf(c, "%10lu  %s\r\n", (unsigned long)ent->size, ent->name);
            total += ent->size;
            ++files;
        }
        free(ent);
    }
    free(ents);

    if (truncated) {
        cli_printf(c, "(list truncated at %d entries)\r\n", DIR_MAX_ENTS);
    }
    if (dirs) {
        cli_printf(c, "%u file(s), %u dir(s), %lu bytes\r\n",
                   files, dirs, (unsigned long)total);
    } else {
        cli_printf(c, "%u file(s), %lu bytes\r\n", files, (unsigned long)total);
    }
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
                cli_printf(c, "%d: %s (%lu bytes)%s\r\n", i, disk_name(i),
                           (unsigned long)disk_image_size(i),
                           disk_is_readonly(i) ? " [read-only]" : "");
            } else if (disk_is_remote(i)) {
                cli_printf(c, "%d: %s (not ready)\r\n", i, disk_name(i));
            } else {
                cli_printf(c, "%d: (empty)\r\n", i);
            }
        }
        return 0;
    }
    if (argc < 3) {
        cli_write(c, "usage: mount <drive> <file|tnfs://url>\r\n");
        return 1;
    }

    int drive = parse_drive(c, argv[1]);
    if (drive < 0) {
        return 1;
    }

    /* A tnfs:// target is a remote mount (§10.1): disk_mount configures the slot and, if
     * WiFi is up, opens the session on the disk-I/O worker; if not, it stays deferred. */
    if (tnfs_is_url(argv[2])) {
        disk_mount(drive, argv[2]); /* configures the slot regardless of link state */
        config_set_drive(drive, argv[2]); /* persist so it re-mounts on reconnect/reboot */
        if (disk_is_mounted(drive)) {
            cli_printf(c, "drive %d: %s (%lu bytes)%s\r\n", drive, argv[2],
                       (unsigned long)disk_image_size(drive),
                       disk_is_readonly(drive) ? " [read-only]" : "");
        } else if (!net_is_connected()) {
            cli_printf(c, "drive %d: %s (will mount when WiFi connects)\r\n", drive, argv[2]);
        } else {
            cli_printf(c, "drive %d: %s (not ready — server unreachable, will retry)\r\n",
                       drive, argv[2]);
        }
        return 0;
    }

    /* Fold a running batch's cwd into the name so a batch mounts an image from its own
     * folder (DESIGN.md §8.2). The qualified, root-relative name is what we open, store,
     * and persist, so it re-mounts correctly from the prompt and at boot (cwd = root). */
    char qname[2 * CONFIG_FILE_CAP];
    if (!qualify(argv[2], qname, sizeof qname)) {
        cli_printf(c, "%s: invalid filename\r\n", argv[2]);
        return 1;
    }
    esp_err_t err = disk_mount(drive, qname);
    if (err != ESP_OK) {
        /* disk_mount drops any previous disk on failure; keep the persisted config in
         * step and say the drive is now empty, so a failed mount can't be mistaken for
         * the old disk still being in place (even if the error line scrolls past). */
        config_set_drive(drive, NULL);
        report_mount_err(c, qname, err);
        cli_printf(c, "drive %d: (empty)\r\n", drive);
        return 1;
    }
    /* Persist the mount so `save` + reboot auto-mounts it (DESIGN.md §7/§12). */
    config_set_drive(drive, qname);
    cli_printf(c, "drive %d: %s (%lu bytes)\r\n", drive, qname,
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
        return 0; /* already unmounted: stay quiet (§8.2 batch-friendly) */
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

static int cmd_mkdir(cli_console_t *c, int argc, char **argv)
{
    if (argc < 2) {
        cli_write(c, "usage: mkdir <dir>\r\n");
        return 1;
    }
    char path[16 + CONFIG_FILE_CAP];
    if (!resolve(c, argv[1], path, sizeof path)) {
        return 1;
    }
    if (mkdir(path, 0777) != 0) {
        cli_printf(c, "%s: %s\r\n", argv[1], strerror(errno));
        return 1;
    }
    cli_printf(c, "created %s\r\n", argv[1]);
    return 0;
}

static int cmd_rmdir(cli_console_t *c, int argc, char **argv)
{
    if (argc < 2) {
        cli_write(c, "usage: rmdir <dir>\r\n");
        return 1;
    }
    char path[16 + CONFIG_FILE_CAP];
    if (!resolve(c, argv[1], path, sizeof path)) {
        return 1;
    }
    if (rmdir(path) != 0) {
        /* FATFS refuses a non-empty directory with EACCES; say so plainly. */
        const char *why = (errno == EACCES || errno == ENOTEMPTY)
                              ? "directory not empty" : strerror(errno);
        cli_printf(c, "%s: %s\r\n", argv[1], why);
        return 1;
    }
    cli_printf(c, "removed %s\r\n", argv[1]);
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

/*
 * Open the copy destination named by `name` — a tnfs:// URL (create+truncate on the
 * server) when `remote`, otherwise an SD-root file opened "wb". Fills exactly one of
 * *sf / *rf on success (the other is NULL) and returns true; prints a diagnostic and
 * returns false on failure. Shared by every source path in cmd_copy (§10.2/§10.3).
 */
static bool copy_open_dest(cli_console_t *c, const char *name, bool remote,
                           FILE **sf, tnfs_file_t **rf)
{
    *sf = NULL;
    *rf = NULL;
    if (remote) {
        esp_err_t err = tnfs_open(name, true, true, rf);
        if (err != ESP_OK) {
            cli_printf(c, "%s: %s\r\n", name,
                       err == ESP_ERR_TIMEOUT ? "server unreachable" : "create failed");
            return false;
        }
        return true;
    }
    char path[16 + CONFIG_FILE_CAP];
    if (!resolve(c, name, path, sizeof path)) {
        return false;
    }
    *sf = fopen(path, "wb");
    if (!*sf) {
        cli_printf(c, "%s: %s\r\n", name, strerror(errno));
        return false;
    }
    return true;
}

/* Show a running "copied N bytes" line, refreshed in place with a bare '\r' (no line
 * feed), at most once per this many bytes so a large transfer doesn't flood the console.
 * The byte count only grows and the final line reuses the same format, so each refresh is
 * at least as wide as the last — no stale tail is left behind, and the final line (ending
 * in "\r\n") simply overwrites the last progress line before the prompt returns. */
#define COPY_PROGRESS_STEP (20 * 1024)

/* Destination state for an http(s):// source pull (§10.3): the open destination (SD or
 * TNFS), the running write offset, and the message for the first write failure. `c` is the
 * console for progress (NULL disables it) and `reported` is the offset last printed. */
typedef struct {
    FILE          *sf;
    tnfs_file_t   *rf;
    uint32_t       off;
    const char    *err;
    cli_console_t *c;
    uint32_t       reported;
} copy_sink_t;

/* http_sink_fn: write one streamed body chunk to the copy destination. Returns 0 to
 * continue or -1 to abort (recording which side failed). */
static int copy_sink_write(void *ctx, const void *data, size_t len)
{
    copy_sink_t *s = (copy_sink_t *)ctx;
    if (s->rf) {
        if (tnfs_write_at(s->rf, s->off, data, len) != ESP_OK) {
            s->err = "copy: remote write error\r\n";
            return -1;
        }
    } else if (fwrite(data, 1, len, s->sf) != len) {
        s->err = "copy: write error\r\n";
        return -1;
    }
    s->off += (uint32_t)len;
    if (s->c && s->off - s->reported >= COPY_PROGRESS_STEP) {
        cli_printf(s->c, "\rcopied %lu bytes", (unsigned long)s->off);
        s->reported = s->off;
    }
    return 0;
}

/*
 * copy <src> <dst> — stream a whole file between the SD root, a TNFS server, and/or a
 * web server (DESIGN.md §10.2/§10.3). <src> or <dst> may be a tnfs://host/path URL; <src>
 * may additionally be an http(s):// URL (source only — HTTP has no upload verb). An
 * endpoint without a scheme is an SD-root filename. Streaming is chunked so neither the
 * SD nor the FDC path is starved (§5.2): each iteration is a bounded read + write,
 * holding no lock across the transfer. A tnfs:// source is bounded by its STAT size; a
 * local or http(s):// source runs to EOF.
 */
static int cmd_copy(cli_console_t *c, int argc, char **argv)
{
    if (argc < 3) {
        cli_write(c, "usage: copy <src> <dst>\r\n");
        return 1;
    }
    bool src_http   = http_is_url(argv[1]);
    bool src_remote = tnfs_is_url(argv[1]);
    bool dst_remote = tnfs_is_url(argv[2]);
    if (http_is_url(argv[2])) {
        cli_write(c, "copy: http(s):// is a source only, not a destination\r\n");
        return 1;
    }
    if ((src_http || src_remote || dst_remote) && !net_is_connected()) {
        cli_write(c, "copy: WiFi not connected\r\n");
        return 1;
    }

    /* http(s):// source (§10.3): a forward-only GET streamed straight into the
     * destination, so the destination is opened first and the transfer runs to EOF. */
    if (src_http) {
        FILE *sout = NULL;
        tnfs_file_t *rout = NULL;
        if (!copy_open_dest(c, argv[2], dst_remote, &sout, &rout)) {
            return 1;
        }
        copy_sink_t sink = { .sf = sout, .rf = rout, .off = 0, .err = NULL,
                             .c = c, .reported = 0 };
        uint32_t got = 0;
        esp_err_t err = http_get(argv[1], copy_sink_write, &sink, &got);

        bool ok = (err == ESP_OK);
        const char *msg = NULL;
        if (!ok) {
            msg = sink.err ? sink.err
                : err == ESP_ERR_INVALID_RESPONSE ? "copy: HTTP request failed\r\n"
                : err == ESP_ERR_TIMEOUT          ? "copy: server unreachable\r\n"
                                                  : "copy: fetch failed\r\n";
        }
        if (rout) {
            tnfs_close(rout);
        } else if (fclose(sout) != 0 && ok) {
            ok = false;
            msg = "copy: close error\r\n";
        }
        if (!ok) {
            cli_write(c, msg);
            return 1;
        }
        cli_printf(c, "\rcopied %lu bytes\r\n", (unsigned long)got);
        return 0;
    }

    /* Open source (SD file or tnfs:// URL). */
    FILE *sin = NULL;
    tnfs_file_t *rin = NULL;
    uint32_t total = 0;
    if (src_remote) {
        esp_err_t err = tnfs_open(argv[1], false, false, &rin);
        if (err != ESP_OK) {
            cli_printf(c, "%s: %s\r\n", argv[1],
                       err == ESP_ERR_NOT_FOUND ? "not found" :
                       err == ESP_ERR_TIMEOUT   ? "server unreachable" : "open failed");
            return 1;
        }
        total = tnfs_size(rin);
    } else {
        char path[16 + CONFIG_FILE_CAP];
        if (!resolve(c, argv[1], path, sizeof path)) {
            return 1;
        }
        sin = fopen(path, "rb");
        if (!sin) {
            cli_printf(c, "%s: %s\r\n", argv[1], strerror(errno));
            return 1;
        }
    }

    /* Open destination (create+truncate for a remote push). */
    FILE *sout = NULL;
    tnfs_file_t *rout = NULL;
    if (!copy_open_dest(c, argv[2], dst_remote, &sout, &rout)) {
        if (rin) { tnfs_close(rin); } else { fclose(sin); }
        return 1;
    }

    uint8_t buf[512];
    uint32_t off = 0;
    uint32_t reported = 0;
    bool ok = true;
    const char *msg = NULL;
    for (;;) {
        size_t n;
        if (src_remote) {
            if (off >= total) {
                break;
            }
            uint32_t want = total - off;
            if (want > sizeof buf) {
                want = sizeof buf;
            }
            if (tnfs_read_at(rin, off, buf, want) != ESP_OK) {
                ok = false; msg = "copy: remote read error\r\n"; break;
            }
            n = want;
        } else {
            n = fread(buf, 1, sizeof buf, sin);
            if (n == 0) {
                if (ferror(sin)) { ok = false; msg = "copy: read error\r\n"; }
                break;
            }
        }
        if (dst_remote) {
            if (tnfs_write_at(rout, off, buf, n) != ESP_OK) {
                ok = false; msg = "copy: remote write error\r\n"; break;
            }
        } else if (fwrite(buf, 1, n, sout) != n) {
            ok = false; msg = "copy: write error\r\n"; break;
        }
        off += (uint32_t)n;
        if (off - reported >= COPY_PROGRESS_STEP) {
            cli_printf(c, "\rcopied %lu bytes", (unsigned long)off);
            reported = off;
        }
    }

    if (rin) { tnfs_close(rin); } else { fclose(sin); }
    if (rout) {
        tnfs_close(rout);
    } else if (fclose(sout) != 0 && ok) {
        ok = false; msg = "copy: close error\r\n";
    }

    if (!ok) {
        cli_write(c, msg ? msg : "copy: failed\r\n");
        return 1;
    }
    cli_printf(c, "\rcopied %lu bytes\r\n", (unsigned long)off);
    return 0;
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
    cli_printf(c, "  %-10s%lu\r\n", "STAT",     (unsigned long)s.stat);
    cli_printf(c, "  %-10s%lu\r\n", "READ",     (unsigned long)s.read);
    cli_printf(c, "  %-10s%lu\r\n", "WRIT",     (unsigned long)s.writ);
    cli_printf(c, "  %-10s%lu\r\n", "rd-retry", (unsigned long)s.read_retry);
    cli_printf(c, "  %-10s%lu\r\n", "not-rdy",  (unsigned long)s.not_ready);
    cli_printf(c, "  %-10s%-6lu(bad command header from FDC)\r\n",
               "cmd-csum", (unsigned long)s.cmd_csum);
    cli_printf(c, "  %-10s%-6lu(bad WRIT track data from FDC)\r\n",
               "data-csum", (unsigned long)s.data_csum);
    cli_printf(c, "  %-10s%-6lu(command header truncated)\r\n",
               "cmd-tmo", (unsigned long)s.cmd_timeout);
    cli_printf(c, "  %-10s%-6lu(WRIT track data incomplete)\r\n",
               "data-tmo", (unsigned long)s.data_timeout);
    cli_printf(c, "  %-10s%-6lu(RX overflow: bytes lost, ISR starved)\r\n",
               "fifo-ovf", (unsigned long)s.fifo_ovf);
    cli_printf(c, "  %-10s%-6lu(RX framing: baud / electrical)\r\n",
               "frame-err", (unsigned long)s.frame_err);
    cli_printf(c, "  %-10s%lu\r\n", "unknown",  (unsigned long)s.unknown);
    cli_printf(c, "  %-10s%s\r\n",  "last",
               s.last_op[0] ? s.last_op : "(none)");
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
        cli_printf(c, "State: connected  IP %s\r\n", s.ip);
        cli_printf(c, "AP:    %02x:%02x:%02x:%02x:%02x:%02x  RSSI %d dBm\r\n",
                   s.bssid[0], s.bssid[1], s.bssid[2],
                   s.bssid[3], s.bssid[4], s.bssid[5], s.rssi);
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
    { "Eastern",  "EST5EDT" },
    { "Central",  "CST6CDT" },
    { "Mountain", "MST7MDT" },
    { "Arizona",  "MST7" },
    { "Pacific",  "PST8PDT" },
    { "Alaska",   "AKST9AKDT" },
    { "Hawaii",   "HST10" },
};
#define TZ_COUNT (sizeof(k_tzs) / sizeof(k_tzs[0]))

static int cmd_tz(cli_console_t *c, int argc, char **argv)
{
    if (argc < 2) {
        cli_printf(c, "timezone: %s\r\n", config_get()->time_zone);
        return 0;
    }

    /* Accept a US name (case-insensitive) or a raw POSIX TZ string (§8.3).
     * The list of names is shown by `help tz`. */
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
    /* Only a network session can be logged out; the USB serial console stays open. */
    if (!c->is_network) {
        cli_write(c, "logout applies only to network sessions\r\n");
        return 0;
    }
    cli_write(c, "Goodbye\r\n");
    c->disconnect = true; /* the TCP console loop closes the socket */
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
        /* Network OTA from the configured repo; installs the repo image whatever its
         * version relative to the running build (§11.1). */
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
    /* Fold a running batch's cwd into the name so `exec sub/x.bat` and bare-name
     * auto-run resolve under the current batch's own folder (DESIGN.md §8.2). */
    char qname[2 * CONFIG_FILE_CAP];
    if (!qualify(name, qname, sizeof qname)) {
        return false;
    }
    if (sd_path(qname, buf, len) >= 0 && file_is_regular(buf)) {
        return true;
    }
    size_t nl = strlen(qname);
    bool has_bat = (nl >= 4) && strcasecmp(qname + nl - 4, ".bat") == 0;
    if (!has_bat) {
        char withext[2 * CONFIG_FILE_CAP + 8];
        snprintf(withext, sizeof withext, "%s.bat", qname);
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

/* Derive a batch file's own directory (root-relative) from its resolved /sd/… path,
 * e.g. "/sd/basic/setup.bat" -> "basic"; a root-level batch -> "". Used to set the
 * batch cwd (DESIGN.md §8.2) while the batch runs. */
static void batch_dir_of(const char *abspath, char *buf, size_t len)
{
    const char *pfx = SD_MOUNT_POINT "/"; /* "/sd/" */
    const char *rel = abspath;
    size_t pl = strlen(pfx);
    if (strncmp(abspath, pfx, pl) == 0) {
        rel = abspath + pl;
    }
    const char *slash = strrchr(rel, '/'); /* split the filename off the directory */
    size_t dlen = slash ? (size_t)(slash - rel) : 0;
    if (dlen >= len) {
        dlen = len - 1;
    }
    memcpy(buf, rel, dlen);
    buf[dlen] = '\0';
}

/* Run a resolved path under the nesting guard, with the batch cwd set to the batch's
 * own folder for its duration (DESIGN.md §8.2). Saving and restoring the cwd across the
 * recursive call forms the cwd stack: a nested `exec` resolves under its parent's
 * folder, then the parent's cwd is put back when the child returns. */
static void batch_exec_path(cli_console_t *c, const char *path)
{
    if (c->exec_depth >= BATCH_MAX_DEPTH) {
        cli_write(c, "exec: batch files nested too deep\r\n");
        return;
    }
    char saved[CONFIG_FILE_CAP];
    snprintf(saved, sizeof saved, "%s", s_batch_cwd);
    batch_dir_of(path, s_batch_cwd, sizeof s_batch_cwd);
    c->exec_depth++;
    run_batch_file(c, path);
    c->exec_depth--;
    snprintf(s_batch_cwd, sizeof s_batch_cwd, "%s", saved);
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

/* ---- diag: one-shot support summary (copy/paste into an email) ------------- */

/* Human-readable last-reset cause for the diag report. */
static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software (reboot/update)";
    case ESP_RST_PANIC:     return "panic / exception";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_BROWNOUT:  return "brownout (power dip)";
    case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "unknown";
    }
}

/*
 * `diag` — gather every status the other commands show (version, config/NVS, WiFi,
 * SD, drives, FDC+ stats) into one report, plus a few hardware facts a support reply
 * wants (chip/MAC, uptime, free heap, last-reset cause). Read-only; no secrets echoed.
 */
static int cmd_diag(cli_console_t *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const config_t *cfg = config_get();

    cli_write(c, "==== FDC+ Serial Disk Server diagnostics ====\r\n");

    /* -- Firmware / hardware -- */
    cli_printf(c, "Firmware:  %s %s\r\n", FDCSDS_PRODUCT, FDCSDS_VERSION_STRING);
    cli_printf(c, "ESP-IDF:   %s\r\n", esp_get_idf_version());

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    /* IDF 5.x encodes silicon revision as major*100 + minor (e.g. 301 -> v3.1). */
    cli_printf(c, "Chip:      ESP32 rev v%d.%d, %d core(s)\r\n",
               chip.revision / 100, chip.revision % 100, chip.cores);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    cli_printf(c, "MAC:       %02x:%02x:%02x:%02x:%02x:%02x\r\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* Seconds fit a 32-bit count for any realistic uptime (~136 years). */
    unsigned long up = (unsigned long)(esp_timer_get_time() / 1000000);
    cli_printf(c, "Uptime:    %lud %02lu:%02lu:%02lu\r\n",
               up / 86400, (up % 86400) / 3600, (up % 3600) / 60, up % 60);
    cli_printf(c, "Free heap: %lu bytes (min %lu since boot)\r\n",
               (unsigned long)esp_get_free_heap_size(),
               (unsigned long)esp_get_minimum_free_heap_size());
    cli_printf(c, "Last boot: %s\r\n", reset_reason_str());

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year < (2016 - 1900)) {
        cli_write(c, "Time:      not set (NTP not synced; needs WiFi)\r\n");
    } else {
        char tbuf[64];
        strftime(tbuf, sizeof tbuf, "%a %Y-%m-%d %H:%M:%S %Z", &tm);
        cli_printf(c, "Time:      %s\r\n", tbuf);
    }

    /* -- Saved configuration (NVS) -- */
    cli_write(c, "\r\n-- Configuration (NVS) --\r\n");
    cli_printf(c, "Saved:     %s\r\n",
               config_dirty() ? "NO - unsaved changes (run 'save')" : "yes");
    cli_printf(c, "Hostname:  %s\r\n", cfg->wifi_name);
    cli_printf(c, "FDC+ baud: %lu\r\n", (unsigned long)cfg->baud_rate);
    cli_printf(c, "Log level: %s\r\n", log_level_name(cfg->log_level));
    cli_printf(c, "Timezone:  %s\r\n", cfg->time_zone[0] ? cfg->time_zone : "(unset)");
    cli_printf(c, "FTP user:  %s (password %s)\r\n",
               cfg->ftp_user[0] ? cfg->ftp_user : "(unset)",
               cfg->ftp_pass[0] ? "set" : "unset");
    cli_printf(c, "OTA repo:  %s\r\n", cfg->ota_repo[0] ? cfg->ota_repo : "(unset)");

    /* -- WiFi -- */
    cli_write(c, "\r\n-- WiFi --\r\n");
    net_status_t ns;
    net_get_status(&ns);
    cli_printf(c, "WiFi:      %s\r\n", ns.enabled ? "enabled" : "disabled");
    cli_printf(c, "SSID:      %s (password %s)\r\n",
               ns.ssid[0] ? ns.ssid : "(unset)",
               cfg->wifi_pass[0] ? "set" : "unset");
    if (ns.connected) {
        cli_printf(c, "State:     connected  IP %s\r\n", ns.ip);
        cli_printf(c, "AP:        %02x:%02x:%02x:%02x:%02x:%02x  RSSI %d dBm\r\n",
                   ns.bssid[0], ns.bssid[1], ns.bssid[2],
                   ns.bssid[3], ns.bssid[4], ns.bssid[5], ns.rssi);
        cli_printf(c, "mDNS:      %s.local\r\n", cfg->wifi_name);
    } else {
        cli_printf(c, "State:     %s\r\n", ns.enabled ? "connecting/disconnected" : "idle");
    }

    /* -- SD card + autoexec -- */
    cli_write(c, "\r\n-- SD card --\r\n");
    if (!sd_mounted()) {
        cli_write(c, "SD card:   not present / not mounted\r\n");
    } else {
        cli_write(c, "SD card:   present\r\n");
        char path[16 + CONFIG_FILE_CAP];
        bool have_autoexec = sd_path("autoexec.bat", path, sizeof path) >= 0 &&
                             file_is_regular(path);
        cli_printf(c, "autoexec.bat: %s\r\n", have_autoexec ? "present" : "none");
    }

    /* -- Drives (mount table) -- */
    cli_write(c, "\r\n-- Drives --\r\n");
    for (int i = 0; i < CONFIG_MAX_DRIVE; ++i) {
        if (disk_is_mounted(i)) {
            cli_printf(c, "%d: %s (%lu bytes)%s\r\n", i, disk_name(i),
                       (unsigned long)disk_image_size(i),
                       disk_is_readonly(i) ? " [read-only]" : "");
        } else if (disk_is_remote(i)) {
            cli_printf(c, "%d: %s (not ready)\r\n", i, disk_name(i));
        } else {
            cli_printf(c, "%d: (empty)\r\n", i);
        }
    }

    /* -- FDC+ link + counters -- */
    cli_write(c, "\r\n-- FDC+ link --\r\n");
    fdc_stats_t s;
    fdc_get_stats(&s);
    cli_printf(c, "Link:      %lu baud\r\n", (unsigned long)fdc_baud());
    cli_printf(c, "STAT %lu  READ %lu  WRIT %lu  rd-retry %lu  not-rdy %lu  unknown %lu\r\n",
               (unsigned long)s.stat, (unsigned long)s.read, (unsigned long)s.writ,
               (unsigned long)s.read_retry, (unsigned long)s.not_ready,
               (unsigned long)s.unknown);
    cli_printf(c, "cmd-csum %lu  data-csum %lu  cmd-tmo %lu  data-tmo %lu  "
                  "fifo-ovf %lu  frame-err %lu\r\n",
               (unsigned long)s.cmd_csum, (unsigned long)s.data_csum,
               (unsigned long)s.cmd_timeout, (unsigned long)s.data_timeout,
               (unsigned long)s.fifo_ovf, (unsigned long)s.frame_err);
    cli_printf(c, "Last op:   %s\r\n", s.last_op[0] ? s.last_op : "(none)");

    cli_write(c, "==== end diagnostics ====\r\n");
    return 0;
}

/* ---- help (defined last so it can list the live baud/log/tz value tables) --- */

static int cmd_help(cli_console_t *c, int argc, char **argv)
{
    if (argc > 1) {
        /* `help <command>`: full help for one command. `? <command>` lands here too,
         * since `?` is help's alias. */
        const cli_command_t *t = NULL;
        for (size_t i = 0; i < CMD_COUNT; ++i) {
            if (strcasecmp(argv[1], k_commands[i].name) == 0 ||
                (k_commands[i].alias && strcasecmp(argv[1], k_commands[i].alias) == 0)) {
                t = &k_commands[i];
                break;
            }
        }
        if (!t) {
            cli_printf(c, "%s: no such command (try 'help')\r\n", argv[1]);
            return 1;
        }

        if (t->alias) {
            cli_printf(c, "%s (%s) - %s\r\n", t->name, t->alias, t->help);
        } else {
            cli_printf(c, "%s - %s\r\n", t->name, t->help);
        }
        if (t->detail) {
            cli_write(c, "\r\n");
            cli_write(c, t->detail);
        }

        /* Append the live list of valid values for the enumerated commands, so help
         * always matches exactly what the command accepts (no drift). */
        if (t->fn == cmd_baud) {
            cli_write(c, "valid rates:");
            for (size_t i = 0; i < BAUD_COUNT; ++i) {
                cli_printf(c, " %lu", (unsigned long)k_bauds[i]);
            }
            cli_write(c, "\r\n");
        } else if (t->fn == cmd_log) {
            cli_write(c, "levels:");
            for (size_t i = 0; i < LOG_LEVEL_COUNT; ++i) {
                cli_printf(c, " %s", k_log_levels[i].name);
            }
            cli_write(c, "  (also off = none, on = info)\r\n");
        } else if (t->fn == cmd_tz) {
            cli_write(c, "zones (name -> POSIX TZ):\r\n");
            for (size_t i = 0; i < TZ_COUNT; ++i) {
                cli_printf(c, "  %-9s %s\r\n", k_tzs[i].name, k_tzs[i].tz);
            }
        }
        return 0;
    }

    /* List commands alphabetically by name (insertion sort over a pointer index, so the
     * table itself keeps its milestone grouping). */
    const cli_command_t *sorted[CMD_COUNT];
    for (size_t i = 0; i < CMD_COUNT; ++i) {
        sorted[i] = &k_commands[i];
    }
    for (size_t i = 1; i < CMD_COUNT; ++i) {
        const cli_command_t *key = sorted[i];
        size_t j = i;
        while (j > 0 && strcasecmp(sorted[j - 1]->name, key->name) > 0) {
            sorted[j] = sorted[j - 1];
            --j;
        }
        sorted[j] = key;
    }

    cli_write(c, "Commands:\r\n");
    for (size_t i = 0; i < CMD_COUNT; ++i) {
        const cli_command_t *t = sorted[i];
        char name[24];
        if (t->alias) {
            snprintf(name, sizeof name, "%s/%s", t->name, t->alias);
        } else {
            snprintf(name, sizeof name, "%s", t->name);
        }
        cli_printf(c, "  %-16s %s\r\n", name, t->help);
    }
    cli_write(c, "\r\nType 'help <command>' for full help on one command.\r\n");
    return 0;
}
