#!/usr/bin/env python3
"""Patch src/pkjs/index.js for an emulator run, or undo the patch.

Usage: scripts/emu-test.py apply LAYOUT ROWS SHOW LAT LON
       scripts/emu-test.py restore

The emulator has no settings page session and no location, so `apply` makes
the phone side pretend: it forces the layout and the board options on the
watch, and answers every location request with LAT, LON. The original file
is kept in build/index.js.orig and put back by `restore`. Every patched line
is marked EMUTEST; never commit a file that contains that word.
"""
import os
import shutil
import sys

SRC = 'src/pkjs/index.js'
KEEP = 'build/index.js.orig'


def restore():
    if os.path.exists(KEEP):
        shutil.move(KEEP, SRC)


def swap(text, old, new):
    if text.count(old) != 1:
        restore()
        sys.exit('emu-test: the code this patches has changed: ' + old.strip()[:60])
    return text.replace(old, new)


if sys.argv[1] == 'restore':
    restore()
    sys.exit(0)

layout, rows, show, lat, lon = sys.argv[2:7]
restore()
os.makedirs('build', exist_ok=True)
shutil.copy(SRC, KEEP)
s = open(SRC).read()
if layout == '3':
    s = swap(s, 'function savedSettings() {',
             "function savedSettings() {\n  return { LAYOUT: '3', RADIUS: '400' }; // EMUTEST")
s = swap(s, "    var geo = typeof navigator !== 'undefined' && navigator.geolocation;\n"
            "    if (!geo) return finishLocation(",
         "    var geo = { getCurrentPosition: function(ok) { ok({ coords: { latitude: %s, "
         "longitude: %s, accuracy: 5 } }); } }; // EMUTEST\n"
         "    if (!geo) return finishLocation(" % (lat, lon))
s = swap(s, '  dict[keys.BIKE_FAILED] = 0;\n  function done() {',
         '  dict[keys.BIKE_FAILED] = 0;\n  dict[keys.LAYOUT] = %s; dict[keys.BOARD_ROWS] = %s; '
         'dict[keys.BOARD_SHOW] = %s; // EMUTEST\n  function done() {' % (layout, rows, show))
open(SRC, 'w').write(s)
