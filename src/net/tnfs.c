/*
 * tnfs.c — compact TNFS client (remote disk images, DESIGN.md §9.5).
 *
 * Implements just enough of the FujiNet TNFS protocol to mount a server root and
 * read/write one file: MOUNT/UMOUNT, OPEN/CLOSE, LSEEK, READ/WRITE. Every message
 * is <2-byte session id LE><1-byte sequence><1-byte command><payload>; responses
 * echo the header and put a status byte at offset 4 (0 = success).
 *
 * Transport: DESIGN.md §9.5 makes UDP (port 16384) the default, with TCP optional.
 * We try UDP first and fall back to TCP on the same port, because cloud-hosted
 * tnfsd is commonly reachable only over TCP. On UDP each request is a self-framed
 * datagram, retransmitted on timeout with the same sequence number. TCP is a byte
 * stream with the *same* messages and no length prefix, so responses are framed by
 * reading the fixed header+status and then the exact command-specific remainder
 * (which is why file size comes from a fixed-size LSEEK(SEEK_END) rather than the
 * variable-length STAT reply). A new sequence number is taken per successful
 * command.
 *
 * One tnfs_file_t owns one socket + session + open fd and carries its own request
 * and response buffers (on the heap, not the caller's stack), so nested calls stay
 * shallow. There is no sharing between sessions, so no locking here — callers
 * serialize access (the SD mutex for a mounted drive; single-threaded copy).
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"

#include "tnfs.h"

static const char *TAG = "tnfs";

/* ---- protocol constants (from the FujiNet TNFS spec) ----------------------- */

#define TNFS_HDR_LEN     4     /* session(2) + seq(1) + cmd(1) */
#define TNFS_STATUS_OFF  4     /* status byte in every response */
#define TNFS_IO_CHUNK    1024  /* max data bytes per READ/WRITE (fits a UDP datagram) */
#define TNFS_MSG_CAP     (TNFS_HDR_LEN + 4 + TNFS_IO_CHUNK) /* biggest message we handle */
#define TNFS_RETRIES     3     /* UDP retransmits (also caps the UDP probe before TCP) */
#define TNFS_UDP_MS      700   /* per-attempt UDP receive timeout */
#define TNFS_TCP_MS      3000  /* TCP receive timeout */

/* Commands. */
#define TNFS_MOUNT   0x00
#define TNFS_UMOUNT  0x01
#define TNFS_OPENDIR  0x10
#define TNFS_READDIR  0x11
#define TNFS_CLOSEDIR 0x12
#define TNFS_OPENDIRX 0x17
#define TNFS_READDIRX 0x18
#define TNFS_READ    0x21
#define TNFS_WRITE   0x22
#define TNFS_CLOSE   0x23
#define TNFS_LSEEK   0x25
#define TNFS_OPEN    0x29

/* OPEN flags (little-endian bit field). */
#define TNFS_O_RDONLY 0x0001
#define TNFS_O_WRONLY 0x0002
#define TNFS_O_RDWR   0x0003
#define TNFS_O_CREAT  0x0100
#define TNFS_O_TRUNC  0x0200

/* Status codes we special-case. */
#define TNFS_OK      0x00
#define TNFS_ENOENT  0x02
#define TNFS_EAGAIN  0x07   /* retry after a little-endian backoff (ms) at offset 5 */
#define TNFS_EOF     0x21

/* LSEEK whence. */
#define TNFS_SEEK_SET 0x00
#define TNFS_SEEK_END 0x02

/* READDIRX per-entry (dirent) flags and reply dir-status flags. */
#define TNFS_DIRENTRY_DIR  0x01  /* entry is a directory */
#define TNFS_DIRSTATUS_EOF 0x01  /* last batch: end of directory reached */
#define TNFS_READDIRX_FIXED 13   /* dirent bytes before the name: flags(1)+size(4)+mtime(4)+ctime(4) */

/* Protocol version we advertise in MOUNT: 1.2, bytes {minor, major} = {0x02, 0x01}. */
#define TNFS_VER_LO 0x02
#define TNFS_VER_HI 0x01

struct tnfs_file {
    int      sock;
    bool     tcp;        /* transport in use */
    uint16_t conn;       /* session id from MOUNT */
    uint8_t  seq;        /* next sequence number */
    uint8_t  fd;         /* remote file descriptor from OPEN */
    uint32_t size;       /* file size captured at open */
    uint32_t pos;        /* server-side file position (to elide redundant LSEEKs) */
    bool     pos_valid;
    size_t   resp_len;   /* length of the last response in resp[] */
    uint8_t  req[TNFS_MSG_CAP];
    uint8_t  resp[TNFS_MSG_CAP];
};

/* ---- little-endian helpers ------------------------------------------------- */

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ---- transport ------------------------------------------------------------- */

/* Read exactly n bytes from a TCP stream into buf. False on timeout/close. */
static bool tcp_read_exact(int sock, uint8_t *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        int r = recv(sock, buf + off, n - off, 0);
        if (r <= 0) {
            return false;
        }
        off += (size_t)r;
    }
    return true;
}

/*
 * Frame one TCP response into f->resp: the 4-byte header + status, then the exact
 * command-specific remainder. Sets f->resp_len. TNFS over TCP has no length prefix,
 * so the length of a reply is derived from its command and status.
 */
static esp_err_t tcp_response(tnfs_file_t *f, uint8_t cmd)
{
    if (!tcp_read_exact(f->sock, f->resp, TNFS_STATUS_OFF + 1)) {
        return ESP_ERR_TIMEOUT;
    }
    size_t total = TNFS_STATUS_OFF + 1;
    uint8_t status = f->resp[TNFS_STATUS_OFF];
    bool ok = (status == TNFS_OK) || (cmd == TNFS_READ && status == TNFS_EOF);

    if (status == TNFS_EAGAIN) {
        if (!tcp_read_exact(f->sock, f->resp + total, 2)) { /* backoff ms */
            return ESP_ERR_TIMEOUT;
        }
        total += 2;
    } else if (ok) {
        size_t extra = 0;
        switch (cmd) {
        case TNFS_MOUNT:    extra = 4; break; /* version(2) + retry(2) */
        case TNFS_OPEN:     extra = 1; break; /* fd */
        case TNFS_OPENDIR:  extra = 1; break; /* dir handle */
        case TNFS_OPENDIRX: extra = 3; break; /* dir handle(1) + entry count(2) */
        case TNFS_LSEEK:    extra = 4; break; /* position */
        case TNFS_WRITE:    extra = 2; break; /* count */
        case TNFS_READ:     extra = 2; break; /* count, then that many data bytes */
        default: break;
        }
        if (extra) {
            if (!tcp_read_exact(f->sock, f->resp + total, extra)) {
                return ESP_ERR_TIMEOUT;
            }
            total += extra;
        }
        if (cmd == TNFS_READ) {
            uint16_t cnt = rd16(&f->resp[TNFS_STATUS_OFF + 1]);
            if (total + cnt > sizeof f->resp) {
                return ESP_FAIL;
            }
            if (cnt && !tcp_read_exact(f->sock, f->resp + total, cnt)) {
                return ESP_ERR_TIMEOUT;
            }
            total += cnt;
        }
        /* READDIR reply is a NUL-terminated name with no length prefix; read to the
         * NUL one byte at a time (names are short). EOF status carries no name and is
         * handled above by `ok` being false. */
        if (cmd == TNFS_READDIR) {
            for (;;) {
                if (total >= sizeof f->resp) {
                    return ESP_FAIL;
                }
                if (!tcp_read_exact(f->sock, f->resp + total, 1)) {
                    return ESP_ERR_TIMEOUT;
                }
                if (f->resp[total++] == '\0') {
                    break;
                }
            }
        }
        /* READDIRX reply: count(1) + dirstatus(1) + dirpos(2), then `count` dirents,
         * each TNFS_READDIRX_FIXED bytes + a NUL-terminated name. No length prefix, so
         * frame it from the count. */
        if (cmd == TNFS_READDIRX) {
            if (!tcp_read_exact(f->sock, f->resp + total, 4)) {
                return ESP_ERR_TIMEOUT;
            }
            uint8_t count = f->resp[total];
            total += 4;
            for (uint8_t i = 0; i < count; ++i) {
                if (total + TNFS_READDIRX_FIXED > sizeof f->resp) {
                    return ESP_FAIL;
                }
                if (!tcp_read_exact(f->sock, f->resp + total, TNFS_READDIRX_FIXED)) {
                    return ESP_ERR_TIMEOUT;
                }
                total += TNFS_READDIRX_FIXED;
                for (;;) {
                    if (total >= sizeof f->resp) {
                        return ESP_FAIL;
                    }
                    if (!tcp_read_exact(f->sock, f->resp + total, 1)) {
                        return ESP_ERR_TIMEOUT;
                    }
                    if (f->resp[total++] == '\0') {
                        break;
                    }
                }
            }
        }
    }
    f->resp_len = total;
    return ESP_OK;
}

/* Receive one matching UDP datagram into f->resp. Returns length, or -1 on timeout. */
static int udp_response(tnfs_file_t *f, uint8_t seq, uint8_t cmd)
{
    for (;;) {
        int n = recv(f->sock, f->resp, sizeof f->resp, 0);
        if (n < 0) {
            return -1; /* SO_RCVTIMEO elapsed */
        }
        if (n < TNFS_STATUS_OFF + 1) {
            continue; /* runt */
        }
        if (f->resp[2] != seq || f->resp[3] != cmd) {
            continue; /* stale reply to an earlier command */
        }
        return n;
    }
}

/*
 * Send command `cmd` with a payload the caller has already placed at
 * f->req[TNFS_HDR_LEN .. TNFS_HDR_LEN+plen-1], then receive the response into
 * f->resp (status at f->resp[TNFS_STATUS_OFF], length in f->resp_len) and advance
 * the sequence number. Retransmits on UDP timeout and honors an EAGAIN backoff.
 */
static esp_err_t tnfs_command(tnfs_file_t *f, uint8_t cmd, size_t plen)
{
    if (TNFS_HDR_LEN + plen > sizeof f->req) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t s = f->seq;
    wr16(f->req, f->conn);
    f->req[2] = s;
    f->req[3] = cmd;
    size_t reqlen = TNFS_HDR_LEN + plen;

    for (int attempt = 0; attempt < TNFS_RETRIES; ++attempt) {
        if (send(f->sock, f->req, reqlen, 0) < 0) {
            return ESP_FAIL;
        }
        if (f->tcp) {
            esp_err_t e = tcp_response(f, cmd);
            if (e != ESP_OK) {
                return e; /* a broken stream is fatal, not retryable */
            }
        } else {
            int n = udp_response(f, s, cmd);
            if (n < 0) {
                continue; /* timeout -> retransmit */
            }
            f->resp_len = (size_t)n;
        }
        if (f->resp[TNFS_STATUS_OFF] == TNFS_EAGAIN) {
            uint32_t ms = (f->resp_len >= TNFS_STATUS_OFF + 3) ? rd16(&f->resp[TNFS_STATUS_OFF + 1]) : 100;
            vTaskDelay(pdMS_TO_TICKS(ms ? ms : 100));
            continue; /* server busy -> retransmit after backoff */
        }
        f->seq = (uint8_t)(s + 1);
        return ESP_OK;
    }
    /* Debug-level: a MOUNT timeout here is the *expected* UDP probe failing before the
     * TCP fallback (tnfs_open), not an error worth a console warning. */
    ESP_LOGD(TAG, "cmd 0x%02x no response", cmd);
    return ESP_ERR_TIMEOUT;
}

/* Map a TNFS status byte to an esp_err_t for the open path. */
static esp_err_t status_err(uint8_t status)
{
    switch (status) {
    case TNFS_OK:     return ESP_OK;
    case TNFS_ENOENT: return ESP_ERR_NOT_FOUND;
    default:          return ESP_FAIL;
    }
}

/* ---- URL parsing ----------------------------------------------------------- */

bool tnfs_is_url(const char *s)
{
    return s && strncasecmp(s, "tnfs://", 7) == 0;
}

esp_err_t tnfs_url_parse(const char *url, char *host, size_t host_cap,
                         uint16_t *port, char *path, size_t path_cap)
{
    if (!tnfs_is_url(url) || !host || !path || !port) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *p = url + 7;               /* past "tnfs://" */
    const char *slash = strchr(p, '/');    /* start of the path */
    if (!slash) {
        return ESP_ERR_INVALID_ARG;        /* need a file path */
    }
    const char *colon = memchr(p, ':', (size_t)(slash - p)); /* optional :port */
    const char *host_end = colon ? colon : slash;

    size_t host_len = (size_t)(host_end - p);
    if (host_len == 0 || host_len >= host_cap) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(host, p, host_len);
    host[host_len] = '\0';

    uint32_t pn = TNFS_DEFAULT_PORT;
    if (colon) {
        pn = 0;
        for (const char *d = colon + 1; d < slash; ++d) {
            if (*d < '0' || *d > '9') {
                return ESP_ERR_INVALID_ARG;
            }
            pn = pn * 10 + (uint32_t)(*d - '0');
            if (pn > 65535) {
                return ESP_ERR_INVALID_ARG;
            }
        }
        if (pn == 0) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    *port = (uint16_t)pn;

    size_t path_len = strlen(slash);       /* keeps the leading '/' */
    if (path_len == 0 || path_len >= path_cap) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(path, slash, path_len + 1);
    return ESP_OK;
}

/* ---- connect + session ----------------------------------------------------- */

/* Resolve host:port and connect a socket of `type` (SOCK_DGRAM/SOCK_STREAM). */
static int tnfs_connect(const char *host, uint16_t port, int type, int recv_ms)
{
    char portstr[6];
    snprintf(portstr, sizeof portstr, "%u", (unsigned)port);

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = type };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "resolve '%s' failed", host);
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock >= 0) {
        struct timeval tv = { .tv_sec = recv_ms / 1000, .tv_usec = (recv_ms % 1000) * 1000 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
            close(sock);
            sock = -1;
        }
    }
    freeaddrinfo(res);
    return sock;
}

/* MOUNT "/" on the current socket; on success stores the session id in f->conn. */
static esp_err_t tnfs_mount_root(tnfs_file_t *f)
{
    f->seq = 0;
    f->conn = 0;
    uint8_t *pl = &f->req[TNFS_HDR_LEN];
    pl[0] = TNFS_VER_LO;
    pl[1] = TNFS_VER_HI;
    pl[2] = '/';
    pl[3] = '\0';   /* mount path "/" */
    pl[4] = '\0';   /* empty user */
    pl[5] = '\0';   /* empty password */

    esp_err_t err = tnfs_command(f, TNFS_MOUNT, 6);
    if (err != ESP_OK) {
        return err;
    }
    if (f->resp[TNFS_STATUS_OFF] != TNFS_OK) {
        ESP_LOGW(TAG, "MOUNT rejected (status 0x%02x)", f->resp[TNFS_STATUS_OFF]);
        return ESP_FAIL;
    }
    f->conn = rd16(f->resp); /* session id echoed in the response header */
    return ESP_OK;
}

/* Try to MOUNT the server over `type` transport; leaves the socket open on success. */
static esp_err_t tnfs_try(tnfs_file_t *f, const char *host, uint16_t port, bool tcp)
{
    f->tcp = tcp;
    f->sock = tnfs_connect(host, port, tcp ? SOCK_STREAM : SOCK_DGRAM,
                           tcp ? TNFS_TCP_MS : TNFS_UDP_MS);
    if (f->sock < 0) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = tnfs_mount_root(f);
    if (err != ESP_OK) {
        close(f->sock);
        f->sock = -1;
    }
    return err;
}

/* LSEEK to the end of the file to learn its size (fixed-length reply). */
static esp_err_t tnfs_query_size(tnfs_file_t *f)
{
    uint8_t *pl = &f->req[TNFS_HDR_LEN];
    pl[0] = f->fd;
    pl[1] = TNFS_SEEK_END;
    wr32(&pl[2], 0);
    esp_err_t err = tnfs_command(f, TNFS_LSEEK, 6);
    if (err != ESP_OK || f->resp[TNFS_STATUS_OFF] != TNFS_OK || f->resp_len < TNFS_STATUS_OFF + 5) {
        return ESP_FAIL;
    }
    f->size = rd32(&f->resp[TNFS_STATUS_OFF + 1]);
    f->pos_valid = false; /* left the position at EOF */
    return ESP_OK;
}

esp_err_t tnfs_open(const char *url, bool writable, bool create, tnfs_file_t **out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    char host[64];
    char path[160];
    uint16_t port = TNFS_DEFAULT_PORT;
    esp_err_t err = tnfs_url_parse(url, host, sizeof host, &port, path, sizeof path);
    if (err != ESP_OK) {
        return err;
    }

    tnfs_file_t *f = calloc(1, sizeof *f);
    if (!f) {
        return ESP_ERR_NO_MEM;
    }

    /* UDP is the default; fall back to TCP (some servers only answer over TCP). */
    if (tnfs_try(f, host, port, false) != ESP_OK &&
        tnfs_try(f, host, port, true) != ESP_OK) {
        ESP_LOGW(TAG, "cannot mount %s:%u (UDP or TCP)", host, port);
        free(f);
        return ESP_ERR_TIMEOUT;
    }

    /* OPEN the path. */
    uint16_t flags = create ? (uint16_t)(TNFS_O_WRONLY | TNFS_O_CREAT | TNFS_O_TRUNC)
                            : (writable ? TNFS_O_RDWR : TNFS_O_RDONLY);
    size_t path_len = strlen(path) + 1;
    if (4 + path_len > sizeof f->req - TNFS_HDR_LEN) {
        goto fail_open;
    }
    wr16(&f->req[TNFS_HDR_LEN + 0], flags);
    wr16(&f->req[TNFS_HDR_LEN + 2], 0x01A4); /* mode 0644 for a created file */
    memcpy(&f->req[TNFS_HDR_LEN + 4], path, path_len);

    err = tnfs_command(f, TNFS_OPEN, 4 + path_len);
    if (err == ESP_OK) {
        err = status_err(f->resp[TNFS_STATUS_OFF]);
    }
    if (err != ESP_OK || f->resp_len < TNFS_STATUS_OFF + 2) {
        if (err == ESP_OK) {
            err = ESP_FAIL;
        }
        ESP_LOGW(TAG, "OPEN '%s' failed", path);
        goto fail_open;
    }
    f->fd = f->resp[TNFS_STATUS_OFF + 1];

    /* Size from LSEEK(SEEK_END); 0 for a just-created file. */
    f->size = 0;
    if (!create) {
        tnfs_query_size(f);
    }
    f->pos_valid = false;

    ESP_LOGI(TAG, "opened %s over %s (%lu bytes, fd %u)", url, f->tcp ? "TCP" : "UDP",
             (unsigned long)f->size, f->fd);
    *out = f;
    return ESP_OK;

fail_open:
    /* Best-effort UMOUNT so the server reaps the session promptly. */
    tnfs_command(f, TNFS_UMOUNT, 0);
    close(f->sock);
    free(f);
    return (err == ESP_OK) ? ESP_FAIL : err;
}

uint32_t tnfs_size(tnfs_file_t *f)
{
    return f ? f->size : 0;
}

/* Seek the server-side position to `off` unless it is already there. */
static esp_err_t tnfs_seek(tnfs_file_t *f, uint32_t off)
{
    if (f->pos_valid && f->pos == off) {
        return ESP_OK;
    }
    uint8_t *pl = &f->req[TNFS_HDR_LEN];
    pl[0] = f->fd;
    pl[1] = TNFS_SEEK_SET;
    wr32(&pl[2], off);

    esp_err_t err = tnfs_command(f, TNFS_LSEEK, 6);
    if (err != ESP_OK || f->resp[TNFS_STATUS_OFF] != TNFS_OK) {
        f->pos_valid = false;
        return (err == ESP_OK) ? ESP_FAIL : err;
    }
    f->pos = off;
    f->pos_valid = true;
    return ESP_OK;
}

esp_err_t tnfs_read_at(tnfs_file_t *f, uint32_t off, void *buf, size_t len)
{
    if (!f || !buf) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t *dst = buf;
    while (len > 0) {
        esp_err_t err = tnfs_seek(f, off);
        if (err != ESP_OK) {
            return err;
        }
        uint16_t want = (len > TNFS_IO_CHUNK) ? TNFS_IO_CHUNK : (uint16_t)len;
        uint8_t *pl = &f->req[TNFS_HDR_LEN];
        pl[0] = f->fd;
        wr16(&pl[1], want);

        err = tnfs_command(f, TNFS_READ, 3);
        if (err != ESP_OK) {
            f->pos_valid = false;
            return err;
        }
        uint8_t status = f->resp[TNFS_STATUS_OFF];
        if ((status != TNFS_OK && status != TNFS_EOF) || f->resp_len < TNFS_STATUS_OFF + 3) {
            f->pos_valid = false;
            return ESP_FAIL;
        }
        uint16_t got = rd16(&f->resp[TNFS_STATUS_OFF + 1]);
        if (got == 0 || got > want || f->resp_len < (size_t)(TNFS_STATUS_OFF + 3 + got)) {
            f->pos_valid = false;
            return ESP_FAIL; /* short read: caller asked within bounds */
        }
        memcpy(dst, &f->resp[TNFS_STATUS_OFF + 3], got);
        dst += got;
        off += got;
        len -= got;
        f->pos += got;
    }
    return ESP_OK;
}

esp_err_t tnfs_write_at(tnfs_file_t *f, uint32_t off, const void *buf, size_t len)
{
    if (!f || !buf) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t *src = buf;
    while (len > 0) {
        esp_err_t err = tnfs_seek(f, off);
        if (err != ESP_OK) {
            return err;
        }
        uint16_t want = (len > TNFS_IO_CHUNK) ? TNFS_IO_CHUNK : (uint16_t)len;
        uint8_t *pl = &f->req[TNFS_HDR_LEN];
        pl[0] = f->fd;
        wr16(&pl[1], want);
        memcpy(&pl[3], src, want);

        err = tnfs_command(f, TNFS_WRITE, (size_t)(3 + want));
        if (err != ESP_OK) {
            f->pos_valid = false;
            return err;
        }
        if (f->resp[TNFS_STATUS_OFF] != TNFS_OK || f->resp_len < TNFS_STATUS_OFF + 3) {
            f->pos_valid = false;
            return ESP_FAIL;
        }
        uint16_t put = rd16(&f->resp[TNFS_STATUS_OFF + 1]);
        if (put == 0 || put > want) {
            f->pos_valid = false;
            return ESP_FAIL;
        }
        src += put;
        off += put;
        len -= put;
        f->pos += put;
        if (off > f->size) {
            f->size = off;
        }
    }
    return ESP_OK;
}

void tnfs_close(tnfs_file_t *f)
{
    if (!f) {
        return;
    }
    uint8_t *pl = &f->req[TNFS_HDR_LEN];
    pl[0] = f->fd;
    tnfs_command(f, TNFS_CLOSE, 1);
    tnfs_command(f, TNFS_UMOUNT, 0);
    close(f->sock);
    free(f);
}

/*
 * Extended listing: OPENDIRX gives a per-entry directory flag (and size/times we don't
 * use). Read one entry per READDIRX (numwanted = 1) so a reply always fits resp[] and
 * TCP framing stays trivial. Fills *dh with the handle to CLOSEDIR. Returns
 * ESP_ERR_NOT_SUPPORTED if the server rejects OPENDIRX so the caller can fall back.
 */
static esp_err_t tnfs_listx(tnfs_file_t *f, const char *path, uint8_t *dh,
                            tnfs_dir_cb cb, void *ctx)
{
    /* OPENDIRX: diropt(1) + dirsort(1) + maxresults(2) + pattern(NUL) + path(NUL). */
    uint8_t *pl = &f->req[TNFS_HDR_LEN];
    size_t path_len = strlen(path) + 1;
    size_t plen = 5 + path_len; /* opts(2) + max(2) + empty pattern(1) + path */
    if (plen > sizeof f->req - TNFS_HDR_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    pl[0] = 0;              /* default directory options */
    pl[1] = 0;              /* default sort */
    wr16(&pl[2], 0);        /* unlimited results */
    pl[4] = '\0';           /* empty wildcard pattern (client filters) */
    memcpy(&pl[5], path, path_len);

    esp_err_t err = tnfs_command(f, TNFS_OPENDIRX, plen);
    if (err != ESP_OK) {
        return err;
    }
    if (f->resp[TNFS_STATUS_OFF] != TNFS_OK || f->resp_len < TNFS_STATUS_OFF + 2) {
        /* A not-found directory is real; anything else we treat as "no OPENDIRX" and
         * let the caller retry with basic OPENDIR. */
        return (f->resp[TNFS_STATUS_OFF] == TNFS_ENOENT) ? ESP_ERR_NOT_FOUND
                                                         : ESP_ERR_NOT_SUPPORTED;
    }
    *dh = f->resp[TNFS_STATUS_OFF + 1];

    for (;;) {
        pl = &f->req[TNFS_HDR_LEN];
        pl[0] = *dh;
        pl[1] = 1;          /* one entry per reply */
        err = tnfs_command(f, TNFS_READDIRX, 2);
        if (err != ESP_OK) {
            return err;
        }
        uint8_t status = f->resp[TNFS_STATUS_OFF];
        if (status == TNFS_EOF) {
            return ESP_OK;
        }
        if (status != TNFS_OK || f->resp_len < TNFS_STATUS_OFF + 1 + 4) {
            return ESP_FAIL;
        }
        /* Body: count(1) + dirstatus(1) + dirpos(2), then `count` dirents. */
        const uint8_t *p = &f->resp[TNFS_STATUS_OFF + 1];
        const uint8_t *end = &f->resp[f->resp_len];
        uint8_t count = p[0];
        uint8_t dstatus = p[1];
        p += 4;
        for (uint8_t i = 0; i < count; ++i) {
            if (p + TNFS_READDIRX_FIXED >= end) {
                return ESP_FAIL;
            }
            bool is_dir = (p[0] & TNFS_DIRENTRY_DIR) != 0;
            const uint8_t *name = p + TNFS_READDIRX_FIXED;
            const uint8_t *nul = memchr(name, '\0', (size_t)(end - name));
            if (!nul) {
                return ESP_FAIL;
            }
            if (name[0]) {
                cb((const char *)name, is_dir, ctx);
            }
            p = nul + 1;
        }
        if (dstatus & TNFS_DIRSTATUS_EOF) {
            return ESP_OK; /* server flagged the last batch — no extra round trip */
        }
    }
}

/*
 * Basic listing fallback: OPENDIR + READDIR return names only (no type), so every entry
 * is reported with is_dir = false. Fills *dh with the handle to CLOSEDIR.
 */
static esp_err_t tnfs_list_basic(tnfs_file_t *f, const char *path, uint8_t *dh,
                                 tnfs_dir_cb cb, void *ctx)
{
    size_t path_len = strlen(path) + 1;
    if (path_len > sizeof f->req - TNFS_HDR_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(&f->req[TNFS_HDR_LEN], path, path_len);
    esp_err_t err = tnfs_command(f, TNFS_OPENDIR, path_len);
    if (err == ESP_OK) {
        err = status_err(f->resp[TNFS_STATUS_OFF]);
    }
    if (err != ESP_OK || f->resp_len < TNFS_STATUS_OFF + 2) {
        return (err == ESP_OK) ? ESP_FAIL : err;
    }
    *dh = f->resp[TNFS_STATUS_OFF + 1];

    for (;;) {
        f->req[TNFS_HDR_LEN] = *dh;
        err = tnfs_command(f, TNFS_READDIR, 1);
        if (err != ESP_OK) {
            return err;
        }
        uint8_t status = f->resp[TNFS_STATUS_OFF];
        if (status == TNFS_EOF) {
            return ESP_OK;
        }
        if (status != TNFS_OK) {
            return ESP_FAIL;
        }
        char *name = (char *)&f->resp[TNFS_STATUS_OFF + 1];
        size_t avail = (f->resp_len > TNFS_STATUS_OFF + 1)
                           ? f->resp_len - (TNFS_STATUS_OFF + 1) : 0;
        if (avail == 0) {
            continue; /* malformed empty reply */
        }
        if (!memchr(name, '\0', avail)) {
            if (TNFS_STATUS_OFF + 1 + avail < sizeof f->resp) {
                name[avail] = '\0';
            } else {
                name[avail - 1] = '\0';
            }
        }
        if (name[0]) {
            cb(name, false, ctx);
        }
    }
}

esp_err_t tnfs_list_dir(const char *url, tnfs_dir_cb cb, void *ctx)
{
    if (!tnfs_is_url(url) || !cb) {
        return ESP_ERR_INVALID_ARG;
    }

    /* A bare `tnfs://host` (no path) means the server root; tnfs_url_parse needs a
     * path, so synthesize a trailing '/'. */
    char urlbuf[240];
    if (!strchr(url + 7, '/')) {
        int n = snprintf(urlbuf, sizeof urlbuf, "%s/", url);
        if (n < 0 || n >= (int)sizeof urlbuf) {
            return ESP_ERR_INVALID_ARG;
        }
        url = urlbuf;
    }

    char host[64];
    char path[160];
    uint16_t port = TNFS_DEFAULT_PORT;
    esp_err_t err = tnfs_url_parse(url, host, sizeof host, &port, path, sizeof path);
    if (err != ESP_OK) {
        return err;
    }

    tnfs_file_t *f = calloc(1, sizeof *f);
    if (!f) {
        return ESP_ERR_NO_MEM;
    }

    /* Transient session, same UDP-then-TCP probe as tnfs_open (§10.2). */
    if (tnfs_try(f, host, port, false) != ESP_OK &&
        tnfs_try(f, host, port, true) != ESP_OK) {
        ESP_LOGW(TAG, "cannot mount %s:%u (UDP or TCP)", host, port);
        free(f);
        return ESP_ERR_TIMEOUT;
    }

    /* Prefer the extended listing (per-entry directory flag); on a server without
     * OPENDIRX, retry with the basic names-only listing. */
    uint8_t dh = 0;
    err = tnfs_listx(f, path, &dh, cb, ctx);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGD(TAG, "OPENDIRX unsupported; using basic READDIR");
        err = tnfs_list_basic(f, path, &dh, cb, ctx);
    }
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "list '%s' failed", path);
    }

    /* CLOSEDIR (best effort) then tear down the session. */
    f->req[TNFS_HDR_LEN] = dh;
    tnfs_command(f, TNFS_CLOSEDIR, 1);
    tnfs_command(f, TNFS_UMOUNT, 0);
    close(f->sock);
    free(f);
    return err;
}
