#!/usr/bin/env python3
"""
fdcsim.py — host-side FDC+ simulator for the FDC+ Serial Disk Server (DESIGN.md §6).

This plays the role of the Altair FDC: it *initiates* every transaction over the serial
link while the ESP32 firmware acts as the passive disk server. It is the primary M4
verification gate — the same wire contract the firmware implements (protocol.h), mirrored
here in Python.

Wire contract (little-endian throughout):
  * Command / response block = 10 bytes: 4-byte ASCII mnemonic + Word1 + Word2 + a
    16-bit checksum = plain sum of the first 8 bytes.
  * STAT: we send drive+head in Word1, track in Word2; server replies "STAT" with the
    mounted-drive bitmap in Word2 (bit n = drive n mounted). Response code is ignored.
  * READ: Word1 = (drive<<12)|track, Word2 = length. Server replies with <length> data
    bytes directly followed by a 16-bit checksum — NO 10-byte header.
  * WRIT: same Word1/Word2. Server replies "WRIT" OK(0)/NOT_READY(1); if OK we send
    <length> data bytes + checksum, then the server replies "WSTA" with the final code
    (0 OK / 2 checksum error / 3 write error).

Typical use (FDC+ port on a second USB adapter, common ground with the ESP32):
    ./fdcsim.py --port /dev/cu.usbserial-AB0NW409 --baud 403200 stat
    ./fdcsim.py --port ... read 0 0 --len 8192 --expect ../../SDCARD/CPM22-8MB-56K.DSK
    ./fdcsim.py --port ... verify ../../SDCARD/CPM22-8MB-56K.DSK --write

Requires pyserial (`pip install pyserial`).
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("fdcsim: pyserial is required — `pip install pyserial`")

BLOCK_LEN = 10
CMD_LEN = 4
CKSUM_LEN = 2

RESP = {0: "OK", 1: "NOT_READY", 2: "CHECKSUM_ERR", 3: "WRITE_ERR"}


def checksum16(data: bytes) -> int:
    """16-bit sum of the given bytes (DESIGN.md §6.2)."""
    return sum(data) & 0xFFFF


def le16(value: int) -> bytes:
    return bytes((value & 0xFF, (value >> 8) & 0xFF))


def rd16(data: bytes, off: int) -> int:
    return data[off] | (data[off + 1] << 8)


def build_block(mnemonic: str, word1: int, word2: int) -> bytes:
    """Assemble a 10-byte command block with its trailing checksum."""
    body = mnemonic.encode("ascii")[:CMD_LEN].ljust(CMD_LEN, b"\x00")
    body += le16(word1) + le16(word2)
    return body + le16(checksum16(body))


def read_encode(drive: int, track: int) -> int:
    """READ/WRIT Word1: drive in the high nibble, track in the low 12 bits (§6.3)."""
    return ((drive & 0xF) << 12) | (track & 0x0FFF)


class Link:
    """Thin serial wrapper with an exact-length, deadline-based reader."""

    def __init__(self, port: str, baud: int, verbose: bool = False):
        # A generous inter-byte timeout; we impose our own overall deadlines below.
        self.ser = serial.Serial(port, baud, timeout=0.2)
        self.verbose = verbose

    def close(self):
        self.ser.close()

    def flush_input(self):
        self.ser.reset_input_buffer()

    def send(self, data: bytes):
        if self.verbose:
            print(f"  TX {len(data)}B: {data[:16].hex()}{'...' if len(data) > 16 else ''}")
        self.ser.write(data)
        self.ser.flush()

    def recv(self, need: int, timeout: float) -> bytes:
        """Read exactly `need` bytes or return what arrived before `timeout`."""
        buf = bytearray()
        deadline = time.time() + timeout
        while len(buf) < need:
            remaining = deadline - time.time()
            if remaining <= 0:
                break
            self.ser.timeout = min(0.2, remaining)
            chunk = self.ser.read(need - len(buf))
            if chunk:
                buf.extend(chunk)
        if self.verbose and buf:
            print(f"  RX {len(buf)}B: {bytes(buf[:16]).hex()}{'...' if len(buf) > 16 else ''}")
        return bytes(buf)

    def recv_block(self, timeout: float = 1.0):
        """Read a 10-byte response block; return (mnemonic, code, data) or None."""
        blk = self.recv(BLOCK_LEN, timeout)
        if len(blk) != BLOCK_LEN:
            return None
        if checksum16(blk[:8]) != rd16(blk, 8):
            print(f"  ! response checksum bad: {blk.hex()}")
            return None
        return blk[:CMD_LEN].decode("latin-1"), rd16(blk, 4), rd16(blk, 6)


# ---- subcommands -----------------------------------------------------------


def cmd_loopback(link: Link, args) -> int:
    """Host-side self-test: jumper THIS adapter's TX<->RX and verify the byte path."""
    n = args.len
    pattern = bytes(i & 0xFF for i in range(n))
    link.flush_input()
    link.send(pattern)
    got = link.recv(n, timeout=2.0)
    if got == pattern:
        print(f"loopback OK: {n}/{n} bytes matched at {args.baud} baud")
        return 0
    mism = sum(1 for i in range(n) if i >= len(got) or got[i] != pattern[i])
    print(f"loopback FAILED: {len(got)}/{n} received, {mism} mismatch(es)")
    print("  (jumper this adapter's TX<->RX; this tests the host side, not the ESP32)")
    return 1


def do_stat(link: Link, drive=0xFF, head=0, track=0):
    """Send one STAT and return the mounted-drive bitmap, or None on no/bad reply."""
    word1 = (drive & 0xFF) | ((0x01 if head else 0x00) << 8)
    link.flush_input()
    link.send(build_block("STAT", word1, track))
    resp = link.recv_block(timeout=1.0)
    if resp is None:
        return None
    mnem, _code, bitmap = resp
    if mnem != "STAT":
        print(f"  ! expected STAT reply, got {mnem!r}")
        return None
    return bitmap


def cmd_stat(link: Link, args) -> int:
    bitmap = do_stat(link, drive=args.drive, head=1)
    if bitmap is None:
        print("stat: no response (check wiring, baud, and that the server is running)")
        return 1
    mounted = [d for d in range(16) if bitmap & (1 << d)]
    print(f"STAT ok: mount bitmap 0x{bitmap:04x}")
    print(f"  mounted drives: {mounted if mounted else 'none'}")
    return 0


def do_read(link: Link, drive: int, track: int, length: int):
    """Send READ; return the track payload bytes, or None on timeout/bad checksum."""
    link.flush_input()
    link.send(build_block("READ", read_encode(drive, track), length))
    payload = link.recv(length + CKSUM_LEN, timeout=7.0)
    if len(payload) != length + CKSUM_LEN:
        return None, f"short read ({len(payload)}/{length + CKSUM_LEN} bytes)"
    data, ck = payload[:length], rd16(payload, length)
    if checksum16(data) != ck:
        return None, f"data checksum bad (got 0x{ck:04x}, want 0x{checksum16(data):04x})"
    return data, None


def cmd_read(link: Link, args) -> int:
    data, err = do_read(link, args.drive, args.track, args.len)
    if err:
        print(f"read: {err}")
        return 1
    print(f"read ok: drive {args.drive} track {args.track}, {len(data)} bytes")
    print("  " + data[:16].hex() + (" ..." if len(data) > 16 else ""))
    if args.out:
        with open(args.out, "wb") as f:
            f.write(data)
        print(f"  wrote payload to {args.out}")
    if args.expect:
        with open(args.expect, "rb") as f:
            f.seek(args.track * args.len)
            want = f.read(args.len)
        if want == data:
            print("  MATCHES expected image bytes")
        else:
            first = next((i for i in range(min(len(want), len(data)))
                          if want[i] != data[i]), None)
            print(f"  DIFFERS from expected image (first byte at offset {first})")
            return 1
    return 0


def do_writ(link: Link, drive: int, track: int, data: bytes):
    """Full WRIT handshake; return (final_code, error_str)."""
    length = len(data)
    link.flush_input()
    link.send(build_block("WRIT", read_encode(drive, track), length))
    resp = link.recv_block(timeout=1.0)
    if resp is None:
        return None, "no WRIT response"
    mnem, code, _ = resp
    if mnem != "WRIT":
        return None, f"expected WRIT reply, got {mnem!r}"
    if code != 0:
        return code, f"server not ready ({RESP.get(code, code)})"
    link.send(data + le16(checksum16(data)))
    resp = link.recv_block(timeout=7.0)
    if resp is None:
        return None, "no WSTA response"
    mnem, code, _ = resp
    if mnem != "WSTA":
        return None, f"expected WSTA reply, got {mnem!r}"
    return code, None


def cmd_writ(link: Link, args) -> int:
    if args.data:
        with open(args.data, "rb") as f:
            data = f.read(args.len).ljust(args.len, b"\x00")
    else:
        data = bytes((0xA0 + (i & 0x1F)) & 0xFF for i in range(args.len))
    code, err = do_writ(link, args.drive, args.track, data)
    if code == 0:
        print(f"writ ok: drive {args.drive} track {args.track}, {len(data)} bytes, WSTA OK")
        return 0
    print(f"writ: {err or RESP.get(code, code)}")
    return 1


def cmd_verify(link: Link, args) -> int:
    """Round-trip every track of a local image against the served drive."""
    import os
    size = os.path.getsize(args.image)
    tlen = args.track_len
    if size % tlen:
        print(f"verify: image size {size} is not a multiple of track length {tlen}")
        return 1
    tracks = size // tlen
    print(f"verify: {args.image} = {tracks} tracks x {tlen} bytes on drive {args.drive}"
          f"{' (read+write)' if args.write else ' (read-only)'}")

    with open(args.image, "rb") as f:
        image = f.read()

    read_ok = write_ok = 0
    t0 = time.time()
    for t in range(tracks):
        want = image[t * tlen:(t + 1) * tlen]
        data, err = do_read(link, args.drive, t, tlen)
        if err or data != want:
            print(f"  track {t}: READ FAILED ({err or 'data mismatch'})")
            return 1
        read_ok += 1

        if args.write:
            code, werr = do_writ(link, args.drive, t, want)
            if code != 0:
                print(f"  track {t}: WRIT FAILED ({werr or RESP.get(code, code)})")
                return 1
            back, err = do_read(link, args.drive, t, tlen)
            if err or back != want:
                print(f"  track {t}: WRITE-BACK MISMATCH ({err or 'data mismatch'})")
                return 1
            write_ok += 1

        if t % 64 == 0 or t == tracks - 1:
            sys.stdout.write(f"\r  progress {t + 1}/{tracks}")
            sys.stdout.flush()

    dt = time.time() - t0
    moved = read_ok * tlen + write_ok * tlen
    print(f"\nverify PASSED: {read_ok} reads"
          f"{f', {write_ok} write round-trips' if args.write else ''} in {dt:.1f}s"
          f" ({moved / dt / 1024:.0f} KiB/s)")
    return 0


# ---- CLI -------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description="Host FDC+ simulator (drives the server).")
    ap.add_argument("--port", required=True, help="serial device of the FDC+ link")
    ap.add_argument("--baud", type=int, default=403200, help="link baud (default 403200)")
    ap.add_argument("-v", "--verbose", action="store_true", help="dump TX/RX bytes")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("loopback", help="host adapter TX<->RX jumper self-test")
    p.add_argument("--len", type=int, default=256)
    p.set_defaults(func=cmd_loopback)

    p = sub.add_parser("stat", help="poll drive-mount status")
    p.add_argument("--drive", type=int, default=0)
    p.set_defaults(func=cmd_stat)

    p = sub.add_parser("read", help="read one track")
    p.add_argument("drive", type=int)
    p.add_argument("track", type=int)
    p.add_argument("--len", type=int, default=8192, help="track length (default 8192)")
    p.add_argument("--out", help="write the payload to this file")
    p.add_argument("--expect", help="compare against this local image at track*len")
    p.set_defaults(func=cmd_read)

    p = sub.add_parser("writ", help="write one track")
    p.add_argument("drive", type=int)
    p.add_argument("track", type=int)
    p.add_argument("--len", type=int, default=8192)
    p.add_argument("--data", help="file whose bytes to write (default: a test pattern)")
    p.set_defaults(func=cmd_writ)

    p = sub.add_parser("verify", help="round-trip a whole image against a drive")
    p.add_argument("image", help="local .dsk to compare against")
    p.add_argument("--drive", type=int, default=0)
    p.add_argument("--track-len", type=int, default=8192)
    p.add_argument("--write", action="store_true", help="also write each track back")
    p.set_defaults(func=cmd_verify)

    args = ap.parse_args()
    link = Link(args.port, args.baud, args.verbose)
    try:
        return args.func(link, args)
    finally:
        link.close()


if __name__ == "__main__":
    sys.exit(main())
