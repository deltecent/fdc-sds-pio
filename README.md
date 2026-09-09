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

## Hardware

| Signal | ESP32 pin | Notes |
|---|---|---|
| FDC+ link | UART2 — RX GPIO16, TX GPIO17 | 3.3 V TTL; to the FDC+ serial port |
| microSD | VSPI — CS 5, CLK 18, MISO 19, MOSI 23 | FAT-formatted; images live in the root |
| Status LED | GPIO2 | FDC activity |
| Drive LEDs | GPIO27 / 14 / 12 / 13 | drives 0–3 |
| Console | UART0 / USB @115200 | banner, CLI, flashing |

Up to four disk images (drives 0–3) are served over the FDC+ serial link. The link baud
is configurable (`baud`); 403200 is the FDC+ high-speed rate.

## Console

Reach the CLI two ways, both sharing the same command set:

- **Serial** — `pio device monitor -b 115200` (USB/UART0).
- **Network** — `nc <host>.local 23` (raw TCP, once WiFi is up). `telnet` also works;
  stray IAC bytes are dropped (this is not a Telnet server).

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
| `dir` / `ls` `[glob]` | List SD files (e.g. `dir *.dsk`) |
| `type` / `cat` `<file>` | Print a text file |
| `copy` / `cp` `<src> <dst>` | Copy a file |
| `rename` / `mv` `<old> <new>` | Rename a file |
| `delete` / `rm` `<file>` | Delete a file |
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

`exec <name>` runs a batch file from the SD root, one command per line. `<name>` is
tried as given and then with a `.bat` suffix, so `exec cpm3` runs `cpm3.bat`. Blank
lines are skipped and lines starting with `#` are echoed as comments. Typing an unknown
command that names a batch file (or has a matching `<name>.bat`) auto-runs it, so
`cpm3` at the prompt is the same as `exec cpm3.bat`.

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
```

On connect the device advertises `<hostname>.local` (mDNS), syncs the clock over NTP,
and starts the raw-TCP console (:23) and an FTP server (default login `fdc` / `fdc`,
changeable with `ftpuser` / `ftppass`). Upload `.dsk` images straight to the SD card
over FTP.

## Firmware update (OTA)

- `update` — report the running version, any `/firmware.bin` staged on the SD card, and
  (when WiFi is up) the version offered by the update repo. Installs nothing.
- `update local` — flash `/firmware.bin` from the SD card into the standby slot and
  reboot into it.
- `update ota` — fetch the repo's `ota/version.txt`, and if it is newer than the running
  build, stream in `ota/firmware.bin` over HTTPS and reboot.

The device carries two app slots (dual-OTA partition table), so a failed or interrupted
update leaves the running firmware intact.

> Built from `DESIGN.md`. Milestones M0–M8 are implemented; remote disk images over
> TNFS (mount/copy) are the remaining M9 work.
