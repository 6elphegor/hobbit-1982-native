#!/usr/bin/env python3
"""List The Hobbit's dictionary: every word with its class and offset.

  tools/dictionary.py TAPE.tzx

Decoding follows the tokenizer ($6E97, ported in port/parser.c)."""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
import mkimage

def continues(mem, ix, count):
    if not mem[ix - 1] & 0x80: return True
    if count == 2: return True
    return count == 3 and bool(mem[ix - 2] & 0x80)

def entries(mem):
    """Yield (offset, letters, class, link) for each entry, by first letter."""
    for letter in range(1, 27):
        ix = 0x6000 + (mem[0x6000 + 2 * letter] | mem[0x6001 + 2 * letter] << 8)
        while (mem[ix] & 0x1F) == letter:
            start, count, codes = ix, 0, []
            while True:
                code = mem[ix] & 0x1F
                if code: codes.append(code)
                ix += 1; count += 1
                if not continues(mem, ix, count): break
            link = None
            if mem[ix - 1] & 0x40:
                link = (mem[ix] | mem[ix + 1] << 8)
                ix += 2
            target = 0x6000 + link if link is not None else start
            b0, b1 = mem[target], mem[target + 1]
            cls = (((b0 << 1) | (b0 >> 7)) & 0xC0) + (((b1 >> 1) | (b1 << 7)) & 0x30)
            yield start - 0x6000, ''.join(chr(64 + c) for c in codes), cls & 0xF0, link

if __name__ == '__main__':
    mem = mkimage.build(sys.argv[1])
    words = list(entries(mem))
    byoff = {off: w for off, w, _, _ in words}
    for off, w, cls, link in words:
        syn = f' -> {byoff.get(link, hex(link))}' if link is not None else ''
        print(f'${off:04X} ${cls:02X} {w}{syn}')
    print('\nspecial words ($8271):')
    for i in range(13):
        c, b = mem[0x8271 + 2 * i], mem[0x8272 + 2 * i]
        h = mem[0x8271 + 2 * i + 0x1A] | mem[0x8272 + 2 * i + 0x1A] << 8
        off = ((b & 0x0F) << 8) | c
        print(f'  ${b:02X}{c:02X} {byoff.get(off, "?")} -> ${h:04X}')
