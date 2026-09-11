# fdc-sds-pio

FDC+ Serial Disk Server for the Altair 8800 — a from-scratch ESP32 rewrite built with
**PlatformIO** and the native Espressif toolchain (pure ESP-IDF, no Arduino).

See **[DESIGN.md](DESIGN.md)** for the full specification (the source of truth) and
**[CLAUDE.md](CLAUDE.md)** for working notes and toolchain setup.

## Quick start

```bash
pio run                        # build
pio run -t upload              # flash over USB
pio device monitor -b 115200   # serial console
```

- Board: DOIT ESP32 DEVKIT V1 (4 MB flash).
- Reference disk images and batch files live in `SDCARD/` (copy them to a FAT microSD).
- Protocol & hardware reference: <https://deramp.com/downloads/altair/hardware/fdc+/>
- Prebuilt binaries (no toolchain needed): **[Releases](https://github.com/deltecent/fdc-sds-pio/releases/latest)**
  — each tag attaches `merged.bin` (full flash, offset `0x0`) and `firmware.bin` (app / OTA
  image). Current release: **[1.0.4](https://github.com/deltecent/fdc-sds-pio/releases/tag/1.0.4)**.

## What's different from the Arduino firmware

This is a ground-up rewrite of the original
[Arduino `fdc-sds-esp32`](https://github.com/deltecent/fdc-sds-esp32) firmware (which
ended at 0.25). It keeps the same FDC+ wire protocol, pin map, and DOS-like command style,
so it drops in where the old one was — but the foundation and the feature set have moved on.

**Platform & timing**

- **Native ESP-IDF, built with PlatformIO** — no Arduino IDE, board manager, or
  third-party Arduino libraries (SimpleCLI, ESP Telnet, SimpleFTPServer). The build is a
  reproducible one-line `pio run`.
- **FreeRTOS tasks instead of one `loop()`.** The FDC+ serial task is pinned to core 1 and
  isolated from WiFi, the CLI, and FTP, so the FDC's ~1 s command timing holds even during
  network traffic or a large `copy` — the old single cooperative loop had to pump the FDC
  by hand to avoid stalls.
- **32-bit offsets/sizes** throughout (the old code used `int`), and real module boundaries
  with headers.

**Firmware updates**

- **Over-the-network OTA from GitHub** (`update ota`, HTTPS) in addition to SD-card updates
  (`update local`). The old firmware could only flash `/update.bin` from the SD card.
- **Dual-OTA, rollback-safe** — a failed or interrupted update never overwrites the running
  firmware. The version is read from the image itself, so you can roll back or re-flash a
  slot on purpose.
- A **prebuilt merged image and a bundled cross-platform flasher** (Python, no toolchain)
  for a new or blank board.

**Networking**

- **mDNS** — reach the board at `<hostname>.local` instead of chasing its IP.
- **Timezone-aware clock** (`tz`) over NTP; the old firmware's NTP was UTC-only.
- **Configurable FTP credentials** (`ftpuser` / `ftppass`) instead of a hardcoded
  `fdc` / `fdc`, and WPA2 is required whenever a password is set.
- **Remote disk images** — mount and `copy` over **TNFS**, and `copy` from an `http(s)://`
  URL. Neither existed before.

**Storage & CLI**

- **Subdirectories** on the SD card — `mkdir` / `rmdir`, `dir` descends into folders, and
  disk-set folders are self-contained and relocatable (a batch file runs from its own
  directory). The old firmware was flat-root only.
- **`diag`** one-shot status report, and FDC+ statistics split by transaction source.

Moving a board from the Arduino firmware to this one is a one-time USB flash (the partition
table and NVS format differ). See the wiki
[Firmware](https://github.com/deltecent/fdc-sds-pio/wiki/Firmware) page.

## Hardware

| Signal | ESP32 pin | Notes |
|---|---|---|
| FDC+ link | UART2 — RX GPIO16, TX GPIO17 | 3.3 V TTL; to the FDC+ serial port |
| microSD | VSPI — CS 5, CLK 18, MISO 19, MOSI 23 | FAT-formatted; disk sets in per-set folders (subdirectories supported) |
| Status LED | GPIO2 | FDC activity |
| Drive LEDs | GPIO27 / 14 / 12 / 13 | drives 0–3 |
| Console | UART0 / USB @115200 | banner, CLI, flashing |

Up to four disk images (drives 0–3) are served over the FDC+ serial link. The link baud
is configurable (`baud`); 403200 is the FDC+ high-speed rate.

> **Swapping the microSD card:** always `reboot` (or power-cycle) after changing the
> card. The card is mounted once at boot and there's no card-detect line, so the firmware
> can't tell that a card was pulled — it keeps using the first card's cached filesystem
> layout. Reading from a different card returns wrong data, and **writing to it (a disk
> write, `copy`, or FTP upload) can corrupt the new card's filesystem.** Reboot first, and
> the new card is mounted cleanly.

## Console

Reach the CLI two ways, both sharing the same command set:

- **Serial** — `pio device monitor -b 115200` (USB/UART0).
- **Network** — `telnet <host>.local` (port 23, once WiFi is up). It's a minimal
  Telnet server: it negotiates character mode with server-side echo, so typing and
  backspace work. `nc <host>.local 23` also works — it skips negotiation, so it relies
  on its own local echo (no double echo) and lacks server-driven line editing.

Commands accept unique prefixes (`ver` → `version`) and the aliases shown by `help`.
The prompt is the host name; a leading `* ` means the config has unsaved edits.

> **macOS users:** if the console floods with a repeating fragment of the last log line
> when you connect after the board has booted, that is a bug in Apple's built-in CP210x
> driver (`AppleUSBSLCOM`), not the firmware — press any key or use `pio device monitor`
> to avoid it. Details and an Apple bug-report template:
> [docs/macos-serial-flood.md](docs/macos-serial-flood.md).

### Commands

| Command | Purpose |
|---|---|
| `help` / `?` | List commands (or `help <cmd>`) |
| `version` | Firmware version |
| `dir` / `ls` `[glob\|subdir]` | List SD files (e.g. `dir *.dsk`); descends into a subdirectory (`dir cpm`, `dir cpm/*.dsk`) |
| `type` / `cat` `<file>` | Print a text file |
| `copy` / `cp` `<src> <dst>` | Copy a file; each endpoint may be an SD name or a `tnfs://` URL, and `<src>` may also be an `http(s)://` URL |
| `rename` / `mv` `<old> <new>` | Rename a file |
| `delete` / `rm` `<file>` | Delete a file |
| `mkdir` / `md` `<dir>` | Create a directory |
| `rmdir` / `rd` `<dir>` | Remove an empty directory |
| `mount` `[<drive> <file>]` | Show the mount table, or mount an image |
| `unmount` / `umount` `<drive>` | Unmount a drive |
| `dump` `[drive [track [len]]]` | Hex-dump a track buffer |
| `exec` / `run` `<file>` | Run a batch file of commands (§ below) |
| `baud` `[rate]` | Show/set the FDC+ link baud |
| `stats` / `clear` | Show / zero FDC+ statistics |
| `loopback` / `lb` | FDC+ serial loopback self-test |
| `wifi` `[on\|off]` | Show / enable / disable WiFi |
| `ssid` / `pass` | Set WiFi credentials |
| `hostname` `<name>` | Set the device/host name |
| `ftpuser` / `ftppass` | Set FTP credentials |
| `time` / `date`, `tz` | Show the clock; set the timezone (`tz ?` lists US zones) |
| `update` `[local\|ota]` | Firmware update (§ below) |
| `save` / `write`, `wipe` | Persist config to NVS / erase it and reload defaults |
| `logout` / `exit` | Disconnect a network client |
| `reboot` | Clean shutdown and restart |

Config edits stay in RAM until `save`. Saved settings — baud, WiFi, host name, FTP
credentials, timezone, and the four mounted drives — reload on the next boot.

## Batch files

A batch file holds one command per line; blank lines are skipped and lines starting with
`#` are echoed as comments. **Just type its name to run it** — an entered command that
isn't built in but names a batch file runs automatically. The name is tried as given and
then with a `.bat` suffix, so `cpm3` runs `cpm3.bat` and `basic/basic5` runs
`basic/basic5.bat`. `exec <name>` (alias `run`) does the same thing explicitly, which is
mainly useful for a batch file whose name has no `.bat` extension.

A batch **runs from its own folder**. `basic/basic5` runs the file, and while it
runs, bare names inside it resolve relative to the batch's directory — so `basic5.bat`
in `basic/` mounts an image sitting next to it with just `mount 0 "Disk BASIC 5.0.dsk"`
(no `basic/` prefix). That makes a disk-set folder **self-contained and relocatable**:
drop `basic/` anywhere on the card
and its batch still works. Nested batches stack (an inner batch resolves under its
parent's folder), and a `mount` made inside a batch is stored root-relative so it
re-mounts correctly at boot. A batch can't climb out of its folder — `..` is rejected —
and the **interactive prompt always runs at the SD root** (there's no `cd`; a working
directory exists only while a batch is running).

If `/autoexec.bat` exists it runs once at boot, after the drives configured in NVS are
mounted and before the first prompt — a convenient place to mount a disk set.

```
# cpm3.bat — mount a CP/M 3 disk set
mount 0 cpm3_v1.0_56K_disk1.dsk
mount 1 cpm3_v1.0_56k_disk2.dsk
mount 2 cpm3_v1.0_56k_build.dsk
umount 3
```

## Networking

Set credentials and enable WiFi, then `save` and `reboot`:

```
ssid MyNetwork
pass MySecret
wifi on
save
reboot
```

On connect the device advertises `<hostname>.local` (mDNS), syncs the clock over NTP,
and starts the Telnet console (:23) and an FTP server (default login `fdc` / `fdc`,
changeable with `ftpuser` / `ftppass`). Upload `.dsk` images straight to the SD card
over FTP.

Once WiFi is up, `copy` can also stage images without a separate client:

```
copy tnfs://192.168.1.10/disks/GAMES.DSK GAMES.DSK        # pull from a TNFS server
copy https://example.com/disks/CPM22.DSK CPM22.DSK        # pull from a web server
```

A `tnfs://` endpoint works as either the source or the destination; an `http(s)://` URL
is a source only (HTTP has no upload path), and `https://` is verified against the
bundled CA roots.

## Firmware update (OTA)

- `update` — report the running version, any `/firmware.bin` staged on the SD card, and
  (when WiFi is up) the version offered by the update repo. Installs nothing.
- `update local` — flash `/firmware.bin` from the SD card into the standby slot and
  reboot into it.
- `update ota` — stream in the repo's `ota/firmware.bin` over HTTPS and reboot. It reports
  the version embedded in that image against the running build (newer, older, or the same)
  and then installs it either way — so you can roll back or re-flash the current version on
  purpose; it is never blocked for "already up to date".

The device carries two app slots (dual-OTA partition table), so a failed or interrupted
update leaves the running firmware intact.

> Built from `DESIGN.md`. The core server (M0–M8) is implemented, along with remote disk
> images over TNFS (mount + `copy`) and `http(s)://` `copy` sources; on-hardware
> validation of remote-read timing against the FDC ~1 s timeout is ongoing.
