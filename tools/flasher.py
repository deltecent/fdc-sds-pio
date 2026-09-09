#!/usr/bin/env python3
"""
flasher.py — one-command USB (re)flasher for the ESP32 FDC+ Serial Disk Server.

For a new, blank, or wedged board, this erases the flash and writes the published
merged image (bootloader + partition table + application, ota/merged.bin) at 0x0 —
a clean factory install. It is the cross-platform (Windows / Linux / macOS) equivalent
of the Espressif Flash Download Tool, and needs nothing installed but Python 3: the
esptool/pyserial/reedsolo it uses are vendored under tools/vendor/ (see that README).

Typical use, from anywhere in the repo:

    python3 tools/flasher.py            # auto-detect the board, confirm, erase + flash
    python3 tools/flasher.py --list     # just list serial ports and exit
    python3 tools/flasher.py --probe    # identify the chip (read-only), then exit

By default this ERASES ALL FLASH, including the NVS partition that holds saved WiFi
credentials, host name, timezone, and mounted drives. The merged image carries no NVS
of its own, so pass --keep-config to reinstall the firmware while preserving those
settings.

esptool (GPL-2.0) is run as a subprocess, never imported, so it stays an aggregated
standalone program alongside this repo rather than a linked library.
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
VENDOR = os.path.join(HERE, "vendor")
DEFAULT_IMAGE = os.path.join(REPO, "ota", "merged.bin")
DEFAULT_BAUD = "921600"

# USB-UART bridges used by the ESP32 DEVKIT V1 this firmware targets. We match on
# these so auto-detect picks the board and not some unrelated serial adapter (an FTDI
# cable, a modem) sharing the machine. Not exhaustive; use --port for anything else.
#   Silicon Labs CP210x  (this board's CP2102)   |   WCH CH340 / CH341
BOARD_USB_IDS = {
    (0x10C4, 0xEA60), (0x10C4, 0xEA70),
    (0x1A86, 0x7523), (0x1A86, 0x5523), (0x1A86, 0x55D4),
}


def die(msg, code=1):
    print("error: " + msg, file=sys.stderr)
    sys.exit(code)


def list_ports():
    """All serial ports, via the vendored pyserial. (import, not subprocess: BSD.)"""
    if VENDOR not in sys.path:
        sys.path.insert(0, VENDOR)
    try:
        from serial.tools import list_ports as _lp
    except Exception as e:  # pragma: no cover - vendor tree missing/broken
        die("could not load the bundled pyserial from %s (%s)" % (VENDOR, e))
    return list(_lp.comports())


def looks_like_board(p):
    return p.vid is not None and (p.vid, p.pid) in BOARD_USB_IDS


def print_ports(ports):
    if not ports:
        print("  (no serial ports found)")
        return
    for p in ports:
        ids = "%04x:%04x" % (p.vid, p.pid) if p.vid is not None else "----:----"
        mark = "  <- looks like the board" if looks_like_board(p) else ""
        desc = (p.product or p.description or "").strip()
        print("  %-24s [%s] %s%s" % (p.device, ids, desc, mark))


def resolve_port(explicit):
    if explicit:
        return explicit
    ports = list_ports()
    matches = [p.device for p in ports if looks_like_board(p)]
    if len(matches) == 1:
        return matches[0]
    if not matches:
        print("No ESP32 board auto-detected among the serial ports:", file=sys.stderr)
        print_ports(ports)
        die("plug the board in (or pass --port <PORT>). On some boards you must hold "
            "BOOT while connecting.")
    print("More than one board-like port found:", file=sys.stderr)
    print_ports([p for p in ports if looks_like_board(p)])
    die("pass --port <PORT> to choose one.")


def esptool(args):
    """Run the vendored esptool as a subprocess; return its exit code."""
    env = dict(os.environ)
    env["PYTHONPATH"] = VENDOR + os.pathsep + env.get("PYTHONPATH", "")
    # esptool 4.5.1 trips a harmless SyntaxWarning ('return' in 'finally') on
    # Python 3.12+; silence just that so it doesn't clutter the flash output.
    cmd = [sys.executable, "-W", "ignore::SyntaxWarning", "-m", "esptool"] + args
    return subprocess.call(cmd, env=env)


def human_size(n):
    return "%.2f MB (%d bytes)" % (n / (1024 * 1024), n)


def main():
    ap = argparse.ArgumentParser(
        description="Erase and reflash the ESP32 FDC+ Serial Disk Server over USB.")
    ap.add_argument("--port", help="serial port (default: auto-detect the board)")
    ap.add_argument("--baud", default=DEFAULT_BAUD,
                    help="flash baud rate (default: %(default)s)")
    ap.add_argument("--image", default=DEFAULT_IMAGE,
                    help="merged image to flash at 0x0 (default: ota/merged.bin)")
    ap.add_argument("--keep-config", action="store_true",
                    help="skip the full erase, preserving saved WiFi/host/timezone/drives")
    ap.add_argument("--yes", "-y", action="store_true",
                    help="do not ask for confirmation")
    ap.add_argument("--list", action="store_true",
                    help="list serial ports and exit")
    ap.add_argument("--probe", action="store_true",
                    help="identify the chip (read-only) and exit")
    args = ap.parse_args()

    if args.list:
        print("Serial ports:")
        print_ports(list_ports())
        return 0

    port = resolve_port(args.port)

    if args.probe:
        print(">> probing %s (read-only)" % port)
        return esptool(["--chip", "esp32", "--port", port, "chip_id"])

    image = os.path.abspath(args.image)
    if not os.path.isfile(image):
        die("image not found: %s\n"
            "       build it with tools/build-ota.sh, or pass --image <PATH>." % image)
    size = os.path.getsize(image)
    if size < 256 * 1024:
        die("image %s is only %d bytes — that is not a merged full-flash image."
            % (image, size))

    erase = not args.keep_config
    print("About to flash the ESP32 FDC+ Serial Disk Server:")
    print("  port:   %s" % port)
    print("  image:  %s" % image)
    print("  size:   %s" % human_size(size))
    print("  baud:   %s" % args.baud)
    if erase:
        print("  erase:  YES — erases ALL flash, INCLUDING saved WiFi/host/timezone/drives")
    else:
        print("  erase:  no (--keep-config) — saved settings are preserved")

    if not args.yes:
        if not sys.stdin.isatty():
            die("not a terminal; re-run with --yes to flash non-interactively.")
        reply = input("Proceed? [y/N] ").strip().lower()
        if reply not in ("y", "yes"):
            print("Aborted.")
            return 1

    if erase:
        print("\n>> erasing flash")
        rc = esptool(["--chip", "esp32", "--port", port, "--baud", args.baud, "erase_flash"])
        if rc != 0:
            return fail_hint(rc)

    print("\n>> writing %s at 0x0" % os.path.basename(image))
    rc = esptool(["--chip", "esp32", "--port", port, "--baud", args.baud,
                  "write_flash", "0x0", image])
    if rc != 0:
        return fail_hint(rc)

    print("\nDone. Connect a serial terminal at 115200 baud and press ENTER — you should")
    print("see the banner and the FDC-SDS-ESP32> prompt. Next: the Initial Setup guide.")
    return 0


def fail_hint(rc):
    print("\nesptool failed (exit %d)." % rc, file=sys.stderr)
    print("Common causes:", file=sys.stderr)
    print("  - The port is busy: close any open serial monitor "
          "(pio device monitor, screen, TeraTerm) and retry.", file=sys.stderr)
    print("  - Could not enter download mode: hold the BOOT button while you start the",
          file=sys.stderr)
    print("    flash (release it after it connects), or lower --baud to 115200.",
          file=sys.stderr)
    return rc


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nInterrupted.", file=sys.stderr)
        sys.exit(130)
