/*
 * test_version.c — host-native guard that the two version sources stay in sync.
 *
 * The firmware version lives in two places that ESP-IDF and the app read
 * independently:
 *   - include/version.h  (FDCSDS_VERSION_STRING) — the banner and the OTA version
 *     gate (ota.c) compare against these numeric macros.
 *   - version.txt (project root) — ESP-IDF's PROJECT_VER, embedded into the image
 *     as esp_app_desc.version (boot log, and the `update local` status line).
 *
 * If they drift, a network OTA still works but `update local` and the boot log
 * misreport the version (exactly the 1.0.0/1.0.1 mismatch this test was added for).
 * This test fails the `pio test -e native` build until they match, so a version
 * bump has to touch both. VERSION_TXT_PATH is injected by platformio.ini so the
 * file is found regardless of the test's working directory.
 */
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "version.h"

#ifndef VERSION_TXT_PATH
#error "VERSION_TXT_PATH must be defined (see the [env:native] build_flags)"
#endif

void setUp(void) {}
void tearDown(void) {}

/* Read the first line of version.txt, trimming a trailing CR/LF and any leading 'v'. */
static int read_version_txt(char *out, size_t cap)
{
    FILE *f = fopen(VERSION_TXT_PATH, "r");
    if (!f) {
        return -1;
    }
    char line[64] = {0};
    char *got = fgets(line, sizeof line, f);
    fclose(f);
    if (!got) {
        return -1;
    }
    line[strcspn(line, "\r\n")] = '\0';
    const char *p = line;
    while (*p == ' ' || *p == '\t' || *p == 'v' || *p == 'V') {
        ++p;
    }
    strncpy(out, p, cap - 1);
    out[cap - 1] = '\0';
    return 0;
}

/* version.txt (ESP-IDF PROJECT_VER) must equal FDCSDS_VERSION_STRING. */
static void test_version_txt_matches_header(void)
{
    char txt[64];
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, read_version_txt(txt, sizeof txt),
                                  "could not read version.txt at VERSION_TXT_PATH");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(FDCSDS_VERSION_STRING, txt,
        "version.txt and include/version.h disagree — bump both together");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_version_txt_matches_header);
    return UNITY_END();
}
