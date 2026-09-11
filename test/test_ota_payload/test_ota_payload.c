/*
 * test_ota_payload.c — host-native guard that the committed ota/ payload is in sync.
 *
 * ota/ ships two files that must describe the SAME build:
 *   - ota/firmware.bin  the app image (flashed at 0x10000; served by update ota/local).
 *                       `update ota` reads its embedded version directly — no side marker.
 *   - ota/merged.bin    bootloader + partitions + app (flashed at 0x0 for a full install)
 *
 * tools/build-ota.sh regenerates both from one `pio run`, so they can't disagree within
 * a refresh. This test closes the other gap: after a version bump, if the ota/ payload
 * was NOT rebuilt, the version string embedded in the binaries still reads the old value
 * — this test catches that (exactly the stale-binary drift that motivated it) by
 * comparing the embedded version to version.h, and fails `pio test -e native` until
 * build-ota.sh is re-run.
 *
 * How the version is read: an ESP-IDF app image carries an esp_app_desc_t right after
 * its 24-byte image header + 8-byte first-segment header, i.e. at offset 0x20 in the
 * app .bin. Its layout puts a 32-bit magic (0xABCD5432) at +0x00 and the NUL-terminated
 * version[32] (from PROJECT_VER / version.txt) at +0x10. So in firmware.bin the version
 * is at 0x30; in merged.bin the app sits at 0x10000, so it is at 0x10030.
 *
 * Paths are injected by platformio.ini ([env:native] build_flags) so the files are found
 * regardless of the test runner's working directory.
 */
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "version.h"

#if !defined(OTA_FIRMWARE_PATH) || !defined(OTA_MERGED_PATH)
#error "define OTA_FIRMWARE_PATH, OTA_MERGED_PATH (see [env:native] build_flags)"
#endif

#define ESP_APP_DESC_MAGIC   0xABCD5432u
#define APP_DESC_OFFSET      0x20   /* esp_app_desc_t position within an app image */
#define APP_DESC_VER_OFFSET  0x10   /* version[32] within esp_app_desc_t */
#define APP_SLOT_OFFSET      0x10000 /* where the app lives inside merged.bin (partitions.csv) */

void setUp(void) {}
void tearDown(void) {}

/* Read the esp_app_desc version out of an app image at file offset `app_off`, after
 * checking the esp_app_desc magic word. Returns 0 on success. */
static int read_embedded_version(const char *path, long app_off, char *out, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    unsigned char magic[4];
    if (fseek(f, app_off + APP_DESC_OFFSET, SEEK_SET) != 0 ||
        fread(magic, 1, 4, f) != 4) {
        fclose(f);
        return -2;
    }
    unsigned int m = (unsigned)magic[0] | ((unsigned)magic[1] << 8) |
                     ((unsigned)magic[2] << 16) | ((unsigned)magic[3] << 24);
    if (m != ESP_APP_DESC_MAGIC) {
        fclose(f);
        return -3; /* not an app image at the expected offset */
    }
    char ver[32] = {0};
    if (fseek(f, app_off + APP_DESC_OFFSET + APP_DESC_VER_OFFSET, SEEK_SET) != 0 ||
        fread(ver, 1, sizeof ver, f) != sizeof ver) {
        fclose(f);
        return -4;
    }
    fclose(f);
    ver[sizeof ver - 1] = '\0';
    strncpy(out, ver, cap - 1);
    out[cap - 1] = '\0';
    return 0;
}

/* ota/firmware.bin must embed the current version (else the OTA payload is stale). */
static void test_ota_firmware_embeds_version(void)
{
    char ver[32];
    int rc = read_embedded_version(OTA_FIRMWARE_PATH, 0, ver, sizeof ver);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc,
        "could not read esp_app_desc version from ota/firmware.bin (missing or not an app image)");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(FDCSDS_VERSION_STRING, ver,
        "ota/firmware.bin is a stale build — re-run tools/build-ota.sh");
}

/* ota/merged.bin must carry the same app (version at 0x10000 + esp_app_desc). */
static void test_ota_merged_embeds_version(void)
{
    char ver[32];
    int rc = read_embedded_version(OTA_MERGED_PATH, APP_SLOT_OFFSET, ver, sizeof ver);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc,
        "could not read esp_app_desc version from ota/merged.bin (missing or app not at 0x10000)");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(FDCSDS_VERSION_STRING, ver,
        "ota/merged.bin is a stale build — re-run tools/build-ota.sh");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ota_firmware_embeds_version);
    RUN_TEST(test_ota_merged_embeds_version);
    return UNITY_END();
}
