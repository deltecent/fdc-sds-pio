#!/usr/bin/env python3
"""
serloop.py — host-only serial loopback baud sweep (no ESP32 involved).

Purpose: decide whether a USB-serial adapter can itself sustain each FDC+ baud,
independent of the ESP32. Jumper the adapter's OWN TX -> its OWN RX (a hardware
loopback on the adapter), then run this. It sends an incrementing pattern the size
of a real FDC+ track at every supported baud and reads it back, reporting how many
bytes returned and how many bytes differ.

  - All bauds OK            -> the adapter is fine; a failure on the wired ESP32
                              link at that baud is the firmware's (ESP32's) fault.
  - Fails at 403200/460800  -> the adapter itself cannot sustain those rates on
                              this host; that ceiling is the adapter, not the ESP32.

This is the cross-check the ESP32's own `loopback` command cannot give you: an
on-chip TX<->RX loopback shares one UART clock, so it passes even when the absolute
baud is wrong. Looping the adapter back tests the adapter's real clock + USB path.

Usage:
  ./serloop.py                         # default adapter, all bauds, 8194-byte frame
  ./serloop.py --port /dev/cu.usbserial-AB0NW409
  ./serloop.py --len 4096 --bauds 230400,403200,460800 --reps 3
"""
import argparse
import sys
import time

import serial

# Supported FDC+ bauds (DESIGN.md §6.1). 403200 is the default/target.
DEFAULT_BAUDS = [9600, 19200, 38400, 57600, 76800, 115200, 230400, 403200, 460800]
DEFAULT_PORT = "/dev/cu.usbserial-AB0NW409"
# A real 8 MB-image track is 8192 data bytes + a 2-byte checksum on the wire.
DEFAULT_LEN = 8194


def loop_once(ser: serial.Serial, pattern: bytes) -> tuple[bytes, float]:
    """Write the whole pattern, then read it back with a baud-scaled deadline."""
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    n = len(pattern)
    # Generous deadline: the on-wire time for n bytes (10 bits/byte) plus slack.
    budget = (n * 10.0) / ser.baudrate + 2.0
    t0 = time.time()
    ser.write(pattern)
    ser.flush()
    got = bytearray()
    deadline = t0 + budget
    while len(got) < n and time.time() < deadline:
        chunk = ser.read(n - len(got))
        if chunk:
            got.extend(chunk)
    return bytes(got), time.time() - t0


def count_mismatches(pattern: bytes, got: bytes) -> int:
    m = 0
    for i in range(len(pattern)):
        if i >= len(got) or got[i] != pattern[i]:
            m += 1
    return m


def main() -> int:
    ap = argparse.ArgumentParser(description="Host-only serial loopback baud sweep.")
    ap.add_argument("--port", default=DEFAULT_PORT, help="adapter device (TX jumpered to its own RX)")
    ap.add_argument("--len", type=int, default=DEFAULT_LEN, help="bytes per frame (default 8194 = one track)")
    ap.add_argument("--reps", type=int, default=3, help="frames per baud (default 3)")
    ap.add_argument("--bauds", help="comma-separated baud list (default: all supported)")
    args = ap.parse_args()

    bauds = ([int(b) for b in args.bauds.split(",")] if args.bauds else DEFAULT_BAUDS)
    pattern = bytes(i & 0xFF for i in range(args.len))

    print(f"serloop: {args.port}  {args.len}-byte frames x{args.reps}  "
          f"(jumper the adapter's OWN TX<->RX)\n")
    print(f"{'baud':>7}  {'result':<6}  {'best bytes':>12}  {'mism':>6}  {'KiB/s':>7}")
    print("-" * 48)

    any_fail = False
    for baud in bauds:
        try:
            ser = serial.Serial(args.port, baud, timeout=0.1)
        except serial.SerialException as e:
            print(f"{baud:7d}  OPEN-ERR  {e}")
            any_fail = True
            continue
        # settle after opening at a new divisor
        time.sleep(0.15)
        best_bytes = -1
        best_mism = args.len + 1
        best_rate = 0.0
        clean = 0
        for _ in range(args.reps):
            got, dt = loop_once(ser, pattern)
            mism = count_mismatches(pattern, got)
            if mism == 0 and len(got) == args.len:
                clean += 1
            # "best" = most bytes back, then fewest mismatches
            if len(got) > best_bytes or (len(got) == best_bytes and mism < best_mism):
                best_bytes, best_mism = len(got), mism
                best_rate = (len(got) / dt / 1024.0) if dt > 0 else 0.0
        ser.close()
        ok = (clean == args.reps)
        any_fail = any_fail or not ok
        result = "OK" if ok else (f"{clean}/{args.reps}" if clean else "FAIL")
        print(f"{baud:7d}  {result:<6}  {best_bytes:7d}/{args.len:<4}  "
              f"{best_mism:6d}  {best_rate:7.0f}")

    print("\n" + ("some bauds FAILED — see above" if any_fail
                  else "all bauds clean on this adapter"))
    return 1 if any_fail else 0


if __name__ == "__main__":
    sys.exit(main())
