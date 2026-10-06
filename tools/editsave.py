#!/usr/bin/env python3
"""Edit a saved game (the .tap the game's SAVE writes), to set up a scene
for a test script (tests/run.sh: a "#scene EDITS" line).

  tools/editsave.py TAPE.tzx IN.tap OUT.tap [ID=LOCATION | ID@OFFSET=BYTE ...]

Each ID=LOCATION (hex) puts object ID, loose (in no container) and
visible, at LOCATION; object 0 is the player. ID@OFFSET=BYTE sets a byte
of its record (+3 weight, the strength; +7 attributes). The saved game is four
blocks (flag $FF): the variables ($B6EB), the objects ($C11B), the timed
events ($CA84) and the locations ($BA8A); their checksums are redone."""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
import mkimage

OBJECTS = 0xC11B
OBJECT_INDEX = 0xC063

def blocks(data):
    """(start of the data, length) of each block."""
    out, p = [], 0
    while p < len(data):
        n = data[p] | data[p + 1] << 8
        out.append((p + 3, n - 2))
        p += 2 + n
    return out

def record_of(mem, obj):
    t = OBJECT_INDEX
    while mem[t] != 0xFF:
        if mem[t] == obj:
            return mem[t + 1] | mem[t + 2] << 8
        t += 3
    raise SystemExit('no object %02X' % obj)

def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    mem = mkimage.build(sys.argv[1])
    data = bytearray(open(sys.argv[2], 'rb').read())
    bl = blocks(data)
    objects_at = bl[1][0]
    for spec in sys.argv[4:]:
        what, value = spec.split('=')
        value = int(value, 16)
        if '@' in what:
            obj, off = (int(x, 16) for x in what.split('@'))
            data[objects_at + record_of(mem, obj) - OBJECTS + off] = value
            continue
        at = objects_at + record_of(mem, int(what, 16)) - OBJECTS
        data[at + 1] = 0xFF  # container: none
        data[at + 0x10] = value  # its (first) location
        data[at + 7] |= 0x80  # visible
    for start, n in bl:
        x = data[start - 1]  # the flag byte, then the data
        for i in range(n):
            x ^= data[start + i]
        data[start + n] = x
    open(sys.argv[3], 'wb').write(data)

main()
