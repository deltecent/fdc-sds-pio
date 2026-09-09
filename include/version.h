/*
 * version.h — firmware identity for the FDC+ Serial Disk Server.
 *
 * This is a from-scratch ESP-IDF rewrite; versioning starts fresh at 1.0.0
 * (the old Arduino firmware ended at 0.25). Keep the version as clean semver
 * so the GitHub OTA check (DESIGN.md §11.1) can compare running vs. release.
 */
#ifndef FDCSDS_VERSION_H
#define FDCSDS_VERSION_H

#define FDCSDS_VERSION_MAJOR 1
#define FDCSDS_VERSION_MINOR 0
#define FDCSDS_VERSION_PATCH 2

/* Stringize helpers so the numeric parts and the string stay in sync. */
#define FDCSDS_STR_(x) #x
#define FDCSDS_STR(x) FDCSDS_STR_(x)

#define FDCSDS_VERSION_STRING \
    FDCSDS_STR(FDCSDS_VERSION_MAJOR) "." \
    FDCSDS_STR(FDCSDS_VERSION_MINOR) "." \
    FDCSDS_STR(FDCSDS_VERSION_PATCH)

/* Banner product name (matches the old server's wording for continuity). */
#define FDCSDS_PRODUCT "ESP32 FDC+ Serial Disk Server"

#endif /* FDCSDS_VERSION_H */
