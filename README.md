# fdc-sds-pio

FDC+ Serial Disk Server for the Altair 8800 — a from-scratch ESP32 rewrite built with
**PlatformIO** and the native Espressif toolchain.

See **[DESIGN.md](DESIGN.md)** for the full specification (the source of truth) and
**[CLAUDE.md](CLAUDE.md)** for working notes and toolchain setup.

## Quick start

```bash
pio run                        # build
pio run -t upload              # flash over USB
pio device monitor -b 115200   # serial console
```

- Board: DOIT ESP32 DEVKIT V1 (4 MB flash).
- Reference disk images live in `SDCARD/`.
- Protocol & hardware reference: <https://deramp.com/downloads/altair/hardware/fdc+/>

> Status: design/scaffold stage. `src/` is implemented from `DESIGN.md`.
