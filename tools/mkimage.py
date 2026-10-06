#!/usr/bin/env python3
"""Build the post-relocation 64K memory image of The Hobbit v1.2 from the user's TZX.

Mirrors pobtastic/hobbit's hobbit.t2s: screen at $4000, game at $6000, then the
four block moves the game performs at startup. Entry point is $6C00.
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
import tzx

MOVES = [(0xC11B, 0x0615, 0xF400), (0xBA8A, 0x05D9, 0xFA15),
         (0xB6EB, 0x001D, 0x5F00), (0xCA84, 0x00BF, 0x5F1D)]

def build(path):
    mem = bytearray(65536)
    datablocks = [b[1:-1] for bid, b in tzx.blocks(open(path, 'rb').read())
                  if bid in (0x10, 0x11, 0x14) and b and b[0] == 0xff]
    screen, game = datablocks[1], datablocks[2]
    assert len(screen) == 6912 and len(game) == 40000, 'unexpected tape layout (not v1.2?)'
    mem[0x4000:0x4000 + 6912] = screen
    mem[0x6000:0x6000 + 40000] = game
    for src, n, dst in MOVES:
        mem[dst:dst + n] = mem[src:src + n]
    return mem

if __name__ == '__main__':
    open(sys.argv[2], 'wb').write(build(sys.argv[1]))
