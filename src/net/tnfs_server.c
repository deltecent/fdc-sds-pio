/*
 * tnfs_server.c — TNFS server: serve the SD card read/write over UDP (DESIGN.md §9.6).
 *
 * The opposite role from the client (tnfs.c): we bind UDP :16384 and let a desktop
 * TNFS client move files to/from the SD card. Shares only the wire constants in
 * tnfs_proto.h with the client; the logic is entirely separate (a client initiates and
 * retransmits; a server listens and replays).
 *
 * Protocol shape: every request is  session(2,LE) | seq(1) | cmd(1) | payload; every
 * reply echoes that header (with the session id we allocated in MOUNT) and puts a status
 * byte at offset 4. Clients retransmit a lost datagram with the *same* sequence number,
 * so the one hard correctness requirement is idempotency: we cache each session's last
 * reply and, on a duplicate (seq, cmd), replay it verbatim WITHOUT re-running the
 * operation — otherwise a replayed WRITE/UNLINK/MKDIR/RENAME would execute twice.
 *
 * No authentication (§9.6): the MOUNT user/password are ignored. The security control is
 * the tnfsdEnabled flag (§7) plus a trusted LAN. Everything is jailed under the SD root
 * via sd_path() (rejects '..'). One task on core 0, off the FDC path; file I/O goes
 * straight through the FATFS VFS, exactly like FTP (§9.4), relying on FATFS reentrancy.
 */
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "net.h"
#include "sd.h"
#include "tnfs_proto.h"
#include "tnfs_server.h"
#include "wildcard.h"

static const char *TAG = "tnfsd";

/* Resource bounds — a staging tool, not a fileserver (DESIGN.md §9.6). */
#define TNFSD_MAX_SESSIONS 2
#define TNFSD_MAX_FILES    4          /* open files per session */
#define TNFSD_MAX_DIRS     2          /* open directories per session */
#define TNFSD_IDLE_US      (30LL * 1000000) /* reap a session after ~30 s idle */
#define TNFSD_RECV_MS      1000       /* socket timeout, so the loop reaps + checks link */
#define TNFSD_MIN_RETRY_MS 1000       /* min-retry timeout advertised to clients in MOUNT */

#define TNFSD_ABS_CAP  320   /* absolute /sd/... path buffer */
#define TNFSD_ROOT_CAP 160   /* per-session mount subdir (root-relative, no leading '/') */
#define TNFSD_PAT_CAP  64    /* READDIRX wildcard pattern */
#define TNFSD_NAME_CAP 256   /* directory entry name (FAT LFN) */

/* ---- per-session open-directory slot --------------------------------------- */

typedef struct {
    bool     in_use;
    DIR     *dir;
    char     abspath[TNFSD_ABS_CAP];  /* absolute dir path, to stat entries */
    char     pattern[TNFSD_PAT_CAP];  /* READDIRX filter; "" = match all */
    uint16_t pos;                     /* running dirpos reported in READDIRX */
    /* One-entry read-ahead: a READDIRX entry that didn't fit the last reply is
     * stashed here so the next READDIRX emits it instead of skipping it. */
    bool     has_pending;
    uint8_t  p_flags;
    uint32_t p_size, p_mtime, p_ctime;
    char     p_name[TNFSD_NAME_CAP];
} dirslot_t;

/* ---- session ---------------------------------------------------------------- */

typedef struct {
    bool     in_use;
    uint16_t sid;                     /* session id we allocated (nonzero) */
    struct sockaddr_in peer;          /* identifies the client's datagrams */
    int64_t  last_active_us;
    char     root[TNFSD_ROOT_CAP];    /* mount subdir under /sd ("" = SD root) */
    int      files[TNFSD_MAX_FILES];  /* POSIX fds; -1 = free. TNFS fd = slot index */
    dirslot_t dirs[TNFSD_MAX_DIRS];   /* TNFS dir handle = slot index */
    /* Last reply, cached for idempotent retransmit replay. */
    bool     have_reply;
    uint8_t  last_seq;
    uint8_t  last_cmd;
    size_t   reply_len;
    uint8_t  reply[TNFS_MSG_CAP];
} session_t;

static int       s_sock = -1;
static session_t s_sessions[TNFSD_MAX_SESSIONS];
static uint16_t  s_next_sid = 1;

/* ---- helpers ---------------------------------------------------------------- */

static inline int64_t now_us(void) { return esp_timer_get_time(); }

/* Map a POSIX errno to the nearest TNFS status byte. */
static uint8_t errno_to_tnfs(int e)
{
    switch (e) {
    case 0:            return TNFS_OK;
    case ENOENT:       return TNFS_ENOENT;
    case EACCES:
    case EPERM:        return TNFS_EACCES;
    case EEXIST:       return TNFS_EEXIST;
    case ENOTDIR:      return TNFS_ENOTDIR;
    case EISDIR:       return TNFS_EISDIR;
    case EINVAL:       return TNFS_EINVAL;
    case EMFILE:
    case ENFILE:       return TNFS_EMFILE;
    case ENOSPC:       return TNFS_ENOSPC;
    case EROFS:        return TNFS_EROFS;
    case ENAMETOOLONG: return TNFS_ENAMETOOLONG;
    case ENOTEMPTY:    return TNFS_ENOTEMPTY;
    case EBADF:        return TNFS_EBADF;
    case ENOMEM:       return TNFS_ENOMEM;
    case EIO:          return TNFS_EIO;
    default:           return TNFS_EIO;
    }
}

static bool same_peer(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static session_t *find_session(uint16_t sid, const struct sockaddr_in *peer)
{
    if (sid == 0) {
        return NULL;
    }
    for (int i = 0; i < TNFSD_MAX_SESSIONS; ++i) {
        if (s_sessions[i].in_use && s_sessions[i].sid == sid &&
            same_peer(&s_sessions[i].peer, peer)) {
            return &s_sessions[i];
        }
    }
    return NULL;
}

/* Close any open files/dirs a session left and return its slot to the pool. */
static void free_session(session_t *s)
{
    for (int i = 0; i < TNFSD_MAX_FILES; ++i) {
        if (s->files[i] >= 0) {
            close(s->files[i]);
        }
    }
    for (int i = 0; i < TNFSD_MAX_DIRS; ++i) {
        if (s->dirs[i].in_use && s->dirs[i].dir) {
            closedir(s->dirs[i].dir);
        }
    }
    memset(s, 0, sizeof *s);
    for (int i = 0; i < TNFSD_MAX_FILES; ++i) {
        s->files[i] = -1;
    }
}

static session_t *alloc_session(void)
{
    for (int i = 0; i < TNFSD_MAX_SESSIONS; ++i) {
        if (!s_sessions[i].in_use) {
            free_session(&s_sessions[i]); /* zero + set fds to -1 */
            s_sessions[i].in_use = true;
            return &s_sessions[i];
        }
    }
    return NULL;
}

static void drop_all_sessions(void)
{
    for (int i = 0; i < TNFSD_MAX_SESSIONS; ++i) {
        if (s_sessions[i].in_use) {
            free_session(&s_sessions[i]);
        }
    }
}

static void reap_idle(void)
{
    int64_t t = now_us();
    for (int i = 0; i < TNFSD_MAX_SESSIONS; ++i) {
        if (s_sessions[i].in_use && (t - s_sessions[i].last_active_us) > TNFSD_IDLE_US) {
            ESP_LOGI(TAG, "session %u idle-timed out", s_sessions[i].sid);
            free_session(&s_sessions[i]);
        }
    }
}

static uint16_t next_sid(void)
{
    for (;;) {
        uint16_t cand = s_next_sid++;
        if (cand == 0) {
            continue; /* 0 is reserved for "no session" */
        }
        bool taken = false;
        for (int i = 0; i < TNFSD_MAX_SESSIONS; ++i) {
            if (s_sessions[i].in_use && s_sessions[i].sid == cand) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            return cand;
        }
    }
}

/*
 * Resolve a session-relative TNFS path to an absolute /sd path, jailed to the SD root.
 * Strips the leading '/' (TNFS paths are absolute from the mount root), prefixes the
 * session's mount subdir, trims trailing '/', then runs it through sd_path() which
 * rejects '..' and escapes. Returns TNFS_OK or a TNFS error status.
 */
static uint8_t resolve_path(const session_t *s, const char *req, char *abs, size_t cap)
{
    while (*req == '/') {
        req++;
    }
    char rel[TNFSD_ABS_CAP];
    if (s->root[0]) {
        if (*req) {
            if (snprintf(rel, sizeof rel, "%s/%s", s->root, req) >= (int)sizeof rel) {
                return TNFS_ENAMETOOLONG;
            }
        } else {
            strlcpy(rel, s->root, sizeof rel);
        }
    } else {
        strlcpy(rel, req, sizeof rel);
    }
    size_t l = strlen(rel);
    while (l > 0 && rel[l - 1] == '/') {
        rel[--l] = '\0';
    }
    if (l == 0) {
        strlcpy(abs, SD_MOUNT_POINT, cap); /* the SD root itself */
        return TNFS_OK;
    }
    if (sd_path(rel, abs, cap) < 0) {
        return TNFS_EINVAL; /* empty component, too long, or '..' */
    }
    return TNFS_OK;
}

/* Build the reply into s->reply (header + body), cache it for replay, and send it. */
static void reply_and_cache(session_t *s, uint8_t seq, uint8_t cmd,
                            const uint8_t *body, size_t blen)
{
    if (TNFS_HDR_LEN + blen > sizeof s->reply) {
        blen = sizeof s->reply - TNFS_HDR_LEN; /* defensive; handlers stay within */
    }
    uint8_t *r = s->reply;
    tnfs_wr16(r, s->sid);
    r[2] = seq;
    r[3] = cmd;
    memcpy(r + TNFS_HDR_LEN, body, blen);
    s->reply_len = TNFS_HDR_LEN + blen;
    s->last_seq = seq;
    s->last_cmd = cmd;
    s->have_reply = true;
    sendto(s_sock, s->reply, s->reply_len, 0,
           (struct sockaddr *)&s->peer, sizeof s->peer);
}

/* ---- directory iteration (READDIRX) ---------------------------------------- */

static void dir_stash(dirslot_t *d, uint8_t flags, uint32_t size,
                      uint32_t mtime, uint32_t ctime, const char *name)
{
    d->has_pending = true;
    d->p_flags = flags;
    d->p_size = size;
    d->p_mtime = mtime;
    d->p_ctime = ctime;
    strlcpy(d->p_name, name, sizeof d->p_name);
}

/* Next visible entry (skips dotfiles + non-matching), with its metadata. False at EOF. */
static bool dir_next(dirslot_t *d, uint8_t *flags, uint32_t *size,
                     uint32_t *mtime, uint32_t *ctime, char *name, size_t name_cap)
{
    if (d->has_pending) {
        d->has_pending = false;
        *flags = d->p_flags;
        *size = d->p_size;
        *mtime = d->p_mtime;
        *ctime = d->p_ctime;
        strlcpy(name, d->p_name, name_cap);
        return true;
    }
    struct dirent *de;
    while ((de = readdir(d->dir)) != NULL) {
        if (de->d_name[0] == '.') {
            continue; /* hide dotfiles, "." and ".." (matches the `dir` command) */
        }
        if (d->pattern[0] && !wildcard_match_ci(d->pattern, de->d_name)) {
            continue;
        }
        uint8_t fl = 0;
        uint32_t sz = 0, mt = 0, ct = 0;
        char full[TNFSD_ABS_CAP];
        if (snprintf(full, sizeof full, "%s/%s", d->abspath, de->d_name) < (int)sizeof full) {
            struct stat st;
            if (stat(full, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    fl |= TNFS_DIRENTRY_DIR;
                }
                sz = (uint32_t)st.st_size;
                mt = (uint32_t)st.st_mtime;
                ct = (uint32_t)st.st_mtime;
            } else if (de->d_type == DT_DIR) {
                fl |= TNFS_DIRENTRY_DIR;
            }
        }
        *flags = fl;
        *size = sz;
        *mtime = mt;
        *ctime = ct;
        strlcpy(name, de->d_name, name_cap);
        return true;
    }
    return false;
}

/* ---- command handlers (fill body starting with a status byte; return body len) */

static size_t h_open(session_t *s, const uint8_t *pl, size_t pn, uint8_t *body)
{
    if (pn < 5) { /* flags(2) + mode(2) + at least a 1-char path+NUL */
        body[0] = TNFS_EINVAL;
        return 1;
    }
    const char *path = (const char *)(pl + 4);
    if (!memchr(path, '\0', pn - 4)) {
        body[0] = TNFS_EINVAL;
        return 1;
    }
    char abs[TNFSD_ABS_CAP];
    uint8_t st = resolve_path(s, path, abs, sizeof abs);
    if (st != TNFS_OK) {
        body[0] = st;
        return 1;
    }
    int slot = -1;
    for (int i = 0; i < TNFSD_MAX_FILES; ++i) {
        if (s->files[i] < 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        body[0] = TNFS_EMFILE;
        return 1;
    }
    uint16_t tf = tnfs_rd16(pl);
    int of;
    switch (tf & 0x0003) {
    case TNFS_O_WRONLY: of = O_WRONLY; break;
    case TNFS_O_RDWR:   of = O_RDWR;   break;
    default:            of = O_RDONLY; break;
    }
    if (tf & TNFS_O_APPEND) of |= O_APPEND;
    if (tf & TNFS_O_CREAT)  of |= O_CREAT;
    if (tf & TNFS_O_TRUNC)  of |= O_TRUNC;
    if (tf & TNFS_O_EXCL)   of |= O_EXCL;

    int fd = open(abs, of, 0666);
    if (fd < 0) {
        body[0] = errno_to_tnfs(errno);
        return 1;
    }
    s->files[slot] = fd;
    body[0] = TNFS_OK;
    body[1] = (uint8_t)slot;
    return 2;
}

static size_t h_read(session_t *s, const uint8_t *pl, size_t pn, uint8_t *body)
{
    if (pn < 3) {
        body[0] = TNFS_EINVAL;
        return 1;
    }
    uint8_t fd = pl[0];
    uint16_t want = tnfs_rd16(&pl[1]);
    if (fd >= TNFSD_MAX_FILES || s->files[fd] < 0) {
        body[0] = TNFS_EBADF;
        return 1;
    }
    if (want > TNFS_IO_CHUNK) {
        want = TNFS_IO_CHUNK;
    }
    int r = read(s->files[fd], body + 3, want);
    if (r < 0) {
        body[0] = errno_to_tnfs(errno);
        return 1;
    }
    if (r == 0 && want > 0) {
        body[0] = TNFS_EOF;
        return 1;
    }
    body[0] = TNFS_OK;
    tnfs_wr16(&body[1], (uint16_t)r);
    return 3 + (size_t)r;
}

static size_t h_write(session_t *s, const uint8_t *pl, size_t pn, uint8_t *body)
{
    if (pn < 3) {
        body[0] = TNFS_EINVAL;
        return 1;
    }
    uint8_t fd = pl[0];
    uint16_t cnt = tnfs_rd16(&pl[1]);
    if (pn < (size_t)3 + cnt) {
        body[0] = TNFS_EINVAL; /* truncated datagram */
        return 1;
    }
    if (fd >= TNFSD_MAX_FILES || s->files[fd] < 0) {
        body[0] = TNFS_EBADF;
        return 1;
    }
    int w = write(s->files[fd], pl + 3, cnt);
    if (w < 0) {
        body[0] = errno_to_tnfs(errno);
        return 1;
    }
    body[0] = TNFS_OK;
    tnfs_wr16(&body[1], (uint16_t)w);
    return 3;
}

static size_t h_close(session_t *s, const uint8_t *pl, size_t pn, uint8_t *body)
{
    if (pn < 1) {
        body[0] = TNFS_EINVAL;
        return 1;
    }
    uint8_t fd = pl[0];
    if (fd >= TNFSD_MAX_FILES || s->files[fd] < 0) {
        body[0] = TNFS_EBADF;
        return 1;
    }
    close(s->files[fd]);
    s->files[fd] = -1;
    body[0] = TNFS_OK;
    return 1;
}

static size_t h_lseek(session_t *s, const uint8_t *pl, size_t pn, uint8_t *body)
{
    if (pn < 6) {
        body[0] = TNFS_EINVAL;
        return 1;
    }
    uint8_t fd = pl[0];
    uint8_t whence = pl[1];
    int32_t off = (int32_t)tnfs_rd32(&pl[2]);
    if (fd >= TNFSD_MAX_FILES || s->files[fd] < 0) {
        body[0] = TNFS_EBADF;
        return 1;
    }
    int w;
    switch (whence) {
    case TNFS_SEEK_SET: w = SEEK_SET; break;
    case TNFS_SEEK_CUR: w = SEEK_CUR; break;
    case TNFS_SEEK_END: w = SEEK_END; break;
    default:
        body[0] = TNFS_EINVAL;
        return 1;
    }
    off_t r = lseek(s->files[fd], off, w);
    if (r < 0) {
        body[0] = errno_to_tnfs(errno);
        return 1;
    }
    body[0] = TNFS_OK;
    tnfs_wr32(&body[1], (uint32_t)r); /* resulting position (client reads this as size) */
    return 5;
}

static size_t h_stat(session_t *s, const uint8_t *pl, size_t pn, uint8_t *body)
{
    if (pn < 1 || !memchr(pl, '\0', pn)) {
        body[0] = TNFS_EINVAL;
        return 1;
    }
    char abs[TNFSD_ABS_CAP];
    uint8_t st = resolve_path(s, (const char *)pl, abs, sizeof abs);
    if (st != TNFS_OK) {
        body[0] = st;
        return 1;
    }
    struct stat sb;
    if (stat(abs, &sb) != 0) {
        body[0] = errno_to_tnfs(errno);
        return 1;
    }
    body[0] = TNFS_OK;
    tnfs_wr16(&body[1], (uint16_t)sb.st_mode);   /* mode (S_IFDIR/S_IFREG + perms) */
    tnfs_wr16(&body[3], 0);                       /* uid */
    tnfs_wr16(&body[5], 0);                       /* gid */
    tnfs_wr32(&body[7], (uint32_t)sb.st_size);
    tnfs_wr32(&body[11], (uint32_t)sb.st_mtime);  /* atime (FAT has no atime) */
    tnfs_wr32(&body[15], (uint32_t)sb.st_mtime);
    tnfs_wr32(&body[19], (uint32_t)sb.st_mtime);  /* ctime */
    return 23;
}

/* Shared by MKDIR/RMDIR/UNLINK: resolve one path arg and run `op`. */
static size_t h_pathop(session_t *s, const uint8_t *pl, size_t pn, uint8_t *body,
                       int (*op)(const char *))
{
    if (pn < 1 || !memchr(pl, '\0', pn)) {
        body[0] = TNFS_EINVAL;
        return 1;
    }
    char abs[TNFSD_ABS_CAP];
    uint8_t st = resolve_path(s, (const char *)pl, abs, sizeof abs);
    if (st != TNFS_OK) {
        body[0] = st;
        return 1;
    }
    body[0] = (op(abs) == 0) ? TNFS_OK : errno_to_tnfs(errno);
    return 1;
}

static int mkdir_op(const char *p) { return mkdir(p, 0777); }

static size_t h_rename(session_t *s, const uint8_t *pl, size_t pn, uint8_t *body)
{
    if (pn < 2) {
        body[0] = TNFS_EINVAL;
        return 1;
    }
    const void *z = memchr(pl, '\0', pn);
    if (!z) {
        body[0] = TNFS_EINVAL;
        return 1;
    }
    const char *oldp = (const char *)pl;
    size_t oldlen = (const uint8_t *)z - pl; /* excludes the NUL */
    const char *newp = (const char *)z + 1;
    size_t rest = pn - (oldlen + 1);
    if (rest == 0 || !memchr(newp, '\0', rest)) {
        body[0] = TNFS_EINVAL;
        return 1;
    }
    char oabs[TNFSD_ABS_CAP], nabs[TNFSD_ABS_CAP];
    uint8_t st = resolve_path(s, oldp, oabs, sizeof oabs);
    if (st != TNFS_OK) {
        body[0] = st;
        return 1;
    }
    st = resolve_path(s, newp, nabs, sizeof nabs);
    if (st != TNFS_OK) {
        body[0] = st;
        return 1;
    }
    body[0] = (rename(oabs, nabs) == 0) ? TNFS_OK : errno_to_tnfs(errno);
    return 1;
}

static size_t h_opendir(session_t *s, const uint8_t *pl, size_t pn, uint8_t *body)
{
    if (pn < 1 || !memchr(pl, '\0', pn)) {
        body[0] = TNFS_EINVAL;
        return 1;
    }
    char abs[TNFSD_ABS_CAP];
    uint8_t st = resolve_path(s, (const char *)pl, abs, sizeof abs);
    if (st != TNFS_OK) {
        body[0] = st;
        return 1;
    }
    int slot = -1;
    for (int i = 0; i < TNFSD_MAX_DIRS; ++i) {
        if (!s->dirs[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        body[0] = TNFS_EMFILE;
        return 1;
    }
    DIR *d = opendir(abs);
    if (!d) {
        body[0] = errno_to_tnfs(errno);
        return 1;
    }
    dirslot_t *ds = &s->dirs[slot];
    memset(ds, 0, sizeof *ds);
    ds->in_use = true;
    ds->dir = d;
    strlcpy(ds->abspath, abs, sizeof ds->abspath);
    body[0] = TNFS_OK;
    body[1] = (uint8_t)slot;
    return 2;
}

static size_t h_opendirx(session_t *s, const uint8_t *pl, size_t pn, uint8_t *body)
{
    /* diropt(1) + sortopt(1) + maxresults(2) + pattern(NUL) + path(NUL). */
    if (pn < 5) {
        body[0] = TNFS_EINVAL;
        return 1;
    }
    const char *pat = (const char *)(pl + 4);
    const void *z = memchr(pat, '\0', pn - 4);
    if (!z) {
        body[0] = TNFS_EINVAL;
        return 1;
    }
    size_t patlen = (const char *)z - pat;
    const char *path = (const char *)z + 1;
    size_t rest = pn - 4 - (patlen + 1);
    if (rest == 0 || !memchr(path, '\0', rest)) {
        body[0] = TNFS_EINVAL;
        return 1;
    }
    char abs[TNFSD_ABS_CAP];
    uint8_t st = resolve_path(s, path, abs, sizeof abs);
    if (st != TNFS_OK) {
        body[0] = st;
        return 1;
    }
    int slot = -1;
    for (int i = 0; i < TNFSD_MAX_DIRS; ++i) {
        if (!s->dirs[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        body[0] = TNFS_EMFILE;
        return 1;
    }
    DIR *d = opendir(abs);
    if (!d) {
        body[0] = errno_to_tnfs(errno);
        return 1;
    }
    dirslot_t *ds = &s->dirs[slot];
    memset(ds, 0, sizeof *ds);
    ds->in_use = true;
    ds->dir = d;
    strlcpy(ds->abspath, abs, sizeof ds->abspath);
    if (patlen > 0 && !(patlen == 1 && pat[0] == '*')) {
        strlcpy(ds->pattern, pat, sizeof ds->pattern); /* "" / "*" mean match all */
    }

    /* Pre-count matching entries for the reply, then rewind for streaming. */
    uint16_t count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        if (ds->pattern[0] && !wildcard_match_ci(ds->pattern, de->d_name)) {
            continue;
        }
        count++;
    }
    rewinddir(d);

    body[0] = TNFS_OK;
    body[1] = (uint8_t)slot;
    tnfs_wr16(&body[2], count);
    return 4;
}

static size_t h_readdir(session_t *s, const uint8_t *pl, size_t pn, uint8_t *body)
{
    if (pn < 1) {
        body[0] = TNFS_EINVAL;
        return 1;
    }
    uint8_t h = pl[0];
    if (h >= TNFSD_MAX_DIRS || !s->dirs[h].in_use) {
        body[0] = TNFS_EBADF;
        return 1;
    }
    dirslot_t *d = &s->dirs[h];
    uint8_t fl;
    uint32_t sz, mt, ct;
    char nm[TNFSD_NAME_CAP];
    if (!dir_next(d, &fl, &sz, &mt, &ct, nm, sizeof nm)) {
        body[0] = TNFS_EOF;
        return 1;
    }
    size_t nl = strlen(nm);
    if (1 + nl + 1 > TNFS_MSG_CAP - TNFS_HDR_LEN) {
        nl = TNFS_MSG_CAP - TNFS_HDR_LEN - 2; /* clamp pathological name */
    }
    body[0] = TNFS_OK;
    memcpy(body + 1, nm, nl);
    body[1 + nl] = '\0';
    return 2 + nl;
}

static size_t h_readdirx(session_t *s, const uint8_t *pl, size_t pn, uint8_t *body)
{
    if (pn < 2) { /* handle(1) + numwanted(1) */
        body[0] = TNFS_EINVAL;
        return 1;
    }
    uint8_t h = pl[0];
    uint8_t numwanted = pl[1];
    if (h >= TNFSD_MAX_DIRS || !s->dirs[h].in_use) {
        body[0] = TNFS_EBADF;
        return 1;
    }
    if (numwanted == 0) {
        numwanted = 1;
    }
    dirslot_t *d = &s->dirs[h];

    /* body: status(1) count(1) dirstatus(1) dirpos(2), then entries from offset 5. */
    const size_t body_cap = TNFS_MSG_CAP - TNFS_HDR_LEN;
    size_t idx = 5;
    uint8_t count = 0;
    uint8_t dstatus = 0;
    while (count < numwanted) {
        uint8_t fl;
        uint32_t sz, mt, ct;
        char nm[TNFSD_NAME_CAP];
        if (!dir_next(d, &fl, &sz, &mt, &ct, nm, sizeof nm)) {
            dstatus |= TNFS_DIRSTATUS_EOF;
            break;
        }
        size_t entry = TNFS_READDIRX_FIXED + strlen(nm) + 1;
        if (idx + entry > body_cap) {
            dir_stash(d, fl, sz, mt, ct, nm); /* emit it next time */
            break;
        }
        body[idx] = fl;
        tnfs_wr32(&body[idx + 1], sz);
        tnfs_wr32(&body[idx + 5], mt);
        tnfs_wr32(&body[idx + 9], ct);
        size_t nl = strlen(nm);
        memcpy(&body[idx + TNFS_READDIRX_FIXED], nm, nl);
        body[idx + TNFS_READDIRX_FIXED + nl] = '\0';
        idx += entry;
        count++;
    }
    if (count == 0) {
        body[0] = TNFS_EOF; /* nothing left to return */
        return 1;
    }
    body[0] = TNFS_OK;
    body[1] = count;
    body[2] = dstatus;
    tnfs_wr16(&body[3], d->pos);
    d->pos += count;
    return idx;
}

static size_t h_closedir(session_t *s, const uint8_t *pl, size_t pn, uint8_t *body)
{
    if (pn < 1) {
        body[0] = TNFS_EINVAL;
        return 1;
    }
    uint8_t h = pl[0];
    if (h >= TNFSD_MAX_DIRS || !s->dirs[h].in_use) {
        body[0] = TNFS_EBADF;
        return 1;
    }
    closedir(s->dirs[h].dir);
    memset(&s->dirs[h], 0, sizeof s->dirs[h]);
    body[0] = TNFS_OK;
    return 1;
}

/* ---- dispatch --------------------------------------------------------------- */

static void process(session_t *s, uint8_t seq, uint8_t cmd, const uint8_t *rx, size_t n)
{
    const uint8_t *pl = rx + TNFS_HDR_LEN;
    size_t pn = (n > TNFS_HDR_LEN) ? n - TNFS_HDR_LEN : 0;
    uint8_t body[TNFS_MSG_CAP];
    size_t blen;
    bool umount = false;

    switch (cmd) {
    case TNFS_UMOUNT:   body[0] = TNFS_OK; blen = 1; umount = true; break;
    case TNFS_OPEN:     blen = h_open(s, pl, pn, body);     break;
    case TNFS_READ:     blen = h_read(s, pl, pn, body);     break;
    case TNFS_WRITE:    blen = h_write(s, pl, pn, body);    break;
    case TNFS_CLOSE:    blen = h_close(s, pl, pn, body);    break;
    case TNFS_LSEEK:    blen = h_lseek(s, pl, pn, body);    break;
    case TNFS_STAT:     blen = h_stat(s, pl, pn, body);     break;
    case TNFS_MKDIR:    blen = h_pathop(s, pl, pn, body, mkdir_op); break;
    case TNFS_RMDIR:    blen = h_pathop(s, pl, pn, body, rmdir);    break;
    case TNFS_UNLINK:   blen = h_pathop(s, pl, pn, body, unlink);   break;
    case TNFS_RENAME:   blen = h_rename(s, pl, pn, body);   break;
    case TNFS_OPENDIR:  blen = h_opendir(s, pl, pn, body);  break;
    case TNFS_OPENDIRX: blen = h_opendirx(s, pl, pn, body); break;
    case TNFS_READDIR:  blen = h_readdir(s, pl, pn, body);  break;
    case TNFS_READDIRX: blen = h_readdirx(s, pl, pn, body); break;
    case TNFS_CLOSEDIR: blen = h_closedir(s, pl, pn, body); break;
    case TNFS_CHMOD:    body[0] = TNFS_OK; blen = 1;        break; /* no-op on FAT */
    default:            body[0] = TNFS_ENOSYS; blen = 1;    break;
    }

    reply_and_cache(s, seq, cmd, body, blen);
    s->last_active_us = now_us();
    if (umount) {
        free_session(s);
    }
}

/* MOUNT is special: the request has no session id (we allocate one in the reply). */
static void handle_mount(uint8_t seq, const uint8_t *rx, size_t n,
                         const struct sockaddr_in *peer)
{
    /* Retransmit of a MOUNT whose reply was lost: replay the cached one. */
    for (int i = 0; i < TNFSD_MAX_SESSIONS; ++i) {
        session_t *e = &s_sessions[i];
        if (e->in_use && same_peer(&e->peer, peer) && e->have_reply &&
            e->last_cmd == TNFS_MOUNT && e->last_seq == seq) {
            sendto(s_sock, e->reply, e->reply_len, 0,
                   (struct sockaddr *)&e->peer, sizeof e->peer);
            e->last_active_us = now_us();
            return;
        }
    }

    session_t *s = alloc_session();
    if (!s) {
        /* Out of session slots: ask the client to retry later (header session 0). */
        uint8_t r[7];
        tnfs_wr16(r, 0);
        r[2] = seq;
        r[3] = TNFS_MOUNT;
        r[4] = TNFS_EAGAIN;
        tnfs_wr16(&r[5], TNFSD_MIN_RETRY_MS);
        sendto(s_sock, r, sizeof r, 0, (struct sockaddr *)peer, sizeof *peer);
        ESP_LOGW(TAG, "MOUNT refused: no free session slot");
        return;
    }

    /* Payload: version(2) + mountpoint(NUL) + user(NUL) + password(NUL). We ignore the
     * credentials (§9.6) and only take the mountpoint, jailed under /sd. */
    const uint8_t *pl = rx + TNFS_HDR_LEN;
    size_t pn = (n > TNFS_HDR_LEN) ? n - TNFS_HDR_LEN : 0;
    const char *mp = "";
    if (pn > 2 && memchr(pl + 2, '\0', pn - 2)) {
        mp = (const char *)(pl + 2);
    }

    /* Resolve the mountpoint to a root-relative subdir under /sd. */
    while (*mp == '/') {
        mp++;
    }
    char rootrel[TNFSD_ROOT_CAP];
    strlcpy(rootrel, mp, sizeof rootrel);
    size_t rl = strlen(rootrel);
    while (rl > 0 && rootrel[rl - 1] == '/') {
        rootrel[--rl] = '\0';
    }
    if (rl > 0) {
        char tmp[TNFSD_ABS_CAP];
        if (sd_path(rootrel, tmp, sizeof tmp) < 0) { /* reject '..' / bad mountpoint */
            s->sid = next_sid();                     /* need a sid to address the reply */
            uint8_t body = TNFS_ENOENT;
            reply_and_cache(s, seq, TNFS_MOUNT, &body, 1);
            free_session(s);
            return;
        }
    }

    s->sid = next_sid();
    s->peer = *peer;
    strlcpy(s->root, rootrel, sizeof s->root);
    s->last_active_us = now_us();

    uint8_t body[5];
    body[0] = TNFS_OK;
    body[1] = TNFS_VER_LO;
    body[2] = TNFS_VER_HI;
    tnfs_wr16(&body[3], TNFSD_MIN_RETRY_MS);
    reply_and_cache(s, seq, TNFS_MOUNT, body, 5);
    ESP_LOGI(TAG, "session %u mounted /%s", s->sid, s->root);
}

/* ---- serve loop ------------------------------------------------------------- */

static void serve(void)
{
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(TNFS_DEFAULT_PORT),
    };
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        ESP_LOGE(TAG, "bind :%d failed: errno %d", TNFS_DEFAULT_PORT, errno);
        close(s_sock);
        s_sock = -1;
        return;
    }
    struct timeval tv = { .tv_sec = TNFSD_RECV_MS / 1000,
                          .tv_usec = (TNFSD_RECV_MS % 1000) * 1000 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ESP_LOGI(TAG, "TNFS server ready on UDP :%d (root %s)",
             TNFS_DEFAULT_PORT, SD_MOUNT_POINT);

    uint8_t rx[TNFS_MSG_CAP];
    while (net_is_connected()) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int n = recvfrom(s_sock, rx, sizeof rx, 0, (struct sockaddr *)&peer, &plen);
        reap_idle();
        if (n < TNFS_HDR_LEN) {
            continue; /* timeout (-1) or a runt */
        }
        uint16_t sid = tnfs_rd16(rx);
        uint8_t seq = rx[2];
        uint8_t cmd = rx[3];

        if (cmd == TNFS_MOUNT) {
            handle_mount(seq, rx, (size_t)n, &peer);
            continue;
        }
        session_t *s = find_session(sid, &peer);
        if (!s) {
            continue; /* unknown/expired session — ignore (client will re-MOUNT) */
        }
        if (s->have_reply && s->last_seq == seq && s->last_cmd == cmd) {
            /* Duplicate request (lost reply): replay verbatim, do NOT re-run it. */
            sendto(s_sock, s->reply, s->reply_len, 0,
                   (struct sockaddr *)&s->peer, sizeof s->peer);
            s->last_active_us = now_us();
            continue;
        }
        process(s, seq, cmd, rx, (size_t)n);
    }

    ESP_LOGW(TAG, "link down; stopping TNFS server");
    close(s_sock);
    s_sock = -1;
    drop_all_sessions();
}

static void tnfsd_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < TNFSD_MAX_SESSIONS; ++i) {
        free_session(&s_sessions[i]); /* initialize fds to -1 */
    }
    for (;;) {
        net_wait_connected(0); /* block until the link is up */
        serve();
    }
}

esp_err_t net_tnfsd_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(tnfsd_task, "tnfsd", 8192, NULL, 5, NULL, 0);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
