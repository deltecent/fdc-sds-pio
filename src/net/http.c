/*
 * http.c — streaming HTTP(S) GET for `copy` sources (DESIGN.md §10.3).
 *
 * A thin wrapper over esp_http_client_perform: the perform loop drives the socket and
 * (for https) the mbedTLS handshake, invoking our event handler as body bytes arrive,
 * so an 8 MB image streams without buffering the whole file. The handler forwards each
 * chunk to the caller's sink, which writes it to the copy destination (SD or TNFS) and
 * tracks the running offset — the caller never sees a raw socket. perform() follows
 * redirects automatically; https:// trusts the bundled CA roots (esp_crt_bundle), the
 * same trust store OTA uses (§11), with nothing host-pinned.
 *
 * Stack: this runs inline on the invoking CLI task, which carries 8 KB (cli.c /
 * console.c) — the same budget OTA gives its mbedTLS task — so no dedicated worker is
 * needed for the TLS path (§5.2 / §8.1).
 */
#include "http.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"

/* http_is_url() is a pure inline in http.h (shared with host-native tests). */

/* A whole-file staging pull may sit on a slow/high-latency link; keep the per-operation
 * socket timeout generous but bounded. Individual reads still stream, so this is the
 * stall timeout, not a total-transfer cap. */
#define HTTP_TIMEOUT_MS 20000

typedef struct {
    http_sink_fn sink;
    void        *ctx;
    uint32_t     bytes;    /* bytes delivered to the sink so far */
    esp_err_t    sink_err; /* ESP_OK until the sink aborts */
} http_get_ctx_t;

static esp_err_t on_http_event(esp_http_client_event_t *e)
{
    if (e->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    http_get_ctx_t *g = (http_get_ctx_t *)e->user_data;
    if (!g || g->sink_err != ESP_OK) {
        return ESP_OK;
    }
    /* Only the final 2xx body should reach the destination — skip any bytes belonging to
     * an intermediate 3xx (redirect) or error response perform() may surface. */
    int status = esp_http_client_get_status_code(e->client);
    if (status < 200 || status >= 300) {
        return ESP_OK;
    }
    if (e->data_len > 0) {
        if (g->sink(g->ctx, e->data, (size_t)e->data_len) != 0) {
            g->sink_err = ESP_FAIL; /* destination write failed */
            return ESP_FAIL;        /* abort the perform loop */
        }
        g->bytes += (uint32_t)e->data_len;
    }
    return ESP_OK;
}

esp_err_t http_get(const char *url, http_sink_fn sink, void *ctx, uint32_t *out_bytes)
{
    http_get_ctx_t g = { .sink = sink, .ctx = ctx, .bytes = 0, .sink_err = ESP_OK };

    esp_http_client_config_t cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach, /* ignored for plain http:// */
        .timeout_ms        = HTTP_TIMEOUT_MS,
        .event_handler     = on_http_event,
        .user_data         = &g,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(cl);
    int status = esp_http_client_get_status_code(cl);
    esp_http_client_cleanup(cl);

    /* A sink abort trumps the transport result: perform() returns ESP_FAIL when our
     * handler aborts, but the destination error is the one worth reporting. */
    if (g.sink_err != ESP_OK) {
        return g.sink_err;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (status < 200 || status >= 300) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (out_bytes) {
        *out_bytes = g.bytes;
    }
    return ESP_OK;
}
