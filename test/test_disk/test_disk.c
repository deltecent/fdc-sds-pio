/*
 * test_disk.c — host-native unit tests for the pure disk/protocol logic (plan M3).
 *
 * Runs with `pio test -e native` (no hardware, no ESP-IDF). Covers the arithmetic the
 * old firmware got wrong (track offset in 32-bit, geometry, bounds), the FDC checksum
 * and READ/WRIT drive+track decode (DESIGN.md §6), the STAT mount bitmap, and the
 * `dir` wildcard matcher (DESIGN.md §8.4).
 */
#include <string.h>

#include <unity.h>

#include "protocol.h"
#include "wildcard.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- checksum (DESIGN.md §6.2) --------------------------------------------- */

static void test_checksum_sum_and_wrap(void)
{
    const uint8_t a[4] = {1, 2, 3, 4};
    TEST_ASSERT_EQUAL_UINT16(10, fdc_checksum16(a, sizeof a));

    /* 16-bit wraparound: 0x100 copies of 0xFF = 0xFF00 (no overflow past 16 bits). */
    uint8_t big[256];
    for (int i = 0; i < 256; ++i) {
        big[i] = 0xFF;
    }
    TEST_ASSERT_EQUAL_UINT16(0xFF00, fdc_checksum16(big, sizeof big));
    TEST_ASSERT_EQUAL_UINT16(0, fdc_checksum16(a, 0));
}

/* ---- little-endian words + block checksum framing (DESIGN.md §6.2) --------- */

static void test_le16_roundtrip(void)
{
    uint8_t p[2];
    fdc_write_le16(p, 0xBEEF);
    TEST_ASSERT_EQUAL_UINT8(0xEF, p[0]); /* low byte first */
    TEST_ASSERT_EQUAL_UINT8(0xBE, p[1]);
    TEST_ASSERT_EQUAL_UINT16(0xBEEF, fdc_read_le16(p));
    TEST_ASSERT_EQUAL_UINT16(0x0000, fdc_read_le16((const uint8_t[]){0, 0}));
}

static void test_block_finalize_and_valid(void)
{
    /* Build a STAT command block (drive 0 selected, head loaded) and check its csum. */
    uint8_t blk[FDC_BLOCK_LEN] = {0};
    memcpy(&blk[FDC_OFF_CMD], "STAT", FDC_CMD_LEN);
    fdc_write_le16(&blk[FDC_OFF_WORD1], 0x0100); /* head=1, drive=0 */
    fdc_write_le16(&blk[FDC_OFF_WORD2], 0x0000); /* track 0 */
    fdc_block_finalize(blk);

    /* Checksum = sum of bytes 0..7 = 'S'+'T'+'A'+'T'+0x00+0x01. */
    uint16_t expect = (uint16_t)('S' + 'T' + 'A' + 'T' + 0x00 + 0x01);
    TEST_ASSERT_EQUAL_UINT16(expect, fdc_read_le16(&blk[FDC_OFF_CKSUM]));
    TEST_ASSERT_TRUE(fdc_block_valid(blk));

    /* Corrupt one byte -> checksum must no longer validate. */
    blk[FDC_OFF_WORD2] ^= 0xFF;
    TEST_ASSERT_FALSE(fdc_block_valid(blk));
}

/* ---- READ/WRIT drive+track decode (DESIGN.md §6.3) ------------------------- */

static void test_word1_decode(void)
{
    /* drive 2, track 5 -> (2<<12)|5 = 0x2005. */
    TEST_ASSERT_EQUAL_UINT8(2, fdc_word1_drive(0x2005));
    TEST_ASSERT_EQUAL_UINT16(5, fdc_word1_track(0x2005));

    /* drive 3, max 12-bit track 0x0FFF. */
    TEST_ASSERT_EQUAL_UINT8(3, fdc_word1_drive(0x3FFF));
    TEST_ASSERT_EQUAL_UINT16(0x0FFF, fdc_word1_track(0x3FFF));

    /* drive 0, track 0. */
    TEST_ASSERT_EQUAL_UINT8(0, fdc_word1_drive(0x0000));
    TEST_ASSERT_EQUAL_UINT16(0, fdc_word1_track(0x0000));
}

/* ---- track offset + geometry (DESIGN.md §6.5, the old signed-int bug) ------ */

static void test_track_offset_32bit(void)
{
    /* 8 MB image: track 1095 * 8192 = 8,970,240 — well past a signed 16-bit range. */
    TEST_ASSERT_EQUAL_UINT32(8970240u, disk_track_offset(1095, 8192));
    TEST_ASSERT_EQUAL_UINT32(0u, disk_track_offset(0, 8192));
}

static void test_track_count(void)
{
    /* The 8 MB CP/M image is exactly 1096 tracks of 8192 bytes (DESIGN.md §6.5). */
    TEST_ASSERT_EQUAL_UINT32(1096u, disk_track_count(8978432u, 8192));
    TEST_ASSERT_EQUAL_UINT32(0u, disk_track_count(8978432u, 0));
}

static void test_track_in_bounds(void)
{
    const uint32_t size = 8978432u; /* 1096 * 8192 */

    TEST_ASSERT_TRUE(disk_track_in_bounds(0, 8192, size));
    TEST_ASSERT_TRUE(disk_track_in_bounds(1095, 8192, size)); /* last full track */
    TEST_ASSERT_FALSE(disk_track_in_bounds(1096, 8192, size)); /* one past the end */

    /* A partial track at the tail must be rejected. */
    TEST_ASSERT_FALSE(disk_track_in_bounds(1095, 8192, size - 1));

    /* Degenerate + overflow guards. */
    TEST_ASSERT_FALSE(disk_track_in_bounds(0, 0, size));
    TEST_ASSERT_FALSE(disk_track_in_bounds(0xFFFFFFFFu, 8192, size));
}

/* ---- STAT mount bitmap (DESIGN.md §6.3) ------------------------------------ */

static void test_status_bitmap(void)
{
    bool none[4] = {false, false, false, false};
    TEST_ASSERT_EQUAL_UINT16(0x0000, disk_status_bitmap_of(none, 4));

    bool d0_d2[4] = {true, false, true, false};
    TEST_ASSERT_EQUAL_UINT16(0x0005, disk_status_bitmap_of(d0_d2, 4)); /* bits 0 + 2 */

    bool all[4] = {true, true, true, true};
    TEST_ASSERT_EQUAL_UINT16(0x000F, disk_status_bitmap_of(all, 4));
}

/* ---- dir wildcard glob (DESIGN.md §8.4) ------------------------------------ */

static void test_wildcard_match(void)
{
    /* Star + extension, case-insensitive. */
    TEST_ASSERT_TRUE(wildcard_match_ci("*.BAT", "8MB.BAT"));
    TEST_ASSERT_TRUE(wildcard_match_ci("*.BAT", "8mb.bat"));
    TEST_ASSERT_TRUE(wildcard_match_ci("*.dsk", "CPM22-8MB-56K.DSK"));
    TEST_ASSERT_TRUE(wildcard_match_ci("CPM*", "CPM3.DSK"));
    TEST_ASSERT_TRUE(wildcard_match_ci("*", "anything.txt"));
    TEST_ASSERT_TRUE(wildcard_match_ci("basic5.txt", "BASIC5.TXT"));

    /* Single-char '?' matches exactly one. */
    TEST_ASSERT_TRUE(wildcard_match_ci("?.txt", "a.txt"));
    TEST_ASSERT_FALSE(wildcard_match_ci("?.txt", "ab.txt"));

    /* Non-matches. */
    TEST_ASSERT_FALSE(wildcard_match_ci("*.dsk", "readme.txt"));
    TEST_ASSERT_FALSE(wildcard_match_ci("CPM*", "ZORK.DSK"));
    TEST_ASSERT_FALSE(wildcard_match_ci("abc", "ab"));

    /* Multiple stars. */
    TEST_ASSERT_TRUE(wildcard_match_ci("*8MB*", "CPM22-8MB-56K.DSK"));
    TEST_ASSERT_FALSE(wildcard_match_ci("*8MB*", "CPM3.DSK"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_checksum_sum_and_wrap);
    RUN_TEST(test_le16_roundtrip);
    RUN_TEST(test_block_finalize_and_valid);
    RUN_TEST(test_word1_decode);
    RUN_TEST(test_track_offset_32bit);
    RUN_TEST(test_track_count);
    RUN_TEST(test_track_in_bounds);
    RUN_TEST(test_status_bitmap);
    RUN_TEST(test_wildcard_match);
    return UNITY_END();
}
