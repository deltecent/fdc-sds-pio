# macOS "garbage on connect" — Apple's `AppleUSBSLCOM` CP210x driver replays a stale buffer

**TL;DR** — On macOS, opening the ESP32's USB serial console *after* the board has
already booted can flood the terminal with a short, repeating fragment of the board's
last log line until you press a key. **This is not a firmware bug.** The device transmits
every byte exactly once. The repeat is produced by Apple's built-in CP210x USB-serial
driver (`AppleUSBSLCOM`) re-delivering a stale buffer window when a terminal drains the
port with small reads and never writes back. It is harmless — the first keystroke stops
it — and it also affected the previous Arduino firmware.

If you just want it to go away: use **`pio device monitor`** (it resets the board on
connect, so there is no stale buffer to replay), or **press any key** immediately after
connecting.

---

## Affected setup

| | |
|---|---|
| Adapter | Silicon Labs **CP2102** USB-to-UART bridge (VID `0x10C4`, PID `0xEA60`) — the USB chip on the DOIT ESP32 DEVKIT V1 |
| Host OS | macOS (observed on **26.6.2**, build 25G83, Apple Silicon; older releases too) |
| Driver | **`AppleUSBSLCOM`** — `com.apple.DriverKit-AppleUSBSLCOM`, Apple's built-in DriverKit system extension (no third-party driver installed) |
| Terminals that trip it | `screen`, `tio`, Serial.app (Decisive Tactics) — anything that reads the port in small chunks without writing |
| Terminals that don't | `pio device monitor` / raw pyserial (they assert DTR/RTS → reset to a clean boot, and drain with large reads) |

Note: Silicon Labs' own VCP driver is not a fix on Apple Silicon — the newest release is
6.0.2 (October 2021) with no stated arm64/modern-macOS support, and macOS already ships
`AppleUSBSLCOM` for this chip.

## Symptom

Connecting a terminal to the console (`/dev/cu.usbserial-XXXX` @115200) after the
firmware has booted and gone idle fills the window with a repeating tail of the last log
line, e.g.:

```
nfs.mitsaltair.com/pub/CPM22-8MB-56K.DSK' opened read-only (write denied: ESP_FAIL)
e denied: ESP_FAIL)
e denied: ESP_FAIL)
e denied: ESP_FAIL)
...            (continues, megabytes of it, until a key is pressed)
```

Only the **tail** of the line repeats — never the full line — and it stops the instant
the terminal sends any byte to the device.

## Root cause

The CP2102 buffers the ESP32's UART output in its on-chip RX FIFO whether or not a USB
host has the port open. When a terminal connects, `AppleUSBSLCOM` hands over whatever is
in that FIFO (the board's last-transmitted bytes). Under a **small-read, no-write** drain
pattern, the driver then re-presents the *same* buffer window over and over instead of
advancing/clearing its read pointer, generating an endless stream from a few stale bytes.
A single host **write** resyncs the USB bulk endpoints and breaks the loop — which is why
any keystroke stops it.

This matches independent reports of `AppleUSBSLCOM` misbehaving (see references).

## Why it is NOT the firmware

Proven three ways on the actual hardware:

1. **The wire carries one copy.** A clean capture of a fresh boot shows the full log
   (~3 KB) transmitted exactly once, then silence.
2. **The repeat count is host-side.** Draining the port with one large read yields the
   stale window a single time and then nothing. Draining with small reads yields the same
   ~20-byte window repeated into the megabytes — *the firmware output is identical; only
   the host read pattern differs.* In a 6-second small-read capture the fragment repeated
   to **1,408,542 bytes** while the underlying full string appeared **exactly once**.
3. **No re-emit path exists in firmware.** The console task only *reads*; all output goes
   once through `stdout` → `uart_vfs` → the UART TX ring. Nothing re-prints a log line.

## Reproduction

Minimal, self-contained (needs Python 3 + `pyserial`; set `PORT` to your device node):

```python
#!/usr/bin/env python3
# Reproduce the AppleUSBSLCOM stale-buffer replay.
# 1) reset the board once to get a known fresh boot + final log line, then close;
# 2) reconnect the way `screen` does (raw termios, DTR/RTS untouched) and read in
#    small chunks with no writes. The board's last log-line tail repeats endlessly.
import os, time, termios, select, serial

PORT = "/dev/cu.usbserial-0001"; BAUD = 115200
MARK = b"write denied"          # any substring of your board's final log line

def reset_once():                # pulse EN via DTR/RTS, drain boot, then close
    s = serial.Serial(); s.port = PORT; s.baudrate = BAUD; s.timeout = 0.05
    s.dtr = False; s.rts = True; s.open()
    s.setRTS(True); time.sleep(0.1); s.setRTS(False)
    last = t0 = time.time()
    while time.time() - last < 3 and time.time() - t0 < 20:
        if s.in_waiting: s.read(s.in_waiting); last = time.time()
    s.close()

def raw_open():                  # os.open like a cu. terminal: no modem-line change
    fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd); a[0] = a[1] = a[3] = 0
    a[2] = termios.CREAD | termios.CLOCAL | termios.CS8
    a[4] = a[5] = termios.B115200
    a[6][termios.VMIN] = 0; a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a); return fd

reset_once(); time.sleep(3)
fd = raw_open(); t0 = time.time(); buf = bytearray()
while time.time() - t0 < 6:      # small reads, NO writes
    r, _, _ = select.select([fd], [], [], 0.02)
    if r:
        b = os.read(fd, 8)
        if b: buf.extend(b)
os.close(fd)
print(f"read {len(buf)} bytes; '{MARK.decode()}' occurs {buf.count(MARK)}x")
# Expected: hundreds of KB / MB read, but MARK occurs exactly 1x -> the driver
# invented every repeat from one transmission. Sending any byte stops it.
```

Contrast: replace `os.read(fd, 8)` with a single large `os.read(fd, 4096)` and the flood
does not occur — the FIFO drains once. That difference is entirely inside the driver.

## Workarounds

- **`pio device monitor`** — asserts DTR/RTS on open, resetting the ESP32 to a clean boot;
  there is no stale buffer to replay.
- **Press any key** right after connecting — the write breaks the driver's read loop.
- A terminal that flushes input (`tcflush`/`TCIFLUSH`) or does one large read on connect
  will not show the flood. Plain `screen`/`tio`/Serial.app do not.

There is no firmware change that helps: the firmware cannot stop the host driver from
re-reading its own buffer, and it already sends each byte once.

## Reporting to Apple (Feedback Assistant)

The only real fix is in Apple's driver. A ready-to-paste report:

> **Title:** AppleUSBSLCOM re-delivers a stale RX buffer window on connect (CP210x)
>
> **Area:** Drivers / USB / Serial (DriverKit)
>
> **Summary:** When a terminal opens a CP2102 (`AppleUSBSLCOM`, VID 0x10C4 / PID 0xEA60)
> serial port on a device that has been transmitting while the port was closed, and then
> reads in small chunks without writing, the driver repeatedly re-delivers the same small
> window of previously-buffered bytes instead of advancing/clearing its read position.
> The result is an unbounded stream (megabytes) generated from a few stale bytes. Any
> write to the port resyncs and stops it.
>
> **Steps to reproduce:** (attach the script above)
> 1. Attach a CP2102 USB-UART bridge whose UART side is emitting a line of text, with no
>    host holding the port open (data accumulates in the chip's RX FIFO).
> 2. Open `/dev/cu.usbserial-*` at 115200, raw termios, without changing DTR/RTS.
> 3. Read the port in small chunks (e.g. 8 bytes) and perform no writes.
>
> **Expected:** the buffered bytes are delivered once, then the port is quiet.
> **Actual:** the last ~20-byte window repeats endlessly (1.4 MB in 6 s in testing) while
> the underlying data was transmitted only once; a single write to the port stops it.
>
> **Notes:** Reading the port with one large read instead of many small reads delivers the
> data once and does not reproduce, which localizes the fault to the driver's small-read
> path, not the device. Reproduced on macOS 26.6.2 (25G83), Apple Silicon.

Cross-reference the Feedback ID here once filed so other users can dupe it.

## References

- PlatformIO community — [`com.apple.DriverKit-AppleUSBSLCOM` causing massive problems](https://community.platformio.org/t/com-apple-driverkit-appleusbslcom-causing-massive-problems/39685)
- Apple Developer Forums — [USB Serial DriverKit Driver Problems](https://developer.apple.com/forums/thread/124613)
- Apple — [SerialDriverKit documentation](https://developer.apple.com/documentation/serialdriverkit) (for a replacement driver, if pursued)
- Silicon Labs — [CP210x VCP Mac driver release notes](https://www.silabs.com/documents/public/release-notes/Mac_OSX_VCP_Driver_Release_Notes.txt) (latest 6.0.2, 2021; no Apple Silicon support)
