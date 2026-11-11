---
name: altairsim-fdcplus
description: Simulate an FDC+ with altairsim on /dev/cu.usbserial-AL009KFH to drive the real ESP32 SDS FDC+ link — STAT traffic at a chosen baud, baud-mismatch / `baud auto` tests, checking `stats` on the ESP32 console. Use when testing FDC+ link behavior without real Altair hardware.
---

# Simulated FDC+ against the real ESP32

## Rig

| What | Where |
|---|---|
| ESP32 console + upload | `/dev/cu.usbserial-0001` (pinned in `platformio.ini`; flashing is pre-authorized) |
| ESP32 FDC+ link (UART2) | `/dev/cu.usbserial-AL009KFH` |
| Simulator | `altairsim` (`~/.local/bin/altairsim`, v1.3.6) |

`/dev/cu.usbserial-AB0NW409` is a different adapter — not the FDC+ link.

## Helpers (in this skill's directory)

- `fdcplus-sim.sh <baud>` — runs altairsim's `default` machine with `dsk0` swapped
  for an `fdcplus` board at port 08, wired to 9KFH, and an 8080 spinning on
  `IN 08h; JMP 0`. Runs until killed. Typical use:
  ```sh
  ./fdcplus-sim.sh 230400 > /tmp/.../sim.log 2>&1 & P=$!
  sleep 4; ./sds-cli.py stats; kill $P
  ```
- `sds-cli.py [--wait S] cmd...` — sends CLI commands to the ESP32 console and prints
  the replies (default 1.5 s per command; use `--wait 30 "baud auto"` for long ones).

## What the sim does (verified 2026-11-11)

- **A bare `fdcplus` board (no CPU, `altairsim -n`) sends nothing.** STAT traffic needs
  a running CPU. With the `IN 08h` loop, the ESP32 sees **~10 STAT/s**
  (`last STAT sel=255 map=...`).
- **Sim baud rates:** `set fdc0 baud=N` accepts the real FDC+ rates: **9600, 19200,
  38400, 57600, 76800, 230400, 403200, 460800**. 115200 is not an FDC+ rate. Any
  other value prints an error and **stays at the previous rate (403200 by default)**,
  and the `-x` chain keeps going. Always check the log or `show fdc0` after a `set`.
- **Wrong rate on the ESP32** shows up as `cmd-csum` / `cmd-tmo` (and sometimes
  `unknown`) climbing with `STAT` stuck at 0. `frame-err` stayed 0 in testing, so
  don't rely on it to spot a mismatch. Example at ESP32 403200: sim 57600 gave
  10 csum + 5 tmo in 4 s, and sim 460800 gave 4 tmo.

## Recipe by hand

```
altairsim default
  boards remove dsk0                    ; dcdd also sits at 08
  boards add fdcplus fdc0
  set fdc0 baud=230400                  ; see the rate list above
  connect fdc0:line serial:/dev/cu.usbserial-AL009KFH
  connect sio0:a null                   ; keep the console out of RUN
  deposit 0 db 08 c3 00 00              ; IN 08h ; JMP 0000
  run 0
```
`show fdc0` shows the board (port, drivetype 5..7, baud, connect), and `show serial`
lists the host ports and which one is in use. To change the rate, stop the sim and
start it again (the helper takes the baud as its argument).

Not yet verified here: booting a CP/M image over the link (e.g. the `cdbl` machine
with `fdcplus`) to generate READ/WRIT traffic. If you verify it, add it to this file.

## Gotchas

- **Only one program can hold a port at a time.** Kill the sim before running
  `tools/fdc-sim/fdcsim.py` on 9KFH, and close `pio device monitor` before
  `sds-cli.py`.
- **macOS pyserial:** never assign `ser.timeout` on an open port; it drops bytes.
  `sds-cli.py` sets it once.
- **The Mac adapter sends junk first.** `sds-cli.py` sends a CR and discards the
  reply before the real command.
- `clear` on the ESP32 before each measurement makes the deltas easy to read.
- An FDC-over-TCP client (`fdctcp`) makes the ESP32 ignore the UART, so make sure
  `stats` shows `interface: UART`.
- `pio run -t upload` uses 0001. Stop `sds-cli.py` and the monitor first; the sim on
  9KFH can keep running.

For one-off scripted STAT/READ/WRIT/loopback exchanges, with no CPU or polling,
`tools/fdc-sim/fdcsim.py --port /dev/cu.usbserial-AL009KFH --baud N ...` is the
lighter tool.
