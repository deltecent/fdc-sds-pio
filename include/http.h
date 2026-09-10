/*
 * http.h — minimal streaming HTTP(S) GET client for `copy` sources (DESIGN.md §10.3).
 *
 * This is a read-only transport: `copy` may name an http:// or https:// URL as its
 * *source* (never its destination — HTTP has no portable upload verb). A GET is a
 * one-way body, so unlike the TNFS client (sized random access, tnfs.h) this streams
 * to EOF in bounded chunks and cannot seek. https:// verifies against the bundled CA
 * roots (esp_crt_bundle, nothing host-pinned) — the same trust store OTA uses (§11) —
 * and redirects are followed automatically. It reuses the esp_http_client transport
 * already linked for OTA, and runs inline on the 8 KB CLI task (§5.2 / §8.1).
 *
 * http_is_url() is a pure predicate (no ESP-IDF), so host-native unit tests can include
 * this header with FDCSDS_HTTP_NO_IDF defined to test it without the toolchain.
 */
#ifndef FDCSDS_HTTP_H
#define FDCSDS_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True if `s` begins with the http:// or https:// scheme (case-insensitive). A `copy`
 * source only — never a valid destination (§10.3). Pure/inline so it is identical in the
 * firmware and in host-native tests. */
static inline bool http_is_url(const char *s)
{
    return s && (strncasecmp(s, "http://", 7) == 0 ||
                 strncasecmp(s, "https://", 8) == 0);
}

/* The streaming GET below pulls in ESP-IDF types; native unit tests define
 * FDCSDS_HTTP_NO_IDF to include only the pure predicate above. */
#ifndef FDCSDS_HTTP_NO_IDF

#include <stdint.h>

#include "esp_err.h"

/*
 * Sink invoked once per received body chunk. Return 0 to continue the transfer or a
 * negative value to abort it (e.g. a destination write failed). `data`/`len` are valid
 * only for the duration of the call.
 */
typedef int (*http_sink_fn)(void *ctx, const void *data, size_t len);

/*
 * GET `url` over http/https and stream the response body to `sink` in bounded chunks.
 * Redirects are followed and https:// is verified against the bundled CA roots. Only a
 * final 2xx body is delivered to the sink. Requires WiFi to be up (caller checks).
 * `out_bytes` (optional) receives the number of bytes handed to the sink.
 *
 * Returns ESP_OK on a complete 2xx transfer; ESP_ERR_INVALID_RESPONSE for a non-2xx
 * final status; ESP_ERR_TIMEOUT / another transport esp_err_t on a connection failure;
 * or ESP_FAIL when the sink aborted (a destination-side error).
 */
esp_err_t http_get(const char *url, http_sink_fn sink, void *ctx, uint32_t *out_bytes);

#endif /* FDCSDS_HTTP_NO_IDF */

#ifdef __cplusplus
}
#endif

#endif /* FDCSDS_HTTP_H */
