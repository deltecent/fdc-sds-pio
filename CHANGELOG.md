# Changelog

All notable changes to the ESP32 FDC+ Serial Disk Server firmware are recorded here.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
this project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Versioning starts fresh at 1.0.0 for this from-scratch ESP-IDF rewrite; the old Arduino
firmware ended at 0.25.

## [1.0.3] - 2026-09-10

### Added

- Subdirectory support on the SD card. File arguments to every command may now name a
  path inside a folder (`mount 0 cpm/disk1.dsk`, `type docs/readme.txt`,
  `copy a.dsk backup/a.dsk`), confined to the card root — a leading `/`, a `\` separator,
  and any `..` parent reference are rejected. A `Drive<n>` mount may point into a subdir.
- `mkdir` / `rmdir` commands (aliases `md` / `rd`) to create and remove directories;
  `rmdir` requires the directory be empty and says so plainly when it is not.

### Changed

- `dir` lists and descends subdirectories: folders are shown with a trailing `/`,
  `dir <subdir>` and `dir <subdir>/<glob>` list a folder, a `Directory of /…` header
  names what is being listed, and entries are sorted (directories first, then
  case-insensitive alphabetical).
- `rename` / `mv` now moves a file between directories, not just renames in place, now
  that paths accept subdirectories.
- `exec` runs a batch file from its own folder: `exec basic/setup.bat` runs the file, and
  while it runs bare names inside it resolve relative to the batch's directory (a
  `setup.bat` in `basic/` writes `mount 0 setup.dsk`), so a disk-set folder is
  self-contained and relocatable. Nested `exec` stacks; the interactive prompt always
  runs at the SD root (no `cd`); a batch cannot climb out of its folder (`..` rejected);
  and a `mount` made inside a batch persists its root-relative path so it re-mounts at
  boot.

## [1.0.2] - 2026-09-10

### Added

- `diag` command — a one-shot support status report gathering firmware, WiFi, storage,
  and drive state in a single dump.
- `copy` now accepts an `http://` or `https://` URL as its source, so an image can be
  pulled straight from a web server (source only; `https://` verified against the
  bundled CA roots, with redirects followed).
- WiFi status now reports the associated access point's BSSID, with RSSI moved onto that
  `AP:` line.
- Self-contained `tools/flasher.py` USB flasher with a vendored `esptool`.
- MIT `LICENSE`; the vendored GPL/BSD/public-domain tools under `tools/vendor` are noted.

### Changed

- The network console on port 23 is now a minimal Telnet server (negotiates
  character-at-a-time mode with server-side echo) instead of a raw socket.
- `help` overhauled; `logout` / Ctrl-D are now scoped to network consoles only.
- US timezone strings no longer carry explicit DST rules.
- FDC+ CRC and timeout statistics are split by transaction source.
- The FDC+ UART is installed on core 1, and hardware RX faults are now counted.
- Drive-activity LEDs track head-load state and turn off after an idle timeout
  (shortened to about 3 seconds) as a fallback when the FDC stops sending commands.

### Fixed

- Aligned the statistics counter columns in CLI output.

## [1.0.1] - 2026-09-09

### Added

- Network OTA publishes a merged full-flash image alongside the app payload, with a
  build-time guard against `ota/` drift.
- Host-native test guarding that `version.txt` and `include/version.h` stay in sync, so
  a version bump has to touch both.

### Fixed

- Set the flash size to 4 MB and rebuilt the `ota/` payload to match.

## [1.0.0] - 2026-09-09

Initial release of the from-scratch PlatformIO / pure ESP-IDF rewrite. Built up over
milestones M0–M9b:

### Added

- **Boot & platform (M0):** project skeleton, boot banner with firmware version, and an
  LED lamp test.
- **Serial CLI (M1):** line editor, command table, and dispatch over the USB console.
- **Storage & config (M2):** microSD (SDSPI/FAT) storage, NVS configuration, and
  file/config CLI commands.
- **Disk engine (M3):** mount/unmount, track I/O, dump, and directory globbing for
  Altair FDC+ disk images.
- **FDC+ protocol (M4):** the FDC+ serial protocol engine plus a host simulator.
- **Networking (M5):** WiFi station mode, a network console, mDNS advertisement, NTP
  time sync, and network-related CLI commands.
- **FTP server (M6):** an FTP server for uploading images to the SD card, with
  NVS-stored credentials.
- **Firmware update (M7):** local (`/firmware.bin`) and network OTA updates.
- **Scripting (M8):** batch files, an autoexec script, and quoted argument parsing.
- **Remote images (M9a/M9b):** a TNFS client, `copy` to/from `tnfs://`, `dir tnfs://`
  directory listing, and remote disk mounts over TNFS.

### Changed

- WPA2 is required whenever a WiFi password is set.
- Runtime console verbosity is adjustable via a log level knob.

### Fixed

- Dropped console echo under concurrent log output.
- Reported mode and reason on a remote OPEN failure over TNFS.
