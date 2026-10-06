#!/usr/bin/env python3
"""List every address the ported code reaches in other code, and whether a
ported routine is registered there: what a build without the emulator
still needs.

  tools/audit.py

Looks at cpu_call_at(site) (the CALL's target, read from the memory
image), cpu_call(addr), cpu_tail(addr), cpu_jump(addr, ...), and the
#defines and helper macros they use."""
import re, glob, os
root = os.path.join(os.path.dirname(__file__), '..')
mem = open(os.path.join(root, 'tools', 'mem.bin'), 'rb').read()
src = {f: open(f).read() for f in glob.glob(os.path.join(root, 'port', '*.c'))}
registered = set()
for s in src.values():
    for m in re.finditer(r'^\s*\{0x([0-9A-Fa-f]{4}),', s, re.M):
        registered.add(int(m.group(1), 16))
defines = {}
for s in src.values():
    for m in re.finditer(r'#define\s+(\w+)\s+(0x[0-9A-Fa-f]+)', s):
        defines[m.group(1)] = int(m.group(2), 16)
def val(tok):
    tok = tok.strip().rstrip(')')
    if re.fullmatch(r'0x[0-9A-Fa-f]+', tok): return int(tok, 16)
    return defines.get(tok)
needs = {}
def note(addr, how, f):
    if addr is None: return
    needs.setdefault(addr, set()).add(f'{how}:{os.path.basename(f)}')
for f, s in src.items():
    for m in re.finditer(r'cpu_call_at\(c,\s*([\w]+)', s):
        site = val(m.group(1))
        if site is None: continue
        op = mem[site]
        if op == 0xCD or (op & 0xC7) == 0xC4:
            note(mem[site+1] | mem[site+2] << 8, 'call', f)
    for kind in ('cpu_call', 'cpu_tail', 'cpu_jump'):
        for m in re.finditer(kind + r'\(c,\s*([\w]+)(?:\s*,\s*([\w]+))?', s):
            note(val(m.group(1)), kind[4:], f)
            if kind == 'cpu_jump' and m.group(2): note(val(m.group(2)), 'resume', f)
    # helper wrappers. actions3 CALL(site, target), events call(c, site,
    # target): the target is second; executor call(c, target, ret): first.
    # call_near/call_local (actions1) and text.c's CALL run local C code.
    base = os.path.basename(f)
    if base in ('actions3.c', 'events.c'):
        for m in re.finditer(r'\b(?:CALL|call)\((?:c,\s*)?(0x[0-9A-Fa-f]+|\w+),\s*(0x[0-9A-Fa-f]+|\w+)\)', s):
            note(val(m.group(2)), 'call', f)
    if base == 'executor.c':
        for m in re.finditer(r'\bcall\(c,\s*(0x[0-9A-Fa-f]+|\w+),\s*(0x[0-9A-Fa-f]+|\w+)\)', s):
            note(val(m.group(1)), 'call', f)
    for m in re.finditer(r'\btail\(c,\s*(\w+)\)', s):
        note(val(m.group(1)), 'tail', f)
missing = {a: h for a, h in needs.items() if a not in registered and a >= 0x4000}
print(f'{len(registered)} registered, {len(needs)} addresses reached, {len(missing)} not registered:')
for a in sorted(missing):
    print(f'  ${a:04X}  {", ".join(sorted(missing[a]))}')
