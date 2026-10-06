#!/usr/bin/env python3
"""Check the game's tables of code addresses against the ported routines.

  tools/tables.py [TAPE.tzx]

Walks the tables the game jumps through (default actions $C730, the
action lists in object records, location events $C78E, timed events
$CA84, the character scripts at $C8xx-$CA83, the special words $8271)
and lists each target with whether port/*.c registers a C version of it.
Default memory image: tools/mem.bin."""
import os, re, sys
here = os.path.dirname(os.path.abspath(__file__))
root = os.path.join(here, '..')
if len(sys.argv) > 1:
    sys.path.insert(0, here)
    import mkimage
    mem = bytes(mkimage.build(sys.argv[1]))
else:
    mem = open(os.path.join(here, 'mem.bin'), 'rb').read()

registered = set()
for f in os.listdir(os.path.join(root, 'port')):
    if f.endswith('.c'):
        for m in re.finditer(r'\{0x([0-9A-Fa-f]{4}),', open(os.path.join(root, 'port', f)).read()):
            registered.add(int(m.group(1), 16))

w = lambda a: mem[a] | mem[a + 1] << 8
targets = {}  # address -> list of where it comes from
def add(addr, where):
    if addr: targets.setdefault(addr, []).append(where)

def three_byte(start, name):
    p = start
    while mem[p] != 0xFF:
        add(w(p + 1), '%s $%04X (key $%02X)' % (name, p, mem[p]))
        p += 3

three_byte(0xC730, 'default actions')
three_byte(0xC78E, 'location events')
p = 0xC063  # objects: id, record
while mem[p] != 0xFF:
    rec = w(p + 1)
    three_byte(rec + mem[rec] + 0x10, 'object $%02X' % mem[p])
    p += 3
p = 0xCA84  # timed events: count, timer, addr, threshold, addr
while mem[p] != 0xFF:
    add(w(p + 2), 'timed event $%04X' % p)
    if mem[p + 4]: add(w(p + 5), 'timed event $%04X (second)' % p)
    p += 7
for i in range(13):  # special words of the parser
    add(w(0x8271 + 0x1A + 2 * i), 'special word %d' % i)
# Character scripts: ops with bit 0 set ($03, $13, $43) call the address
# that follows (and a further word with bit 4). Found by pattern, so check
# the listing.
for a in range(0xC808, 0xCA84):
    if mem[a] in (0x03, 0x13, 0x43) and 0x8000 <= w(a + 1) < 0xC000 and mem[a + 3] == 0x00:
        add(w(a + 1), 'script op $%02X at $%04X' % (mem[a], a))

# Jumped to inside a routine that is ported whole (no entry of its own).
inside = [(0x8251, 0x838E, 'sentence.c: the parser dispatches the special words itself')]
missing = 0
for addr in sorted(targets):
    note = [n for lo, hi, n in inside if lo <= addr <= hi]
    if note:
        print('$%04X ok      %s (%s)' % (addr, '; '.join(targets[addr][:3]), note[0]))
        continue
    ok = addr in registered
    missing += not ok
    print('$%04X %-7s %s' % (addr, 'ok' if ok else 'MISSING', '; '.join(targets[addr][:3])))
print('%d targets, %d without a C version' % (len(targets), missing))
