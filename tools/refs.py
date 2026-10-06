#!/usr/bin/env python3
"""Cross-references from the disassembly (reference/pobtastic-hobbit).

  tools/refs.py ADDR [ADDR...]

For each address (hex), lists every instruction that reaches it: CALL,
JP, JR, DJNZ (with the condition, if any), and whether the instruction
before it falls through into it. Use it to decide whether a routine can
be hooked at that address: every way in must leave a return address on
top of the stack."""
import re, sys, os
skool = os.path.join(os.path.dirname(__file__), '..', 'reference', 'pobtastic-hobbit', 'sources', 'hobbit.skool')
ins = []
for l in open(skool).read().split('\n'):
    m = re.match(r'^([a-z* ])\$([0-9A-F]{4}) (.*?)\s*(;.*)?$', l)
    if m: ins.append((int(m.group(2), 16), m.group(1), m.group(3).strip()))
by_addr = {a: (k, t) for a, k, t in ins}
for arg in sys.argv[1:]:
    target = int(arg, 16)
    print(f'${target:04X}:')
    for a, k, t in ins:
        m = re.match(r'(CALL|JP|JR|DJNZ)\s+(?:([A-Z]+),)?\$([0-9A-F]{4})$', t)
        if m and int(m.group(3), 16) == target:
            print(f'  ${a:04X}  {t}')
    idx = [i for i, (a, k, t) in enumerate(ins) if a == target]
    if idx and idx[0] > 0:
        pa, pk, pt = ins[idx[0] - 1]
        ends = re.match(r'(RET|JP|JR)\b', pt) and not re.match(r'(RET|JP|JR)\s+[A-Z]+,|RET [A-Z]+', pt)
        if not ends and not pt.startswith('DEF'):
            print(f'  ${pa:04X}  {pt}   <- falls through')
