#!/usr/bin/env python3
"""Send CLI commands to the ESP32 console and print replies.
usage: sds-cli.py [--wait S] "cmd1" "cmd2" ...   (--wait applies to the following cmds)"""
import sys, time, serial
ser = serial.Serial('/dev/cu.usbserial-0001', 115200, timeout=0.1)  # set once, never changed
ser.write(b'\r'); time.sleep(0.3); ser.read(4096)  # flush Mac-driver junk + prompt
wait = 1.5
args = sys.argv[1:]
i = 0
while i < len(args):
    if args[i] == '--wait':
        wait = float(args[i+1]); i += 2; continue
    ser.write(args[i].encode() + b'\r')
    end = time.time() + wait; out = b''
    while time.time() < end:
        out += ser.read(4096)
    sys.stdout.write(out.decode('utf-8', 'replace'))
    i += 1
