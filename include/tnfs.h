/*
 * tnfs.h — compact TNFS client (remote disk images over UDP, DESIGN.md §9.5).
 *
 * TNFS ("The Network File System", from the FujiNet project) serves files from a
 * remote server addressed by a tnfs://host[:port]/path URL. This is the transport
 * glue (§5.3): a thin API exposing exactly what the callers need — open a remote
 * file, learn its size, read/write at absolute offsets, close. Each open file owns
 * its own session (one MOUNT + one OPEN over one UDP socket); `copy` uses a
 * transient one (§10.2) and a remote-mounted drive keeps one for its lifetime
 * (§10.1). The wire protocol (little-endian, sequence-numbered datagrams with
 * timeout/retry) is entirely inside tnfs.c.
 *
 * TNFS needs the network, so callers must only use this while WiFi is up (§9.5);
 * on a WiFi drop the datagrams simply time out and operations fail cleanly.
 */
#ifndef FDCSDS_TNFS_H
#define FDCSDS_TNFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "tnfs_proto.h" /* shared wire constants incl. TNFS_DEFAULT_PORT (§9.5/§9.6) */

#ifdef __cplusplus
extern "C" {
#endif

/* An open remote file over its own dedicated session. Opaque; created by
 * tnfs_open(), released by tnfs_close(). */
typedef struct tnfs_file tnfs_file_t;

/* True if `s` begins with the tnfs:// scheme (case-insensitive). */
bool tnfs_is_url(const char *s);

/*
 * Split a tnfs://host[:port]/path URL into parts: `host` and `path` (the path keeps
 * its leading '/') are written into the caller buffers, and *port is the URL's port
 * or TNFS_DEFAULT_PORT. Returns ESP_ERR_INVALID_ARG on a malformed URL, a missing
 * path, or a buffer overflow.
 */
esp_err_t tnfs_url_parse(const char *url, char *host, size_t host_cap,
                         uint16_t *port, char *path, size_t path_cap);

/*
 * Open the file named by a tnfs://host[:port]/path URL: resolve the host, connect a
 * UDP socket, MOUNT "/", then OPEN the path. `writable` opens an existing file
 * read/write; `create` (which implies writable) opens write-only with create+truncate
 * for a fresh file. On success *out owns the session — release it with tnfs_close().
 * Returns ESP_ERR_INVALID_ARG (bad URL), ESP_ERR_NOT_FOUND (no such remote file),
 * ESP_ERR_TIMEOUT (server unreachable), or ESP_FAIL.
 */
esp_err_t tnfs_open(const char *url, bool writable, bool create, tnfs_file_t **out);

/* File size in bytes as of open (0 for a freshly created file). */
uint32_t tnfs_size(tnfs_file_t *f);

/*
 * Read/write exactly `len` bytes at absolute offset `off`, transparently split into
 * protocol-sized datagrams. Returns ESP_OK, or ESP_FAIL / ESP_ERR_TIMEOUT on a short
 * transfer or network error.
 */
esp_err_t tnfs_read_at(tnfs_file_t *f, uint32_t off, void *buf, size_t len);
esp_err_t tnfs_write_at(tnfs_file_t *f, uint32_t off, const void *buf, size_t len);

/* CLOSE the file, UMOUNT the session, close the socket and free `f`. NULL-safe. */
void tnfs_close(tnfs_file_t *f);

/*
 * Callback invoked once per directory entry during tnfs_list_dir. `is_dir` is true
 * when the entry is a subdirectory (known only over the extended READDIRX path; always
 * false on the basic-READDIR fallback).
 */
typedef void (*tnfs_dir_cb)(const char *name, bool is_dir, void *ctx);

/*
 * List the directory named by a tnfs://host[:port]/path/ URL over a transient session
 * (§10.2): MOUNT "/", then OPENDIRX/READDIRX (extended: carries a per-entry directory
 * flag) with a fallback to basic OPENDIR/READDIR (names only) on servers that don't
 * support the extended opcodes, then CLOSEDIR + UMOUNT. A bare tnfs://host with no path
 * lists the server root. `cb` is called for every raw entry (including "." / ".."); the
 * caller applies any filtering. Returns ESP_OK, ESP_ERR_INVALID_ARG (bad URL),
 * ESP_ERR_NOT_FOUND (no such directory), ESP_ERR_TIMEOUT (server unreachable), or
 * ESP_FAIL.
 */
esp_err_t tnfs_list_dir(const char *url, tnfs_dir_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* FDCSDS_TNFS_H */
