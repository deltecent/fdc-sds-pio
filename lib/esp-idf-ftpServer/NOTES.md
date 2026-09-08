# Vendored: esp-idf-ftpServer

Source: <https://github.com/nopnop2002/esp-idf-ftpServer> (MIT, see `LICENSE`).
Vendored per DESIGN.md §5.3/§9.4 — an IDF-native FTP server (TCP :21, PASV,
user/pass auth, FATFS/SD over VFS) wrapped behind the project glue in
`src/net/ftp.c`. Only the reusable server (`ftp.c` + its header) is vendored; the
upstream example app (`main.c`), Kconfig, partition tables, and `sdkconfig.defaults`
are not.

## Local changes (kept minimal, so the component stays swappable)

- **Header renamed** `ftp.h` → `ftpserver.h` to avoid colliding with the project's
  own glue header `include/ftp.h`. `ftp.c`'s `#include` updated to match.
- **Runtime credentials.** Upstream reads the login from the compile-time
  `CONFIG_FTP_USER` / `CONFIG_FTP_PASSWORD` Kconfig defines (inside `ftp_task`). We
  need them from NVS at runtime (`ftpUser`/`ftpPass`, DESIGN.md §7), so a new
  `ftp_set_credentials(user, pass)` copies them into the server's `ftp_user`/
  `ftp_pass` buffers. Called by the glue before `ftp_init()`.
- **Serving root** default changed `"/root"` → `"/sd"` (our SD mount, DESIGN.md §10),
  and made overridable at runtime via `ftp_set_root(path)`.
- **Buffer size** exposed via `ftp_set_buffer_size(n)` (read at `ftp_init()`), so the
  glue can widen the default 1 KB transfer buffer.
- **Example driver removed.** The upstream `ftp_task()` (and its `extern` couplings to
  the example's `xEventTask` / `FTP_TASK_FINISH_BIT`) were deleted; the project drives
  the cooperative `ftp_run()` state machine from its own task in `src/net/ftp.c`.
- **Unused includes dropped** (`esp_system.h`, `esp_flash.h`, `nvs_flash.h`,
  `esp_wifi.h`) — the server only touches lwIP sockets, the VFS, and logging, so the
  component's only non-common requirements are `lwip` and `vfs`.
- **LIST/NLST hides dotfiles.** `ftp_list_dir()` skipped only `.` and `..`; it now
  skips every leading-dot name, so FTP listings match the console `dir` rule
  (DESIGN.md §8.1/§8.4) and don't show host cruft (`._*`, `.fseventsd`,
  `.Spotlight-V100`). Dotfiles remain reachable by explicit name.
