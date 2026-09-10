/*
 * fdc.h — FDC+ serial protocol engine (DESIGN.md §6, plan M4).
 *
 * Owns UART2 (the only task that touches it) and answers the FDC's STAT/READ/WRIT
 * transactions: 10-byte command blocks in, whole-track data over the shared `disk`
 * module out. Runs as a high-priority FreeRTOS task pinned to core 1 so WiFi/CLI work
 * on core 0 can never stall the ~1 s FDC command timeout (DESIGN.md §5.2).
 *
 * The wire contract is entirely in protocol.h (block layout, checksums, drive/track
 * decode) so the host simulator (tools/fdc-sim) and the native unit tests share it.
 */
#ifndef FDCSDS_FDC_H
#define FDCSDS_FDC_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Running counters + last-transaction string, shown by the `stats` command.
 *
 * The server is a passive responder: it can only observe corruption in bytes the FDC
 * sends *to* it, so the checksum/timeout counters name where in the transaction the
 * fault occurred (DESIGN.md §6.4). A READ whose data the FDC rejects (bad CRC on its
 * side) produces no error packet here — the FDC silently re-reads — so `read_retry`
 * (a repeated READ of the same drive/track) is the only visibility into that case.
 */
typedef struct {
    uint32_t stat;         /* STAT commands answered */
    uint32_t read;         /* READ commands that returned track data */
    uint32_t writ;         /* WRIT commands accepted (data received, WSTA sent) */
    uint32_t not_ready;    /* READ/WRIT to an unmounted / out-of-range drive, or write error */
    uint32_t cmd_csum;     /* bad checksum on a 10-byte command block (header from FDC) */
    uint32_t data_csum;    /* bad checksum on WRIT track data (payload from FDC) -> WSTA 2 */
    uint32_t cmd_timeout;  /* command block truncated before the 10 bytes arrived */
    uint32_t data_timeout; /* WRIT track data / its checksum did not fully arrive */
    uint32_t read_retry;   /* READ repeating the previous drive/track (FDC re-reading) */
    uint32_t fifo_ovf;     /* UART RX FIFO / ring overflow: bytes lost (ISR starved) */
    uint32_t frame_err;    /* UART framing error: bits mis-sampled (baud / electrical) */
    uint32_t unknown;      /* unrecognized 4-byte command mnemonics */
    char     last_op[40];
} fdc_stats_t;

/* Result of the loopback self-test (DESIGN.md §6.8). */
typedef struct {
    bool     ran;         /* the fdc task performed the test */
    uint32_t sent;        /* pattern bytes transmitted */
    uint32_t received;    /* bytes read back before timeout */
    uint32_t mismatches;  /* byte positions that differed (missing bytes count too) */
    uint32_t first_bad;   /* index of the first mismatch (== sent if all matched) */
} fdc_loopback_t;

/*
 * Install UART2 at the configured baud (DESIGN.md §6.1) and start the fdc task
 * (high priority, core 1). Call once at boot after disk_init()/config_init(). Safe to
 * call before the SD or any drive is mounted — STAT still answers, READ/WRIT report
 * not-ready until a drive is mounted.
 */
esp_err_t fdc_init(void);

/* Apply a new FDC+ link baud rate live (re-programs the UART divisor + flushes RX). */
esp_err_t fdc_set_baud(uint32_t baud);

/* The baud currently programmed on the FDC+ link. */
uint32_t fdc_baud(void);

/* Snapshot the counters. */
void fdc_get_stats(fdc_stats_t *out);

/* Zero the counters and clear the last-op string. */
void fdc_clear_stats(void);

/*
 * Loopback self-test: with a TX↔RX jumper on the FDC+ port, transmit a 256-byte
 * incrementing pattern and read it back, filling *out. Returns ESP_ERR_INVALID_STATE
 * if the engine isn't running, ESP_ERR_TIMEOUT if the task didn't service the request.
 */
esp_err_t fdc_loopback(fdc_loopback_t *out);

#ifdef __cplusplus
}
#endif

#endif /* FDCSDS_FDC_H */
