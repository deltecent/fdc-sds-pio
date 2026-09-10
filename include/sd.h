/*
 * sd.h — microSD card mount over SPI (DESIGN.md §2.1/§10).
 *
 * Mounts a FAT-formatted card on VSPI (CS 5, CLK 18, MISO 19, MOSI 23) at /sd and
 * keeps it mounted for the life of the run. Disk images and batch files live in the
 * card root; the `disk` module (M3) opens image handles under this mount.
 */
#ifndef FDCSDS_SD_H
#define FDCSDS_SD_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mount point and root path for all SD file operations. */
#define SD_MOUNT_POINT "/sd"

/* Mount the card and print its type/size (DESIGN.md §12 step 3). */
esp_err_t sd_mount(void);

/* True once sd_mount() has succeeded. */
bool sd_mounted(void);

/*
 * Resolve a user-supplied name to an absolute path under the SD root, into buf.
 * Subdirectory paths are allowed ('/' separates components, DESIGN.md §10) but the
 * name stays confined to the root: a leading '/', a '\\' separator, an empty
 * component, and any ".." component are rejected. Returns the path length, or -1 if
 * the name is empty, too long, or fails one of those checks.
 */
int sd_path(const char *name, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* FDCSDS_SD_H */
