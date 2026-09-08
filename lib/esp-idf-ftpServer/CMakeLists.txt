# Vendored FTP server component (nopnop2002/esp-idf-ftpServer, MIT — see LICENSE).
# Wrapped behind the project glue in src/net/ftp.c (DESIGN.md §5.3/§9.4). It only
# uses lwIP sockets, the VFS (dirent/stat/opendir), FreeRTOS, and logging; the last
# two are ESP-IDF common requirements, so only lwip + vfs need declaring.
idf_component_register(
    SRCS "ftp.c"
    INCLUDE_DIRS "."
    REQUIRES lwip vfs
)
