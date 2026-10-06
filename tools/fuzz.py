#!/usr/bin/env python3
"""Generate a script of random commands from the game's own vocabulary, for
running the ported routines against the originals (hobbit-ref --check).

  tools/fuzz.py TAPE.tzx SEED COUNT > script.txt

Half the lines follow rough sentence shapes, the rest are word soup; both
mix in punctuation, quotes, the special words and the line-editing keys.
Commands that restart the game (QUIT, LOAD) and the save/pause prompts
just consume keys, which is fine for this purpose."""
import random, sys, os
sys.path.insert(0, os.path.dirname(__file__))
import mkimage, dictionary

mem = mkimage.build(sys.argv[1])
rng = random.Random(int(sys.argv[2]))
count = int(sys.argv[3])

by_class = {}
for off, word, cls, link in dictionary.entries(mem):
    if word:
        by_class.setdefault(cls, []).append(word)
words = [w for ws in by_class.values() for w in ws]

def pick(cls):
    return rng.choice(by_class[cls])

def noun_phrase():
    parts = []
    if rng.random() < 0.5: parts.append(pick(0x80))
    for _ in range(rng.choice([0, 0, 1, 1, 2, 3])): parts.append(pick(0x60))
    parts.append(rng.choice([pick(0x50), pick(0x50), 'IT', 'ALL', 'ONE']))
    return ' '.join(parts)

def sentence():
    r = rng.random()
    if r < 0.15: return f'{pick(0x40)} {pick(0x20)}'
    if r < 0.25: return pick(0x20)
    parts = []
    if rng.random() < 0.2: parts.append(pick(0x00))
    parts.append(pick(0x30))
    if rng.random() < 0.8: parts.append(noun_phrase())
    while rng.random() < 0.3: parts.append(rng.choice([',', 'AND', 'EXCEPT', 'BUT']) + ' ' + noun_phrase())
    if rng.random() < 0.5: parts.append(f'{rng.choice([pick(0x70), pick(0x10)])} {noun_phrase()}')
    if rng.random() < 0.1: parts.append(pick(0x00))
    return ' '.join(parts)

def command():
    if rng.random() < 0.5:
        parts = [sentence()]
        while rng.random() < 0.3:
            parts.append(rng.choice(['.', 'THEN', ',', 'AND']) + ' ' + sentence())
        line = ' '.join(parts)
        if rng.random() < 0.2:
            line = f'SAY TO {pick(0x50)} "{sentence()}' + ('"' if rng.random() < 0.9 else '')
    else:
        soup = [rng.choice(words + [',', '.', '"', 'AND', 'THEN']) for _ in range(rng.randint(1, 8))]
        line = ' '.join(soup)
    for w in ('QUIT', 'LOAD', 'SAVE', 'PAUSE'):
        if w in line.split() and rng.random() < 0.8:
            line = line.replace(w, 'LOOK')
    if rng.random() < 0.05: line = line[:rng.randint(0, len(line))] + '{DEL}' + line[rng.randint(0, len(line)):]
    if rng.random() < 0.02: line = 'XYZ{CLEAR}' + line
    if rng.random() < 0.03: return rng.choice(['@key @', '@key {UP}', '@key {LEFT}', '@key {ENTER}', '@wait 3500'])
    if rng.random() < 0.02: line = rng.choice(words) + 'X'  # unknown word
    return line[:200]

print(f'#opts --seed {int(sys.argv[2]) & 0xFF}')
print(f'# tools/fuzz.py TAPE {sys.argv[2]} {count}')
for _ in range(count):
    print(command())
