# CLAUDE.md — fdc-sds-pio

Working notes for AI assistance on this project.

## What this project is

A **from-scratch** rewrite of the ESP32 **FDC+ Serial Disk Server** under
**PlatformIO** with the native Espressif toolchain. It serves Altair 8800 FDC+ disk
images (files on a microSD card) to the FDC+ controller over a high-speed serial link.

## Source of truth

- **`DESIGN.md` is the spec. Build only from it.** It is a living draft that the owner
  edits. Do **not** port the old code structure; implement to the spec.
- The old Arduino firmware in **`../fdc-sds-esp32/`** (and its `../fdc-sds-esp32/DESIGN.md`)
  is **behavioral reference only** — read it to understand intended behavior, not to
  copy files.
- **Do not start writing `src/` implementation until the owner says the relevant part
  of `DESIGN.md` is settled.** Many sections are marked **[DECIDE]** and are still open.
  When a decision is unresolved and blocks work, ask.

## Reference material

- Protocol + hardware: <https://deramp.com/downloads/altair/hardware/fdc+/>
  (FDC+ Manual, `FDC Serial Server Protocol.txt`, schematics, Windows server, firmware
  source). The protocol and pin map are **wire contracts** — see DESIGN.md §2.1, §6.
- `SDCARD/` — reference disk images + `.bat` mount scripts (intended card contents).
  Standard Altair images are 337,664 B; 8 MB CP/M disks are 8,978,432 B = 1096 × 8192,
  which is why the track buffer must be ≥ 8192 B.

## Toolchain (macOS Apple Silicon)

- PlatformIO CLI installed via Homebrew: **`/opt/homebrew/bin/pio`** (v6.2.0).
  Ensure `/opt/homebrew/bin` is on `PATH`.
- Board: `esp32doit-devkit-v1`. Partition table: `partitions.csv` (dual-OTA + NVS).
- Framework is **RESOLVED (DESIGN.md §3): pure ESP-IDF, no Arduino.** `platformio.ini`
  has a single `env:devkit-espidf` (`framework = espidf`) as `default_envs`. Use IDF
  APIs directly (`driver/uart`, `esp_vfs_fat`+SDSPI, `esp_wifi`/`esp_netif`, `nvs`,
  `esp_ota_ops`, `esp_timer`); no Arduino libraries or Arduino-as-component.

Common commands (from project root):

```bash
pio run                        # build default env
pio run -t upload              # build + flash
pio device monitor -b 115200   # serial console
pio run -t clean               # clean
```

`pio run` will report "no source" until `src/` is implemented — that is expected at
this stage.

## Layout

See DESIGN.md §4. `src/` app code, `include/` shared headers, `lib/` vendored libs
(e.g. an FTP library), `test/` PlatformIO tests, `docs/` reference, `SDCARD/` card
contents. `.pio/`, `.vscode/`, build artifacts, and `sdkconfig` are git-ignored.

## Conventions

- Default git branch is **`master`** (never `main`).
- 32-bit (`uint32_t`/`size_t`/`off_t`) for disk offsets and sizes — never `int`.
- Real module boundaries with headers; no reliance on Arduino implicit prototype
  generation (the whole point of leaving the `.ino` world).
- Keep the pin map, protocol framing, and (if compatibility is chosen) the NVS schema
  identical to the contracts in DESIGN.md.

## Git attribution

- Commit trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`
- Only commit/push when the owner asks.
