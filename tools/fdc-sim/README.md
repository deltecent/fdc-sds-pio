# fdc-sim — host FDC+ simulator

`fdcsim.py` plays the role of the Altair FDC over the serial link so the ESP32 firmware
(the passive disk **server**) can be exercised without real Altair hardware. It is the
primary verification gate for milestone **M4** and is reused as a regression at every
later stop (DESIGN.md §6, build plan M4/M5/M6).

It speaks the exact wire contract in `include/protocol.h`: 10-byte command/response
blocks (4-byte ASCII mnemonic + two little-endian words + a 16-bit sum checksum), and
raw track payloads followed by a 16-bit checksum.

## Requirements

- Python 3.8+ and `pyserial` (`pip install pyserial`).
- A **second** USB-serial adapter for the FDC+ port (the first one is the ESP32 console
  at `/dev/cu.usbserial-0001`). The FDC+ adapter here is `/dev/cu.usbserial-AB0NW409`.

## Wiring (test rig, DESIGN.md §2.1 + build plan)

| Adapter pin | ESP32 pin | Note |
|---|---|---|
| adapter **TX** → | **GPIO16** (UART2 RX) | server receives commands |
| adapter **RX** ← | **GPIO17** (UART2 TX) | server sends responses |
| **GND** ↔ | **GND** | common ground required |

ESP32 UART2 is **3.3 V TTL**. If the adapter is true RS-232, add a level shifter or use
a 3.3 V TTL adapter — do not connect RS-232 levels to GPIO16/17.

## Bring-up order (M4 verification)

1. **Host loopback** — jumper the *adapter's own* TX↔RX (not the ESP32) and run
   `loopback`. Proves the host adapter + baud before involving the firmware.
   ```
   ./fdcsim.py --port /dev/cu.usbserial-AB0NW409 --baud 403200 loopback
   ```
   (The firmware has its own `loopback` CLI command that tests the ESP32 side with a
   TX↔RX jumper on GPIO16↔GPIO17.)

2. **STAT** — wire adapter↔ESP32 as above. Mount a drive on the server
   (`mount 0 CPM22-8MB-56K.DSK` on the console), then:
   ```
   ./fdcsim.py --port ... stat          # expect mount bitmap 0x0001 (drive 0)
   ```

3. **READ** — verify bytes match the local image:
   ```
   ./fdcsim.py --port ... read 0 0 --len 8192 --expect ../../SDCARD/CPM22-8MB-56K.DSK
   ```

4. **WRIT** — write one track and confirm `WSTA OK`:
   ```
   ./fdcsim.py --port ... writ 0 5 --len 8192 --data ../../SDCARD/CPM22-8MB-56K.DSK
   ```

5. **Full round-trip** — read (and optionally write-back) every track:
   ```
   ./fdcsim.py --port ... verify ../../SDCARD/CPM22-8MB-56K.DSK --write
   ```

6. **Baud ramp** — set `baud 230400` (or `403200`) on the console, restart `fdcsim`
   with the matching `--baud`, and re-run `verify`. Record the ceiling that stays clean.

Add `-v` to any command to dump the TX/RX bytes.

## Track length

The server is geometry-agnostic — it uses whatever transfer length the FDC sends. The
8 MB CP/M images (`CPM22-8MB-56K.DSK`, blank/Games/Zork) are **1096 tracks × 8192
bytes**, so `--len 8192` / `--track-len 8192` (the defaults). Standard 8" floppy images
(337,664 bytes) use a different track length — pass the right `--track-len` for those.

## Notes

- A READ to an unmounted or out-of-range drive gets **no reply** (the server is a passive
  responder); `fdcsim` reports it as a short read / timeout — that is expected.
- A WRIT with a deliberately corrupted data checksum yields `WSTA CHECKSUM_ERR` (code 2),
  which the server counts under `csum-err` in `stats`.
