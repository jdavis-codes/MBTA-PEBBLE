#!/usr/bin/env python3
"""Save the emulator's screen as a PNG, straight from QEMU.

Usage: scripts/dump.py OUT.png [SCALE]

Reads the running emulator's monitor port from its command line, so it works
when `pebble screenshot` is slow or the emulator is busy. Needs Pillow.
"""
import re
import socket
import subprocess
import sys
import time

from PIL import Image

out = sys.argv[1]
scale = int(sys.argv[2]) if len(sys.argv) > 2 else 4
cmd = subprocess.run(['pgrep', '-af', 'qemu-pebble'], capture_output=True, text=True).stdout
match = re.search(r'-monitor tcp::(\d+)', cmd)
if not match:
    sys.exit('No emulator is running: start one with `make emu`.')
ppm = out + '.ppm'
s = socket.create_connection(('127.0.0.1', int(match.group(1))))
time.sleep(.3)
s.recv(65536)
s.sendall(('screendump %s\n' % ppm).encode())
time.sleep(.6)
s.close()
im = Image.open(ppm)
im.resize((im.width * scale, im.height * scale), Image.NEAREST).save(out)
print('Saved', out)
