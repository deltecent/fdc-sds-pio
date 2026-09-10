/*
 * test_http_url.c — host-native unit tests for the http(s):// copy-source predicate
 * and the copy destination-rejection rule (DESIGN.md §10.3).
 *
 * Runs with `pio test -e native` (no hardware, no ESP-IDF). http_is_url() is a pure
 * inline in http.h; defining FDCSDS_HTTP_NO_IDF pulls in only that predicate (not the
 * esp_http_client streaming API), so these tests exercise the *exact* function the
 * firmware's cmd_copy uses to (a) route a source to the HTTP path and (b) reject an
 * http(s):// URL as a copy destination. The full cmd_copy branch is ESP-IDF-coupled
 * (CLI/TNFS/VFS) and not host-native testable; this covers the decision that gates it.
 */
#define FDCSDS_HTTP_NO_IDF
#include "http.h"

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- http_is_url: recognized as an HTTP(S) copy source --------------------- */

static void test_http_is_url_accepts_http_and_https(void)
{
    TEST_ASSERT_TRUE(http_is_url("http://example.com/CPM22.DSK"));
    TEST_ASSERT_TRUE(http_is_url("https://example.com/CPM22.DSK"));
    /* Scheme match is case-insensitive (RFC 3986 schemes are). */
    TEST_ASSERT_TRUE(http_is_url("HTTP://EXAMPLE.COM/A"));
    TEST_ASSERT_TRUE(http_is_url("HtTpS://example.com/a"));
    /* Real deramp.com URL: encoded spaces (%20) and literal parens must not trip it. */
    TEST_ASSERT_TRUE(http_is_url(
        "https://deramp.com/downloads/altair/software/8_inch_floppy/CPM/CPM%202.2/"
        "Serial%20Disk%208Mb%20CPM%202.2%20(thru%2088-2SIO)/CPM22-8MBS-56K.DSK"));
}

static void test_http_is_url_rejects_non_http(void)
{
    /* Plain SD-root filenames. */
    TEST_ASSERT_FALSE(http_is_url("CPM22.DSK"));
    TEST_ASSERT_FALSE(http_is_url("BACKUP.DSK"));
    /* TNFS endpoints are handled by tnfs_is_url(), not this predicate. */
    TEST_ASSERT_FALSE(http_is_url("tnfs://192.168.1.10/disks/GAMES.DSK"));
    /* Near-misses must not match: other schemes, prefixes, truncations. */
    TEST_ASSERT_FALSE(http_is_url("ftp://example.com/a"));
    TEST_ASSERT_FALSE(http_is_url("httpx://example.com/a"));
    TEST_ASSERT_FALSE(http_is_url("xhttp://example.com/a"));
    TEST_ASSERT_FALSE(http_is_url("http:/example.com"));   /* one slash */
    TEST_ASSERT_FALSE(http_is_url("http"));
    TEST_ASSERT_FALSE(http_is_url("https"));
    TEST_ASSERT_FALSE(http_is_url(""));
    TEST_ASSERT_FALSE(http_is_url(NULL));                  /* NULL-safe */
}

/* ---- destination rejection: cmd_copy refuses an http(s):// <dst> (§10.3) ---- */
/*
 * cmd_copy rejects the destination iff http_is_url(dst) — HTTP has no upload verb, so a
 * push target must stay SD or tnfs://. These assert that exact gate: the endpoints that
 * must be refused as a destination, and those that must be allowed through.
 */

static void test_dst_http_is_rejected(void)
{
    /* Must be rejected as a copy destination. */
    TEST_ASSERT_TRUE(http_is_url("http://example.com/upload/CPM22.DSK"));
    TEST_ASSERT_TRUE(http_is_url("https://example.com/upload/CPM22.DSK"));
}

static void test_dst_sd_and_tnfs_are_allowed(void)
{
    /* Must NOT be rejected — these are valid copy destinations (SD name / TNFS push). */
    TEST_ASSERT_FALSE(http_is_url("CPM22.DSK"));
    TEST_ASSERT_FALSE(http_is_url("tnfs://192.168.1.10/backup/CPM22.DSK"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_http_is_url_accepts_http_and_https);
    RUN_TEST(test_http_is_url_rejects_non_http);
    RUN_TEST(test_dst_http_is_rejected);
    RUN_TEST(test_dst_sd_and_tnfs_are_allowed);
    return UNITY_END();
}
