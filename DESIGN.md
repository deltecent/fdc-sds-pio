# FDC+ Serial Disk Server (PlatformIO) — Design Specification

> **Status: DRAFT / STARTING POINT.**
> This document is the *single source of truth* for a **from-scratch** rewrite of the
> ESP32 FDC+ Serial Disk Server under **PlatformIO** with the **native Espressif**
> toolchain. It is **not** a description of the existing Arduino code — it is the
> spec the new code will be built from. It will be edited by the project owner before
> and during implementation. This is **not** a 100% refactor of the old firmware:
> behavior may be added, dropped, or changed here deliberately.
>
> Decision points that the owner should resolve are marked **[DECIDE]**.
> Decision points that Claude should resolve are marked **[CLAUDE]**.
> Anything that is a hardware/wire contract with the FDC+ or with existing SD cards is
> marked **[CONTRACT]** and should not change if drop-in compatibility is desired.

---

## 1. Purpose and Scope

Turn an **ESP32 DEVKIT V1** board into a serial disk server for the Altair 8800
**FDC+** floppy disk controller. Disk images are files on a microSD card; the ESP32
serves and accepts whole tracks over a high-speed UART using the **FDC+ Serial Drive
Server protocol** (designed by Mike Douglas). From the Altair's perspective, real
Altair/Pertec drives appear attached.

In scope for v1:

- Serve up to **4 drives** (0–3), each backed by a disk-image file on SD.
- FDC+ serial protocol: `STAT`, `READ`, `WRIT` (+ `WSTA`) with checksums.
- Console CLI over **USB serial** and a **raw-TCP network console** on port 23
  (reachable with a `telnet`/`nc` client; not the Telnet protocol — see §9.2).
- **WiFi** station mode; **FTP** for image transfer.
- **OTA** firmware update: from `/firmware.bin` on SD **and** over the network from a
  GitHub release (§11.1).
- Persistent configuration in **NVS**.
- Drive-activity **LEDs** and a status LED.

Out of scope for v1 (deferred): access-point provisioning mode, web UI, multiple
simultaneous Telnet clients, >4 drives, disk-image format conversion.

### Documentation

All user- and developer-facing documentation produced for this project (guides,
protocol notes, build/flash instructions, CLI reference, etc.) is authored to live
on this repository's **GitHub Wiki**
(<https://github.com/deltecent/fdc-sds-pio/wiki>), not as loose Markdown files in the
tree. `DESIGN.md` remains the in-repo spec / single source of truth; everything else
is written as, and formatted for, wiki pages. The wiki is a git repo
(`https://github.com/deltecent/fdc-sds-pio.wiki.git`) and its **Home** page has been
initialized, so pages can be cloned, edited, committed, and pushed like any repo.

### Reference material

- `../fdc-sds-esp32/` — the original Arduino firmware and its `DESIGN.md`
  (behavioral reference only; do not copy structure).
- `../fdc-sds-esp32/DESIGN.md` — detailed reverse-engineering of the old firmware.
- <https://deramp.com/downloads/altair/hardware/fdc+/> — FDC+ Manual, the
  **FDC Serial Server Protocol.txt** spec, schematics, the Windows server, and
  firmware sources. Authoritative for the protocol and hardware.
- `./SDCARD/` — reference disk images and batch files (copied from the original repo).

---

## 2. Target Hardware

- **Board:** DOIT ESP32 DEVKIT V1 (ESP-WROOM-32), 4 MB flash, single core use of the
  IDF event loop (`esp_event`).
- **Host toolchain:** PlatformIO (installed via Homebrew at `/opt/homebrew/bin/pio`),
  macOS Apple Silicon.

### 2.1 Pin map **[CONTRACT]**

| Function | Signal | ESP32 GPIO |
|---|---|---|
| FDC+ UART (UART2) | RX (from FDC+ TXD) | GPIO16 |
| FDC+ UART (UART2) | TX (to FDC+ RXD) | GPIO17 |
| microSD (SPI/VSPI) | CS | GPIO5 |
| microSD (SPI/VSPI) | CLK | GPIO18 |
| microSD (SPI/VSPI) | MISO | GPIO19 |
| microSD (SPI/VSPI) | MOSI | GPIO23 |
| Drive 0 LED | active HIGH | GPIO27 |
| Drive 1 LED | active HIGH | GPIO14 |
| Drive 2 LED | active HIGH | GPIO12 |
| Drive 3 LED | active HIGH | GPIO13 |
| Status LED | activity | `LED_BUILTIN` (GPIO2) |

- Console UART = UART0 over USB at **115200 8N1**.
- SD init: SPI at 4 MHz, mount point `/sd`, ≥8 open files.

---

## 3. Framework Decision — **RESOLVED: pure ESP-IDF (native), no Arduino**

Per the "native Espressif tools" requirement, the framework is **ESP-IDF only**
(`framework = espidf`). **No Arduino, and no Arduino-as-component.** `platformio.ini`
has a single `env:devkit-espidf` set as `default_envs`.

This means the whole stack uses IDF APIs directly:

| Concern | IDF component / API |
|---|---|
| FDC+ / console UART | `driver/uart` (UART2 for FDC+, UART0 for console) |
| SD card | `esp_vfs_fat` + `sdspi_host`/`sdmmc` over SPI, mounted `/sd` |
| Config store | `nvs_flash` / `nvs` |
| WiFi (STA) | `esp_wifi` + `esp_netif` + `esp_event` |
| Time | `esp_sntp` (`esp_netif_sntp_*`), `setenv("TZ")` + `tzset()` |
| OTA | `esp_ota_ops` (SD) and `esp_https_ota` (network, §11.1) |
| Timers | `esp_timer` (LED/timeout); `gptimer` if a hardware timer is needed |
| Web (if added later) | `esp_http_server` / `esp_https_server` |

**Ports required (no Arduino libs to lean on):** Telnet, FTP, and the CLI line editor
are all built on raw sockets / UART in IDF — see §8 (CLI), §9.2 (Telnet), §9.4 (FTP).
Vendor an IDF-native FTP component under `lib/`; do not port an Arduino library.
Everywhere below that this spec once said "Arduino vs. IDF," read **IDF**.

---

## 4. Directory Layout

```
fdc-sds-pio/
├── platformio.ini          # envs, board, partitions, lib_deps
├── partitions.csv          # dual-OTA + NVS, 4 MB flash
├── DESIGN.md               # this file (source of truth)
├── CLAUDE.md               # working notes for AI assistance
├── README.md
├── include/                # public/shared headers (config.h, pins.h, protocol.h)
├── src/                    # application source (written from scratch, see §5)
│   ├── main.cpp            # lifecycle: app_main + FreeRTOS tasks
│   ├── fdc/                # protocol engine
│   ├── disk/               # mount/unmount, image I/O, directory
│   ├── cli/                # command interpreter + command table
│   ├── net/                # wifi, telnet, ftp glue, ntp
│   └── config/            # NVS load/save
├── lib/                    # private/vendored libs (e.g. SimpleFTPServer port)
├── test/                   # PlatformIO unit tests (native + on-target)
├── docs/                   # protocol spec, manual excerpts
├── boards/                 # custom board defs if needed
└── SDCARD/                 # reference disk images + batch files (card contents)
```

> Directory boundaries are a suggestion; the module responsibilities in §5 matter more
> than the exact folders. Each module should have a header exposing a small API and a
> `.cpp`; **no** reliance on Arduino's implicit cross-file prototype generation.

---

## 5. Software Architecture

### 5.1 Modules

| Module | Responsibility | Key API (sketch) |
|---|---|---|
| `config` | Load/save settings in NVS; defaults; dirty flag | `config_load()`, `config_save()`, `config_wipe()`, accessors |
| `disk` | Mount/unmount images (SD **or** TNFS backing, §10.1), open handles, dir listing, track read/write at offset | `disk_mount(n, path)`, `disk_unmount(n)`, `disk_read_track()`, `disk_write_track()`, `disk_status_bitmap()` |
| `fdc` | UART framing, checksums, STAT/READ/WRIT/WSTA, timeout timer, LEDs, stats | `fdc_init()`, `fdc_poll()` |
| `cli` | Line editor, command table, dispatch, shared across consoles | `cli_init()`, `cli_feed(stream, ch)` / `cli_poll(console)` |
| `net` | WiFi STA connect, Telnet server, FTP server, NTP, events | `net_init()`, `net_poll()` |
| `main` | Wire modules together; main loop / task | — |

### 5.2 Execution model — **RESOLVED: FreeRTOS tasks**

The old firmware used one cooperative `loop()`. v1 uses **FreeRTOS tasks** (IDF's
native model) to isolate the latency-critical FDC serial path (403.2 kbaud, ~1 s FDC
command timeout) from networking and CLI work. `app_main` initializes subsystems
(§12) and spawns the tasks below, then returns.

| Task | Priority | Core | Responsibility |
|---|---|---|---|
| `fdc` | **High** | **1 (APP)** | The only task on the FDC UART. Blocks on UART RX; parses STAT/READ/WRIT/WSTA; does the SD track read/write inline; drives LEDs + stats. Never blocked by net/CLI. |
| `cli` | Low | 0 | Serial (UART0) + Telnet line editors and command dispatch. |
| `net` | Low | 0 | WiFi/`esp_netif` event handling, Telnet accept, NTP; owns the FTP server (its own task/threads from the component). |

Rules that make the split safe:

- **Pin `fdc` to core 1** and keep the WiFi/lwIP stack (which lives on core 0) off its
  path, so radio bursts can't stall serial timing. `fdc` must not block on any
  network or CLI resource.
- **SD access is shared** by `fdc` (track I/O) and `cli`/`net` (`dir`, `copy`, FTP,
  OTA). Serialize it with a **mutex**; the `fdc` track path takes it briefly per
  READ/WRIT. Long CLI/FTP file ops must chunk their I/O so they never hold the SD
  mutex long enough to breach the FDC's ~1 s timeout (this replaces the old
  "pump `fdc_poll()` during copy" hack).
- **Config** is shared state: guard the in-memory config + dirty flag with a mutex
  (or confine writes to one task and publish via a lock).
- Cross-task hand-offs (e.g. a CLI `mount` that changes what `fdc` serves) go through
  the `disk`/`config` APIs under their locks — no direct poking of another task's
  data.
- Console output is routed to the "active console" stream; serialize writes so serial
  and Telnet output don't interleave.

Overriding invariant (unchanged): the FDC serial path must not be blocked longer than
the FDC's ~1 s command timeout, and should respond within a few ms.

### 5.3 Third-party / vendored components **[RESOLVED: allowed for any subsystem]**

Reusing existing code is not limited to FTP. **Any subsystem may use a vendored
community component** rather than being written from scratch where a good one exists —
FTP (§9.4) is just the first example. Ground rules:

- **Must be ESP-IDF-native.** No Arduino libraries or Arduino-as-component (§3). Prefer
  components from the **ESP Component Registry** (`idf.py add-dependency` /
  `idf_component.yml`) or a maintained GitHub repo.
- **Vendor it** under `lib/` (or pin it as a managed component in
  `src/idf_component.yml`) and **pin an exact version** — no floating refs.
- **Check the license** is compatible with this project before adding.
- **Wrap it behind a thin project glue API** (e.g. `net/ftp`, `net/http`) so our code
  depends on our types, not the library's — a component can then be swapped or replaced
  with a hand-rolled version without touching callers.
- **Keep it off the latency-critical FDC path** unless it is trivially non-blocking;
  the `fdc` task (§5.2) stays hand-written.
- Falling back to our own implementation is always fine when no maintained component is
  reliable enough (see the FTP note in §9.4).

#### Candidate components (evaluated)

Verified against the ESP Component Registry / GitHub. Pin exact versions at integration
time; re-check maintenance before committing.

| Need | Component | Source | License | Verdict |
|---|---|---|---|---|
| FTP server | **`esp-idf-ftpServer`** (nopnop2002) | [GitHub](https://github.com/nopnop2002/esp-idf-ftpServer) | MIT | **Use.** IDF ≥5.0, SD-over-SPI + FATFS, PASV, username/password auth. See §9.4 for the NVS-credentials adaptation. |
| FTP server (alt) | `espp/ftp` | [ESP Registry](https://components.espressif.com/components/espp/ftp) | MIT | **Rejected.** No authentication (accepts any login) and it's part of the larger `esp-cpp`/`espp` C++ framework — too much to pull in. |
| mDNS `.local` discovery | **`espressif/mdns`** | [ESP Registry](https://components.espressif.com/components/espressif/mdns) | Apache-2.0 | **Use (optional but cheap).** Advertise `<wifiName>.local` so Telnet/FTP/OTA don't need the IP. See §9.1. |
| CLI parsing / command table | `esp_console` + `linenoise` + `argtable3` | built into ESP-IDF | Apache-2.0 / BSD | **Partial.** Reuse `argtable3` + command registration, but **hand-roll the line editor** — `esp_console`'s REPL is single-stream (stdio/UART) and doesn't fit our dual serial + raw-TCP "active console" model (§8/§9.2). |

First-party ESP-IDF pieces we simply **use rather than hand-roll** (not "vendored," but
same spirit): `esp_http_server`/`esp_https_server` (web GUI, deferred §8.1),
`esp_https_ota` (§11.1), `esp_sntp` (NTP §9.1), `esp_vfs_fat` + `sdspi_host` (SD §10),
`esp_ota_ops` (§11), `esp_timer` (LED/timeout §5.2/§6.7).

---

## 6. FDC+ Serial Drive Server Protocol **[CONTRACT]**

Authoritative spec: `docs/FDC Serial Server Protocol.txt` (from deramp.com). Summary:

### 6.1 Link
- **8N1**, baud rate configurable. Supported: `9600, 19200, 38400, 57600, 76800,
  115200, 230400, 403200, 460800`. Default **403200** (best accuracy + full speed).
  `460800` works but ~3.5% off; `230400` is common and runs ~80–90% disk speed.
- All transactions are FDC-initiated; the server is a passive responder.
- Multi-byte words are **little-endian**.
- Setting baud may require a UART stop/restart (or explicit divisor set) at these high
  rates for the clock divisor to apply — verify on the chosen driver.

### 6.2 Command / response block (10 bytes)

```
Bytes 0-3   Bytes 4-5 (word)   Bytes 6-7 (word)   Bytes 8-9 (word)
Command     Word 1             Word 2             Checksum
```

- 4-byte ASCII command + two 16-bit little-endian words + 16-bit checksum.
- **Checksum** = 16-bit sum of the preceding bytes (8 for a block; all data bytes for
  a track transfer), sent little-endian. The transfer-length field for track data
  **excludes** the 2 checksum bytes.

### 6.3 Commands

**STAT** (FDC polls ~10×/s):
- Word1 LSB = selected drive number (`0xff` = none). Word1 MSB = head-load (nonzero =
  loaded). Word2 (from FDC) = current track.
- Server responds with a **mounted bitmap** in Word2: bit *n* = drive *n* mounted
  (bits 15–0 → drives 15–0). Only 0–3 used. Response code ignored.

**READ**:
- Word1 = drive number in high nibble of the high byte, track in the low 12 bits:
  `drive = word1_MSB >> 4; track = ((word1_MSB & 0x0F) << 8) | word1_LSB`.
- Word2 = transfer length = track length.
- Server seeks `track * len`, reads `len` bytes, sends the data block + checksum.

**WRIT**:
- Same drive/track encoding as READ; Word2 = length.
- Server replies `WRIT` with response code `OK (0)` (ready) or `NOT_READY (1)`
  (drive not mounted). If OK, server then receives `len` data bytes + checksum,
  seeks `track * len`, writes, then sends **`WSTA`** with the final code.

**Response codes:** `0 OK`, `1 Not Ready`, `2 Checksum error`, `3 Write error`.

### 6.4 Error recovery
- ~1 s FDC timeout after last byte determines a dropped transaction.
- Ignore commands with a bad checksum (FDC retries). Bad checksum on **write data** →
  return `WRIT` response code `2` (do not silently ignore).
- Maintain per-call receive timeouts (e.g. ~1 s for a command block, ~7 s for a data
  block) and flush RX on timeout.

### 6.5 Track geometry (informative, **not** hardcoded)
The server is geometry-agnostic: it uses the FDC's per-command transfer length and
computes the byte offset as `track * len`. Images are flat, headerless concatenations
of fixed-length tracks. Observed from the reference SD images:

| Image class | File size | Implied geometry |
|---|---|---|
| Standard Altair floppy (`.dsk`) | 337,664 bytes | ~77-track 8" CP/M/BASIC images |
| 8 MB CP/M disk (`8MB`, blank, Games, Zork) | 8,978,432 bytes | **1096 tracks × 8192 bytes** |

The **8192-byte** track of the 8 MB format sets the **minimum track-buffer size**.

> **[RESOLVED]** Track-offset arithmetic: the 8 MB image (8,978,432 B) exceeds a signed
> 16-bit range and `track * len` can approach 2^23. Use `uint32_t`/`size_t`/`off_t`
> for offsets and sizes (the old code used `int` — do not repeat that). If images
> larger than 8 MB are ever supported, revisit the 12-bit track field limit.

### 6.6 Track buffer
- Single track buffer sized to the largest supported track. **≥ 8192 bytes.**
  READ/WRIT must reject `len` larger than the buffer.
- A 1-entry "last (drive,track,len)" cache may skip redundant SD reads (optional).

### 6.7 LEDs
- Status LED (built-in): on when a valid command is received; a hardware/`esp_timer`
  timeout (~75 ms) turns it off, so it reflects live activity.
- Drive LEDs: set the selected drive on STAT (when head loaded). Because CP/M and FLEX
  serial drivers don't send STAT, READ/WRIT should also drive the LEDs (clear all,
  set the accessed drive).

### 6.8 Loopback self-test
Provide a `loopback` CLI command: send a 256-byte incrementing pattern out the FDC
UART and read it back (TX↔RX jumper), reporting mismatches — validates wiring and baud.

---

## 7. Configuration & Persistence

Store in ESP32 **NVS**. **[RESOLVED: start clean]** — v1 defines its own schema; it
does **not** migrate the old device's namespace/keys (the timezone format changed to a
POSIX `TZ` string, filenames grew to 64 chars, and new keys were added, so a clean
break is cleaner than partial migration). Existing units reconfigure once after flash.
v1 schema (namespace `fdcsds`):

| Key | Type | Meaning | Default |
|---|---|---|---|
| `baudRate` | u32 | FDC+ baud | `403200` |
| `logLevel` | u8 | Console log verbosity, `esp_log_level_t` 0–5 *(see §13)* | `2` (warn) |
| `wifiEnabled` | bool | WiFi on/off | `false` |
| `wifiSSID` | string(≤80) | SSID | empty |
| `wifiPass` | string(≤80) | password | empty |
| `wifiName` | string(≤40) | hostname / device name / prompt | `FDC-SDS-ESP32` |
| `timeZone` | string(≤40) | Selected timezone, POSIX `TZ` string | `UTC0` *(see §8.3)* |
| `ftpUser` | string(≤32) | FTP username *(see §9.4)* | `fdc` |
| `ftpPass` | string(≤32) | FTP password *(see §9.4)* | `fdc` |
| `otaRepo` | string(≤64) | GitHub `owner/repo` for network OTA *(see §11.1)* | `deltecent/fdc-sds-pio` |
| `Drive0`…`Drive3` | string(≤128) | mounted image: SD filename **or** `tnfs://` URL (§10.1) | empty |

Rules:
- Changes are in-memory until an explicit **`save`**; a **dirty flag** shows a leading
  `* ` in the prompt.
- **`wipe`** erases NVS and reloads defaults.
- At boot, re-mount each drive whose `Drive<n>` filename is set. A `Drive<n>` holding a
  `tnfs://` URL is mounted after WiFi connects, not at boot (§10.1 / §12).
- The `Drive<n>` value cap is **128** (not 64) so it can hold a full `tnfs://` URL; the
  SD-filename cap stays 64 chars (§10).
- `wifiName` doubles as WiFi hostname and CLI prompt prefix (e.g. `FDC-SDS-ESP32>`).

---

## 8. Command-Line Interface

One command interpreter serves **both** consoles:

- **Serial** (UART0/USB, 115200), local echo **on**.
- **Network console** (TCP :23), echo **off** (client echoes); available only when
  WiFi up. This is a **raw TCP line console**, *not* the Telnet protocol — see §9.2.
- **HTTP web GUI** — **[CLAUDE — resolved: defer to a post-v1 phase, keep the door open].**
  Feasible: the ESP32 comfortably runs a web server (IDF `esp_http_server` /
  `esp_https_server`) and could offer mount/unmount, config edit, an
  upload form, and browser-driven OTA. But it is a meaningful surface (HTML/JS assets,
  form handling, multipart upload, auth) that competes with the latency-critical FDC
  path for a v1 that must ship. **Recommendation:** not in v1. The CLI + FTP already
  cover mounting, configuration, and image transfer. Design the `config`, `disk`, and
  OTA modules so their operations are callable behind a thin API (not wired only into
  the CLI parser), so a web GUI can be added later without refactoring. Revisit once
  the serial server and networking are solid.

Line editor: handle backspace (`\b`/0x7F), CR/LF (collapse CRLF), Ctrl-D on the network
console = disconnect, bounded input buffer (~80 bytes). Track the "active console" so
command output goes to the right stream. Commands support abbreviations/aliases. The
editor accepts printable ASCII plus that handful of control keys and **silently drops
any other byte** — which also means a stray Telnet `IAC` (0xFF) sequence from a
`telnet` client is discarded rather than parsed (§9.2), so no Telnet handling is
needed.

### 8.1 Command set **[RESOLVED: this is the locked v1 set]**

| Command (aliases) | Args | Description |
|---|---|---|
| `help` / `?` | — | Command help |
| `baud` | rate | Set FDC+ baud rate |
| `dir` / `ls` | — \| spec | List SD root (name + size; hide dotfiles); optional glob `spec` filters (e.g. `*.BAT`, §8.4) |
| `mount` | — \| drive file\|url | Show mount table / mount on drive 0–3: SD file **or** `tnfs://` URL (§10.1) |
| `unmount` / `umount` | drive | Unmount a drive |
| `stats` | — | Baud + STAT/READ/WRIT/ERR/TOUT counters + last-op strings |
| `clear` | — | Zero statistics |
| `log` | level | Show/set console log level: `none`/`error`/`warn`/`info`/`debug`/`verbose` (also `off`=none, `on`=info); default `warn` (§13) |
| `save` / `write` | — | Persist config to NVS |
| `wipe` | — | Erase NVS, reload defaults |
| `dump` | — | Hex-dump current track buffer |
| `wifi` | — \| ON\|OFF | Show WiFi status / enable / disable |
| `ssid` | ssid | Set SSID |
| `pass` | password | Set password |
| `hostname` | name | Set device/host name |
| `ftpuser` | name | Set FTP username (§9.4) |
| `ftppass` | password | Set FTP password (§9.4) |
| `reboot` | — | Clean shutdown + restart |
| `update` | — \| `local` \| `ota` | Firmware: bare = show versions (running/SD/OTA); `local` flashes SD `/firmware.bin`; `ota` = network OTA from `otaRepo` (§11.1) |
| `version` | — | Firmware version |
| `type` / `cat` | file | Print a text file |
| `exec` / `run` | file | Run a batch file of CLI commands |
| `logout` / `exit` | — | Disconnect Telnet client |
| `delete` / `rm` | file | Delete a file |
| `rename` / `mv` | old new | Rename a file |
| `copy` / `cp` | src dst | Copy a file (chunked I/O, §5.2); `src`/`dst` may be an SD name **or** a `tnfs://` URL (§10.1) |
| `loopback` / `lb` | — | FDC+ serial loopback test |
| `time` / `date` | — | Show current (UTC) time |
| `tz` | timezone | Set/show timezone; `tz ?` lists the US zones in §8.3 |

- All filename args resolve relative to SD root.
- Bounds-check drive numbers as `0..MAX_DRIVE-1` (fix the old `> MAX_DRIVE` off-by-one).
- The set stays close to the original baseline at the level of command *names*: the new
  names beyond it are `ftpuser`/`ftppass` (§9.4) and `log` (the console-verbosity knob,
  §13). Existing commands gained argument forms:
  the `update local`/`update ota` arguments (bare `update` reports version status; a bare
  `update <url>` also works but is undocumented, §11.1), an optional glob `spec` on
  `dir`/`ls` (§8.4), a
  `tnfs://` URL as a `mount` target, and `tnfs://` endpoints on `copy` (§10.1).
- Wildcards apply only to `dir`/`ls`; other file commands take one explicit name in v1
  (no glob-delete/-copy). `type`/`delete`/`rename` operate on SD only; `copy` is the one
  file command that also accepts `tnfs://` (§10.1).

### 8.2 Batch files & AUTOEXEC
- `exec <name>` reads `<name>` (or `<name>.bat`) from SD and feeds each line to the
  parser; lines starting with `#` are echoed as comments.
- Unknown command ending in `.bat` (or with a matching `/<name>.bat`) is auto-run.
- At startup, run `/autoexec.bat` if present.
- Example (`SDCARD/8mb.bat`): `mount 0 CPM22-8MB-56K.DSK` … etc.

### 8.3 Timezones **[CLAUDE — resolved]**

`tz` stores a **POSIX `TZ` string** in NVS (`timeZone` key) and applies it with
`setenv("TZ", …); tzset();` so `time`/NTP render local wall-clock with correct DST.
`tz ?` lists **only US zones** (per the owner's note); `tz <name>` accepts a name
below **or** a raw POSIX `TZ` string (so non-US users are not locked out). Default is
`UTC0`.

| Name | POSIX `TZ` | Notes |
|---|---|---|
| `UTC` | `UTC0` | Default; no DST |
| `Eastern` | `EST5EDT,M3.2.0,M11.1.0` | US DST rules |
| `Central` | `CST6CDT,M3.2.0,M11.1.0` | |
| `Mountain` | `MST7MDT,M3.2.0,M11.1.0` | |
| `Arizona` | `MST7` | Mountain, no DST |
| `Pacific` | `PST8PDT,M3.2.0,M11.1.0` | |
| `Alaska` | `AKST9AKDT,M3.2.0,M11.1.0` | |
| `Hawaii` | `HST10` | No DST |

> DST transition rules (`M3.2.0`/`M11.1.0` = 2nd Sun of Mar / 1st Sun of Nov) are the
> current US rules; hardcoding them avoids shipping the full IANA tz database.

### 8.4 Directory wildcards **[RESOLVED]**

`dir`/`ls` take an optional filename `spec`. With no arg they list the whole SD root
(hiding dotfiles, as today). With a `spec` they list only matching entries using a
**case-insensitive glob** over the (long) filename: `*` matches any run of characters,
`?` matches exactly one — e.g. `dir *.BAT`, `ls CPM*.DSK`, `dir ?.txt`.

- Implementation: `fnmatch(spec, name, FNM_CASEFOLD)` where the toolchain provides it,
  else a small hand-rolled `*`/`?` matcher (case-insensitive). Case-insensitivity matches
  the CP/M / DOS `DIR *.BAT` expectation and the FAT filesystem.
- The dotfile-hiding rule still applies unless the `spec` itself begins with `.`.
- A `spec` that matches nothing prints the normal footer with `0 file(s)`.
- Wildcards are a **listing** convenience only — see §8.1: no glob-delete/-copy in v1.

---

## 9. Networking

### 9.1 WiFi (STA)
- Connect only if enabled and SSID+password set; set hostname to `wifiName`.
- Connect to strongest SSI in Multi-AP (meshed) environments.
- Handle connect/disconnect events; on connect start Telnet + FTP, on disconnect stop
  them. Auto-reconnect on drop.
- NTP so `time` is meaningful (`esp_sntp`).
- **mDNS (optional):** advertise `<wifiName>.local` plus the telnet/ftp service records
  via the **`espressif/mdns`** component (§5.3), so clients reach the box by name
  instead of chasing its DHCP IP. Enable alongside WiFi; harmless if the client LAN
  lacks mDNS.

### 9.2 Network console (raw TCP, "telnet"-reachable)
- TCP :23. On connect, print banner + prompt and route the CLI to the socket stream.
  Serial console stays active concurrently.
- **Not the Telnet protocol.** This is a plain byte-stream TCP console: a socket the
  CLI reads lines from and writes output to, nothing more. There is **no Telnet option
  negotiation, no `IAC`/0xFF command handling, no line-mode/echo negotiation, no
  binary-mode escaping.** We named the port 23 only so the ordinary `telnet` client
  reaches it; `nc host 23` works identically (and, being negotiation-free, is the
  cleaner client).
- Consequences of raw mode, by design:
  - A real `telnet` client may emit a few `IAC` (0xFF …) negotiation bytes at connect.
    We **ignore/drop** them (§8 line editor drops non-handled bytes); we never reply,
    so the client falls back to its defaults. `nc` sends none.
  - The client is expected to **echo locally** (server echo is off, §8); with `nc`,
    typed characters may not echo — that's the client's business, not ours.
  - The console carries text only; the binary FDC path is a separate UART, so 0xFF in
    the byte stream is never meaningful here.

### 9.3 ssh console
- SSH :22. On connect, print banner + prompt and route the CLI to the Ssh stream.
  Serial console stays active concurrently.
- **[CLAUDE — resolved: keep Telnet for v1; SSH is not worth it here].**
  SSH *is* technically possible on an ESP32 (e.g. `wolfSSH` over `wolfSSL`, or
  `libssh2`), but the cost is high: the key exchange + AEAD ciphers pull in a large
  TLS/crypto stack (tens of KB of flash and a heavy per-session RAM footprint on a
  single-connection device), the first handshake is slow, and it adds host-key
  management and a real maintenance burden — all to protect a device that is meant to
  sit on a **trusted home lab LAN** serving a hobby vintage computer. The threat model
  does not justify it. **Recommendation:** ship Telnet only. If confidentiality is ever
  needed, the cheaper path is the web GUI (§8.1) behind HTTPS via `esp_https_server`,
  or simply tunneling Telnet over the LAN. Treat §9.3 as **deferred / likely dropped**.

### 9.4 FTP server
- Primary way to move disk images to/from SD over the network. TCP :21, SD storage.
- **Credentials — [RESOLVED: configurable in NVS, default `fdc`/`fdc`].** Store
  `ftpUser`/`ftpPass` in NVS (§7), defaulting to `fdc`/`fdc` so nothing breaks out of
  the box; the owner can change them via CLI (`ftpuser`/`ftppass`, §8.1) and `save`.
- **Library — [CLAUDE — resolved: vendor a community IDF component; do not build our own].**
  FTP looks simple but has real edge cases — the separate control/data channels,
  active vs. **PASV** mode, `TYPE I`/`REST` for large binary images, `LIST` formatting
  that clients parse, and firewall/NAT quirks. A from-scratch server would re-litigate
  all of that for no benefit. Note: **core ESP-IDF ships no first-party FTP server**,
  and since the framework is pure ESP-IDF (§3) the Arduino `SimpleFTPServer` is out.
  So the choices are a **community IDF-native, VFS-based component** or writing our own.
  **Recommendation:** vendor **`nopnop2002/esp-idf-ftpServer`** (MIT, IDF ≥5.0,
  SD-over-SPI + FATFS, PASV, user/pass auth — see §5.3), behind a thin `net/ftp` glue
  API, per the general policy in §5.3.
  - **Credentials adaptation:** that component reads its login from compile-time
    `menuconfig` (`CONFIG_FTP_USER`/`CONFIG_FTP_PASSWORD`). We need it from **NVS at
    runtime** (`ftpUser`/`ftpPass`, §7). Since it's MIT, adapt the vendored copy to take
    the credentials as start-up parameters fed from config rather than Kconfig defines —
    a small, local change kept in our `lib/` copy.
  - Fall back to a custom implementation only if the component proves unreliable at our
    image sizes (8 MB transfers).

### 9.5 TNFS client (remote disk images) **[RESOLVED: in scope for v1]**

TNFS (the FujiNet **"The Network File System"**) lets an image live on a remote **TNFS
server** instead of the SD card, addressed by a `tnfs://host[:port]/path` URL. It backs
two features: mounting a drive from a remote image (§10.1) and `copy` to/from the server
(§10.2). This subsection covers the transport; the disk-facing semantics are in §10.

- **Protocol.** Default transport **UDP**, default port **16384**; session-based
  (`MOUNT` → session id, then `OPEN`/`LSEEK`/`READ`/`WRITE`/`CLOSE`, `UMOUNT`),
  little-endian, retried datagrams with a sequence byte and per-request timeout/retry.
  Reference: the FujiNet `tnfsd` server project and its protocol document.
- **Client — [CLAUDE — resolved: hand-roll behind `net/tnfs`].** The protocol is small
  and there is no clean standalone IDF-native component (existing ones are entangled with
  FujiNet firmware). Write a compact client on IDF BSD sockets behind a thin `net/tnfs`
  glue API (per §5.3) exposing exactly what callers need: `open`, `size`, `read_at`,
  `write_at`, `close`. One session per mounted remote drive; a transient session for a
  `copy`.
- **Off the `fdc` task.** lwIP/WiFi live on core 0; the `fdc` task is pinned to core 1
  and must never block on the network (§5.2). Remote track I/O is serviced on the
  `net`/CLI side, not inline on `fdc` — see §10.1 for dispatch and timeout bounding.
- **Availability.** Enabled only when WiFi is up (§9.1); on WiFi drop, remote drives go
  **not-ready** until reconnect, and an in-flight `copy` fails cleanly.

---

## 10. Storage (SD Card)

- FAT-formatted microSD over SPI (§2.1), mounted `/sd`.
- Disk images are plain files in SD **root**. Old filename cap was 30 chars incl.
  leading `/` — Increase to 64 characters.
- Image handle opened `r+` at mount, kept open for the mount lifetime; READ/WRIT seek
  and transfer on the open handle; unmount closes it.
- `firmware.bin` in root is the image `update local` flashes (§11).
- `SDCARD/` in this repo mirrors intended card contents: CP/M 2.2 (8 MB + standard),
  CP/M 3, Disk/Timeshare BASIC, AltairDOS, Lifeboat, Games, Zork, a blank 8 MB image,
  plus `.bat` mount scripts and PDFs. Print card type/size on init.
- **[CLAUDE — resolved: keep v1 flat (root only), but don't hardcode "root"].**
  The FAT/VFS layer already supports subdirectories, so the cost isn't the filesystem —
  it's the UI/UX surface: `dir` would need to show and descend folders, `mount`/`type`/
  `delete`/`rename`/`copy` would need path resolution and a notion of "current
  directory," and batch/`autoexec` path handling would grow. For a card that holds a
  few dozen disk images, a flat root is simpler and matches the FDC+ workflow.
  **Recommendation for v1:** images live in SD **root** (as today). *But* write the
  `disk`/CLI path handling to take a **full path string** rather than assuming root
  (e.g. resolve args against a base dir constant), so adding `cd`/subdirectory browsing
  later is additive, not a rewrite. Defer full directory navigation to the same
  post-v1 phase as the web GUI.

### 10.1 Remote images over TNFS **[RESOLVED: in scope for v1]**

A drive can be mounted from a **TNFS server** as well as from SD —
`mount 0 tnfs://host[:port]/path/CPM22-8MB-56K.DSK` — and a `Drive<n>` config value may
hold such a URL (§7). The FDC+ path is unchanged: the FDC still asks for whole tracks;
only the *backing* differs.

- **Backing abstraction [CLAUDE — resolved].** The `disk` module opens each mount through
  a small backing interface — `size()`, `read_at(off,len,buf)`, `write_at(off,len,buf)`,
  `close()`. **Local** backing = `pread`/`pwrite` on the open FATFS handle (§10);
  **remote** backing = TNFS `LSEEK`+`READ`/`WRITE` via `net/tnfs` (§9.5).
  `disk_read_track`/`disk_write_track` and the FDC engine call the backing and don't know
  which it is. Design the M3 disk API this way from the start so TNFS is additive (same
  spirit as the "don't hardcode root" rule above).
- **Latency vs the ~1 s FDC timeout — the key risk [DECIDE].** An 8192-byte track is many
  TNFS datagrams (per-read payload is capped), each a WiFi round trip; a blocking remote
  read can approach or exceed the FDC command timeout, and it must **not** run on the
  core-1 `fdc` task (§9.5 / §5.2). Mitigations to build and measure: keep the 1-entry
  track cache warm with **read-ahead**; service remote I/O on core 0 while the FDC path
  waits under its longer *data* timeout (~7 s, §6.4) rather than the 1 s command timeout;
  treat TNFS as **best-effort** — the owner may need a lower baud for remote drives.
  Validate timing on real hardware before declaring TNFS done (plan gate).
- **Read-only option.** Default **read-write** to match SD; allow a mount to be flagged
  read-only (WRIT → `Not Ready`) for servers the owner does not want written.
- **Lifecycle / ordering.** Remote mounts need the network, so a `tnfs://` `Drive<n>` is
  **not** mounted at boot step 5 with the SD drives; it is deferred until WiFi connects
  (step 9), mounted then, re-mounted on reconnect, and dropped to not-ready on
  disconnect. See §12.

### 10.2 File copy between TNFS and SD **[RESOLVED: in scope for v1]**

`copy` transfers whole files (not tracks) between the SD card and a TNFS server, in
either direction, so images can be staged without a separate FTP client:

- `copy tnfs://host/path/CPM3.DSK CPM3.DSK` — **pull** a remote file to SD root.
- `copy CPM3.DSK tnfs://host/backup/CPM3.DSK` — **push** an SD file to the server.
- SD-to-SD (`copy a.dsk b.dsk`) is the existing local case; TNFS-to-TNFS is allowed too.

Rules: the endpoint with a `tnfs://` prefix is remote, otherwise it is an SD-root name;
copy streams in bounded chunks and, when the SD side is involved, releases the **SD
mutex between chunks** so it never breaches the FDC ~1 s timeout (§5.2) — the same
chunking discipline as local `copy` and FTP. Copy uses a **transient** TNFS session
(open → stream → close), independent of any mounted-drive session. On any network error
mid-copy, report failure and leave no partial file mounted (a partial SD file may remain,
as with a failed local copy — noted, not cleaned in v1).

---

## 11. Firmware Update (OTA)

`update` (no argument) reports status only and installs nothing: the running firmware
version, whether an installable `/firmware.bin` is present on the SD root (with the
version embedded in that image), and — only when WiFi is up — the version the configured
repo is offering (§11.1). Installing is an explicit verb.

`update local`:
1. Open `/firmware.bin` (non-empty file) from SD root.
2. Stream it into the OTA writer (`esp_ota_ops`: `esp_ota_begin`/`_write`/`_end`)
   targeting the inactive slot.
3. On success, set boot partition, delete `/firmware.bin`, reboot.

`update ota` is the network path (§11.1). Both require the dual-OTA partition table
(`partitions.csv`). Offline workflow: FTP the new binary to SD as `firmware.bin`, then
run `update local`.

### 11.1 Network OTA **[RESOLVED: in scope for v1; a second source alongside SD]**

`esp_https_ota` streams a firmware image straight into the inactive slot. The release
binary and a version marker are **plain files committed to the `otaRepo` repository**
(not GitHub *release assets* — this dodges the Releases-API rate limits and its
asset-download CDN redirects). Two forms:

- **`update ota`** — pull from the configured repo. Fetch
  `https://raw.githubusercontent.com/<otaRepo>/master/ota/version.txt`, parse the
  `major.minor.patch` it holds, and compare to the running `version`. If it is newer,
  stream `https://raw.githubusercontent.com/<otaRepo>/master/ota/firmware.bin` into
  the inactive slot; if not, report "already up to date" and stop.
- **`update <url>`** — flash an explicit image (undocumented; not shown in `help`). The
  URL may be `https://` (any HTTPS binary, no version check) or `tnfs://` (served by the
  M9 TNFS client — reported unavailable until that lands). No repo/version logic.

Notes:
- **TLS.** `raw.githubusercontent.com` serves repo files directly (HTTP 200, no redirect
  to a release-asset CDN), so certs validate against the ESP-IDF **`esp_crt_bundle`** CA
  roots with nothing host-pinned. Adds some TLS RAM pressure during the update.
- **`otaRepo`** (§7) is `owner/repo`, defaulting to this project's own repo so
  `update ota` works out of the box; the locked CLI set (§8.1) has no command to change
  it, so a fork points elsewhere by rebuilding with a different default (or `wipe`+reflash).
- **Release layout.** Cutting a release commits `ota/version.txt` (one `x.y.z` line, in
  sync with `include/version.h`) and `ota/firmware.bin` to `master`.
- **Offline.** Network OTA **supplements**, does not replace, the SD `firmware.bin` path in
  §11, which stays the primary/offline mechanism; the whole feature is gated on WiFi up.

**Build order:** SD-based OTA (§11) lands first since it's simplest; `update ota` /
`update <url>` build on it once networking is up. All share one OTA writer + verify +
set-boot-partition + reboot backend, so the network path is mostly the `esp_https_ota`
front end plus the version-marker check.

---

## 12. Startup Sequence

1. Console UART @115200; banner with version.
2. Init status + drive LED GPIOs (blink drives in sequence as a lamp test).
3. Init SD; print card type/size.
4. Load config from NVS (defaults if absent).
5. Mount configured drives (SD-backed only; `tnfs://` drives are deferred to step 9).
6. Start FDC timeout timer.
7. Init FDC UART at configured baud.
8. Init CLI; run `/autoexec.bat` if present; show prompt.
9. If WiFi enabled, connect → (on connect) start Telnet + FTP and mount any deferred
   `tnfs://` drives (§10.1); re-mount them on reconnect, drop to not-ready on disconnect.

---

## 13. Statistics & Diagnostics

In-RAM counters: `stat`, `read`, `writ`, `errs` (checksum), `tout` (timeouts); plus
formatted "last STAT/READ/WRIT/error" strings. `dump` hex-dumps the track buffer.
`clear` resets. Not persisted.

**Console log verbosity.** IDF's `ESP_LOG` output (WiFi/net/fdc/sd/ftp INFO, etc.) is
gated by a runtime level so the console is quiet in normal use and verbose only on
demand. The `log` command sets it — `log none|error|warn|info|debug|verbose` (with
`off`=none, `on`=info), bare `log` reports the current level — calling
`esp_log_level_set("*", level)` live. The level persists in NVS (`logLevel`, §7) and is
re-applied at boot: `app_main` drops the level to `warn` immediately (so the boot
subsystems are quiet by default), then applies the saved level once config loads. Default
is `warn` (warnings + errors still show; the printf banner always shows). The firmware is
built with `ESP_LOG` compiled in up to `info`, so `debug`/`verbose` need a debug build to
emit more.

---

## 14. Build / Flash / Monitor

PlatformIO is installed at `/opt/homebrew/bin/pio` (v6.2.0). From the project root:

```bash
pio run                      # build default env (devkit-espidf)
pio run -t upload            # build + flash over USB
pio device monitor -b 115200 # serial console
pio run -t upload -t monitor # flash then monitor
pio run -t menuconfig        # ESP-IDF sdkconfig editor
```

Board: `esp32doit-devkit-v1`. Partition table: `partitions.csv`. The single
`devkit-espidf` env (`framework = espidf`) is `default_envs` in `platformio.ini`.

---

## 15. Explicit Deltas From the Old Firmware

Decisions already leaning a certain way for v1 (owner may override):

1. Proper module split with headers — no implicit Arduino prototype merging.
2. 32-bit offsets/sizes for track math (old code used `int`).
3. Fix `mount`/`unmount` drive bounds (`0..MAX_DRIVE-1`).
4. FreeRTOS task separation for the latency-critical FDC path (§5.2, resolved).
5. Reconsider hardcoded FTP credentials (§9.3).
6. Optional NTP enabled so `time` works (§9.1).
7. Framework: PlatformIO `espressif32`, **pure ESP-IDF, no Arduino** (§3, resolved).

---

## 16. Open Questions for the Owner

All resolved:

- ~~Framework~~ — **RESOLVED: pure ESP-IDF, no Arduino** (§3).
- ~~Execution model~~ — **RESOLVED: FreeRTOS tasks** (§5.2).
- ~~NVS schema compatibility~~ — **RESOLVED: start clean, namespace `fdcsds`** (§7).
- ~~FTP credentials~~ — **RESOLVED: configurable in NVS, default `fdc`/`fdc`** (§9.4).
- ~~FTP library~~ — **RESOLVED: vendor a community IDF component** (§9.4).
- ~~Filename length limits~~ — **RESOLVED: 64 chars** (§7/§10).
- ~~CLI command set~~ — **RESOLVED: §8.1 locked for v1** (§8.1).
- ~~AP provisioning / web UI~~ — **RESOLVED: both deferred (out of v1 scope)** (§1).
- ~~GitHub OTA~~ — **RESOLVED: in scope for v1** (§11.1).
- ~~Directory wildcards~~ — **RESOLVED: `dir`/`ls <spec>` glob in scope** (§8.4).
- ~~Remote disk images (TNFS)~~ — **RESOLVED: mount from & `copy` to/from `tnfs://` in
  scope for v1** (§9.5 / §10.1 / §10.2). Remaining open risk is remote-read latency vs
  the FDC ~1 s timeout (§10.1), to validate on hardware.
