/*
 * protocol.h — shared FDC+ protocol + disk-geometry definitions (DESIGN.md §6/§10).
 *
 * This header is deliberately FREE of ESP-IDF dependencies: it holds only constants
 * and pure `static inline` helpers (checksums, track offset/geometry, the STAT mount
 * bitmap, READ/WRIT drive+track decode). That keeps the tricky arithmetic — the stuff
 * the old firmware got wrong with signed `int` (DESIGN.md §6.5/§15) — unit-testable on
 * the host via `pio test -e native`, with no hardware and no framework.
 *
 * The stateful disk module (mount table, handles, cache) lives in disk.h/disk.c; the
 * FDC engine that uses the block/response constants lands at M4.
 */
#ifndef FDCSDS_PROTOCOL_H
#define FDCSDS_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Command/response block layout (DESIGN.md §6.2): 4-byte cmd + 3 LE words. */
#define FDC_BLOCK_LEN     10
#define FDC_CMD_LEN       4
#define FDC_CHECKSUM_LEN  2

/* Response codes (DESIGN.md §6.3). */
#define FDC_RESP_OK          0
#define FDC_RESP_NOT_READY   1
#define FDC_RESP_CSUM_ERR    2
#define FDC_RESP_WRITE_ERR   3

/*
 * Track-buffer size = the largest supported track (DESIGN.md §6.5/§6.6): the 8 MB
 * CP/M image is 1096 tracks × 8192 bytes, so a track is at most 8192 bytes. READ/WRIT
 * must reject a transfer length larger than this.
 */
#define DISK_TRACK_BUF_SIZE 8192

/*
 * 16-bit checksum = plain sum of the given bytes, taken little-endian on the wire
 * (DESIGN.md §6.2). Used for both the 8-byte command block and the track payload.
 */
static inline uint16_t fdc_checksum16(const uint8_t *data, size_t n)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < n; ++i) {
        sum = (uint16_t)(sum + data[i]);
    }
    return sum;
}

/*
 * READ/WRIT encode drive + track in Word1 (DESIGN.md §6.3): drive = high nibble of the
 * high byte, track = low 12 bits. With a 16-bit little-endian word that is simply the
 * top nibble and the bottom 12 bits.
 */
static inline uint8_t fdc_word1_drive(uint16_t word1)
{
    return (uint8_t)(word1 >> 12);
}

static inline uint16_t fdc_word1_track(uint16_t word1)
{
    return (uint16_t)(word1 & 0x0FFF);
}

/* Byte offset of a track in a flat, headerless image (DESIGN.md §6.5): track * len. */
static inline uint32_t disk_track_offset(uint32_t track, uint32_t track_len)
{
    return track * track_len;
}

/*
 * True if a whole track of `track_len` bytes at `track` fits inside `image_size`.
 * Guards the 32-bit multiply against overflow before comparing (the 8 MB image pushes
 * track*len toward 2^23; a bad geometry could push it past 2^32).
 */
static inline bool disk_track_in_bounds(uint32_t track, uint32_t track_len,
                                        uint32_t image_size)
{
    if (track_len == 0) {
        return false;
    }
    if (track > UINT32_MAX / track_len) {
        return false; /* offset would overflow */
    }
    uint32_t off = track * track_len;
    if (off > image_size) {
        return false;
    }
    return (image_size - off) >= track_len;
}

/* How many whole `track_len` tracks an image holds (informative; DESIGN.md §6.5). */
static inline uint32_t disk_track_count(uint32_t image_size, uint32_t track_len)
{
    return track_len ? image_size / track_len : 0;
}

/*
 * STAT mount bitmap (DESIGN.md §6.3): bit n set = drive n mounted. `mounted[i]` is the
 * per-drive state; only bits 0..count-1 (and at most 16) are considered.
 */
static inline uint16_t disk_status_bitmap_of(const bool *mounted, int count)
{
    uint16_t bits = 0;
    for (int i = 0; i < count && i < 16; ++i) {
        if (mounted[i]) {
            bits |= (uint16_t)(1u << i);
        }
    }
    return bits;
}

#endif /* FDCSDS_PROTOCOL_H */
