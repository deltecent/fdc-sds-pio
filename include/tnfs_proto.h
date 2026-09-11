/*
 * tnfs_proto.h — TNFS on-the-wire constants shared by the client and the server.
 *
 * The TNFS client (tnfs.c, §9.5) and the TNFS server (tnfs_server.c, §9.6) play
 * opposite roles but speak the same protocol, so the framing, command opcodes, OPEN
 * flags, and status codes live here and nowhere else. This header is wire contract
 * only — no behavior, no timing (per-attempt timeouts and retry counts stay private
 * to each side, since a client retransmits and a server does not).
 *
 * Every message is:  session(2, LE) | seq(1) | cmd(1) | payload…
 * Every response echoes that 4-byte header and carries a status byte at offset 4
 * (0 = success). Reference: the FujiNet TNFS protocol document.
 */
#ifndef FDCSDS_TNFS_PROTO_H
#define FDCSDS_TNFS_PROTO_H

#include <stdint.h>

/* Default UDP port (DESIGN.md §9.5/§9.6). */
#define TNFS_DEFAULT_PORT 16384

/* ---- framing --------------------------------------------------------------- */

#define TNFS_HDR_LEN     4     /* session(2) + seq(1) + cmd(1) */
#define TNFS_STATUS_OFF  4     /* status byte in every response */
#define TNFS_IO_CHUNK    1024  /* max data bytes per READ/WRITE (fits one UDP datagram) */
#define TNFS_MSG_CAP     (TNFS_HDR_LEN + 4 + TNFS_IO_CHUNK) /* biggest message either side handles */

/* ---- commands -------------------------------------------------------------- */

#define TNFS_MOUNT    0x00
#define TNFS_UMOUNT   0x01
#define TNFS_OPENDIR  0x10
#define TNFS_READDIR  0x11
#define TNFS_CLOSEDIR 0x12
#define TNFS_MKDIR    0x13
#define TNFS_RMDIR    0x14
#define TNFS_OPENDIRX 0x17
#define TNFS_READDIRX 0x18
#define TNFS_READ     0x21
#define TNFS_WRITE    0x22
#define TNFS_CLOSE    0x23
#define TNFS_STAT     0x24
#define TNFS_LSEEK    0x25
#define TNFS_UNLINK   0x26
#define TNFS_CHMOD    0x27
#define TNFS_RENAME   0x28
#define TNFS_OPEN     0x29

/* ---- OPEN flags (little-endian bit field) ---------------------------------- */

#define TNFS_O_RDONLY 0x0001
#define TNFS_O_WRONLY 0x0002
#define TNFS_O_RDWR   0x0003
#define TNFS_O_APPEND 0x0008
#define TNFS_O_CREAT  0x0100
#define TNFS_O_TRUNC  0x0200
#define TNFS_O_EXCL   0x0400

/* ---- status codes (the TNFS errno table, a subset) ------------------------- */

#define TNFS_OK           0x00
#define TNFS_EPERM        0x01  /* operation not permitted */
#define TNFS_ENOENT       0x02  /* no such file or directory */
#define TNFS_EIO          0x03  /* I/O error */
#define TNFS_EBADF        0x06  /* bad file descriptor */
#define TNFS_EAGAIN       0x07  /* try again; backoff (ms, LE) follows the status byte */
#define TNFS_ENOMEM       0x08  /* out of memory */
#define TNFS_EACCES       0x09  /* permission denied */
#define TNFS_EBUSY        0x0A  /* device or resource busy */
#define TNFS_EEXIST       0x0B  /* file exists */
#define TNFS_ENOTDIR      0x0C  /* not a directory */
#define TNFS_EISDIR       0x0D  /* is a directory */
#define TNFS_EINVAL       0x0E  /* invalid argument */
#define TNFS_EMFILE       0x10  /* too many open files */
#define TNFS_EFBIG        0x11  /* file too large */
#define TNFS_ENOSPC       0x12  /* no space left on device */
#define TNFS_EROFS        0x14  /* read-only filesystem */
#define TNFS_ENAMETOOLONG 0x15  /* filename too long */
#define TNFS_ENOSYS       0x16  /* function not implemented */
#define TNFS_ENOTEMPTY    0x17  /* directory not empty */
#define TNFS_EOF          0x21  /* end of file (READ past the end) */

/* ---- LSEEK whence ---------------------------------------------------------- */

#define TNFS_SEEK_SET 0x00
#define TNFS_SEEK_CUR 0x01
#define TNFS_SEEK_END 0x02

/* ---- directory entry (READDIRX) -------------------------------------------- */

#define TNFS_DIRENTRY_DIR   0x01  /* dirent flag: entry is a directory */
#define TNFS_DIRSTATUS_EOF  0x01  /* reply dir-status flag: end of directory reached */
#define TNFS_READDIRX_FIXED 13    /* dirent bytes before the name: flags(1)+size(4)+mtime(4)+ctime(4) */

/* Protocol version advertised in MOUNT: 1.2, bytes {minor, major} = {0x02, 0x01}. */
#define TNFS_VER_LO 0x02
#define TNFS_VER_HI 0x01

/* ---- little-endian accessors (header-only, used by both sides) ------------- */

static inline uint16_t tnfs_rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t tnfs_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void tnfs_wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static inline void tnfs_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

#endif /* FDCSDS_TNFS_PROTO_H */
