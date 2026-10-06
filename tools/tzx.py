#!/usr/bin/env python3
"""Minimal TZX reader: lists blocks and loads CODE blocks into a 64K memory image."""
import sys, struct

def blocks(data):
    assert data[:8] == b'ZXTape!\x1a', 'not a TZX file'
    i = 10
    while i < len(data):
        bid = data[i]; i += 1
        if bid == 0x10:
            pause, n = struct.unpack_from('<HH', data, i); i += 4
            yield bid, data[i:i+n]; i += n
        elif bid == 0x11:
            n = int.from_bytes(data[i+15:i+18], 'little'); i += 18
            yield bid, data[i:i+n]; i += n
        elif bid == 0x14:
            n = int.from_bytes(data[i+7:i+10], 'little'); i += 10
            yield bid, data[i:i+n]; i += n
        elif bid == 0x30:
            n = data[i]; yield bid, data[i+1:i+1+n]; i += 1 + n
        elif bid == 0x32:
            n = struct.unpack_from('<H', data, i)[0]; yield bid, data[i+2:i+2+n]; i += 2 + n
        elif bid == 0x20:
            i += 2
        elif bid in (0x21,):
            n = data[i]; yield bid, data[i+1:i+1+n]; i += 1 + n
        elif bid == 0x22:
            pass
        else:
            raise ValueError(f'unsupported TZX block 0x{bid:02x} at {i-1}')

def load(path):
    mem = bytearray(65536)
    header = None
    loaded = []
    for bid, b in blocks(open(path, 'rb').read()):
        if bid not in (0x10, 0x11, 0x14) or not b:
            continue
        flag, body = b[0], b[1:-1]
        if flag == 0x00 and len(body) == 17:
            header = body
        elif flag == 0xff and header is not None:
            typ = header[0]; name = header[1:11].decode('latin-1').rstrip()
            length, p1, p2 = struct.unpack_from('<HHH', header, 11)
            if typ == 3:
                mem[p1:p1+len(body)] = body
            loaded.append((typ, name, p1, len(body), p2))
            header = None
        elif flag == 0xff:
            loaded.append((None, '(headerless)', None, len(body), None))
    return mem, loaded

if __name__ == '__main__':
    mem, loaded = load(sys.argv[1])
    for typ, name, start, n, p2 in loaded:
        kind = {0: 'Program', 3: 'Bytes', None: '?'}.get(typ, typ)
        s = f'{start} (0x{start:04X})' if start is not None else '-'
        print(f'{kind:8} {name!r:14} start={s:16} len={n} p2={p2}')
    if len(sys.argv) > 2:
        open(sys.argv[2], 'wb').write(mem)
