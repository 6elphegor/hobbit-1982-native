#!/usr/bin/env python3
"""The demo's start and end card (tools/make-demo.sh).

  tools/tape-intro.py TAPE.tzx ROM OUTDIR

Writes OUTDIR/intro.rgb and OUTDIR/intro.wav: a 48K Spectrum switched
on, LOAD "" typed (with the ROM's key clicks), the header found ("Program:
hobbit"), and the game block loading behind the loading screen (the
tape's "p" block), all from the tape's own data: the ROM loader's border
stripes (red and cyan in the pilot tone, blue and yellow in the data)
drawn where the beam is at each edge, and the sound of the same edges. And
OUTDIR/end.rgb: a card in the ROM's character set. Frames are 320x240 RGB
(the screen and the border as build/hobbit-clean --video writes them), 25
a second."""
import bisect, os, struct, sys, wave

sys.path.insert(0, os.path.dirname(__file__))
import tzx

W, H, BX, BY = 320, 240, 32, 24
FPS = 25
CLOCK = 3500000
FRAME_T = 69888
LINE_T = 224
FIRST_LINE = 64 - BY  # the TV line the frame's top row is
PALETTE = [0x000000, 0x0000D7, 0xD70000, 0xD700D7, 0x00D700, 0x00D7D7, 0xD7D700, 0xD7D7D7,
           0x000000, 0x0000FF, 0xFF0000, 0xFF00FF, 0x00FF00, 0x00FFFF, 0xFFFF00, 0xFFFFFF]
RED, CYAN, BLUE, YELLOW = 2, 5, 1, 6

# The intro: this long before the data starts (in the pilot), and after.
PILOT_SHOWN = 0.9
DATA_SHOWN = 2.1


def rgb(c):
    return bytes((c >> 16 & 255, c >> 8 & 255, c & 255))


def edges(block):
    """(time in T, border colour after it) for each edge of a standard-speed
    block, and the time its data starts."""
    out, t = [], 0
    level = 0
    pilot = 8063 if block[0] < 0x80 else 3223
    for _ in range(pilot):
        t += 2168
        level ^= 1
        out.append((t, RED if level else CYAN))
    for d in (667, 735):
        t += d
        level ^= 1
        out.append((t, RED if level else CYAN))
    data_start = t
    for byte in block:
        for b in range(7, -1, -1):
            half = 1710 if byte >> b & 1 else 855
            for _ in range(2):
                t += half
                level ^= 1
                out.append((t, BLUE if level else YELLOW))
    return out, data_start


def screen_rows(scr):
    """The loading screen, 192 rows of 256 RGB pixels."""
    rows = []
    for y in range(192):
        line = ((y & 0xC0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2)
        row = bytearray()
        for cx in range(32):
            bits, attr = scr[line + cx], scr[0x1800 + (y >> 3) * 32 + cx]
            bright = (attr & 0x40) >> 3
            ink, paper = rgb(PALETTE[(attr & 7) | bright]), rgb(PALETTE[((attr >> 3) & 7) | bright])
            for b in range(8):
                row += ink if bits & (0x80 >> b) else paper
        rows.append(bytes(row))
    return rows


class Tape:
    """Edges of a block from a time on, for the border and the sound."""
    def __init__(self, block, before_data):
        self.ev, data_start = edges(block)
        self.times = [t for t, _ in self.ev]
        self.start = data_start - int(before_data * CLOCK)

    def colour_at(self, t):
        i = bisect.bisect_right(self.times, t) - 1
        return self.ev[i][1] if i >= 0 else RED


def frame(rows, border_at):
    """A frame: the 192 rows of the screen, and the border colour at each
    pixel's moment (border_at(row, x) -> colour)."""
    out = bytearray()
    for y in range(H):
        def border(x0, x1):
            seg = bytearray()
            for x in range(x0, x1, 2):  # a T-state is two pixels
                seg += rgb(PALETTE[border_at(y, x)]) * 2
            return seg
        if BY <= y < BY + 192:
            out += border(0, BX) + rows[y - BY] + border(BX + 256, W)
        else:
            out += border(0, W)
    return bytes(out)


def striped(tape, n):
    """The border of frame n of a tape's loading, as the beam meets it."""
    t0 = tape.start + n * CLOCK // FPS
    return lambda y, x: tape.colour_at(t0 + (FIRST_LINE + y) * LINE_T + (x - BX) // 2)


def text_screen(font, lines, flash_on):
    """The ROM's screen: black on white, lines of (row, column, text,
    inverse, flashing)."""
    cells = {}
    for row, col, text, inverse, flashing in lines:
        for i, ch in enumerate(text):
            cells[(row, col + i)] = (ch, inverse != (flashing and flash_on))
    white, black = rgb(PALETTE[7]), rgb(PALETTE[0])
    rows = []
    for y in range(192):
        r = bytearray()
        for cx in range(32):
            ch, inv = cells.get((y >> 3, cx), (' ', False))
            code = 0x7F if ch == '\u00a9' else ord(ch)
            bits = font[(code - 32) * 8 + (y & 7)]
            for b in range(8):
                on = bool(bits & (0x80 >> b)) != inv
                r += black if on else white
        rows.append(bytes(r))
    return rows


def sound(tape, seconds, rate):
    """The level after each edge, quietly."""
    n = int(seconds * rate)
    level, i = 0, bisect.bisect_right(tape.times, tape.start)
    out = bytearray()
    for s in range(n):
        t = tape.start + s * CLOCK // rate
        while i < len(tape.times) and tape.times[i] <= t:
            level ^= 1
            i += 1
        out += struct.pack('<h', 5000 if level else -5000)
    return out


def click(rate):
    """The 48K ROM's key click: a short pulse on the speaker."""
    return struct.pack('<h', 9000) * int(rate * 0.0012) + struct.pack('<h', -9000) * int(rate * 0.0012)


def intro(tape, rom, outdir):
    blocks = [b for bid, b in tzx.blocks(open(tape, 'rb').read()) if bid == 0x10]
    font = open(rom, 'rb').read()[0x3D00:0x4000]
    rate = 44100
    frames, audio = [], bytearray()

    def silence(n):
        return b'\0\0' * n

    def add(f, sound_bytes=None):
        frames.append(f)
        per = rate // FPS * 2
        chunk = sound_bytes if sound_bytes is not None else b''
        audio.extend((chunk + silence(per // 2))[:per])

    white = lambda y, x: 7
    # The machine switched on, then LOAD "" typed: J (LOAD), SYMBOL+P twice, ENTER.
    boot = [(23, 0, '\u00a9 1982 Sinclair Research Ltd', False, False)]
    for n in range(int(1.0 * FPS)):
        add(frame(text_screen(font, boot, False), white))
    typed = ['', 'LOAD ', 'LOAD "', 'LOAD ""']
    cursor = ['K', 'L', 'L', 'L']
    for k, (text, cur) in enumerate(zip(typed, cursor)):
        for n in range(int(0.45 * FPS)):
            line = [(23, 0, text, False, False), (23, len(text), cur, False, True)]
            add(frame(text_screen(font, line, (len(frames) // 8) & 1), white), click(rate) if n == 0 and k else None)
    add(frame(text_screen(font, [], False), white), click(rate))  # ENTER: the screen clears
    # The header: some of its pilot tone, its data, and the ROM saying what it found.
    header = Tape(blocks[0], 0.7)
    hdr_seconds = 0.7 + 0.15
    hdr_sound = sound(header, hdr_seconds, rate)
    per = rate // FPS * 2
    for n in range(int(hdr_seconds * FPS)):
        add(frame(text_screen(font, [], False), striped(header, n)), bytes(hdr_sound[n * per:(n + 1) * per]))
    name = bytes(blocks[0][2:12]).decode().rstrip()
    found = text_screen(font, [(0, 0, 'Program: ' + name, False, False)], False)
    for n in range(int(0.5 * FPS)):
        add(frame(found, white))
    # The game block loading behind the loading screen (the tape's "p" block).
    screen = blocks[3][1:-1]
    game = Tape(blocks[5], PILOT_SHOWN)
    rows = screen_rows(screen)
    secs = PILOT_SHOWN + DATA_SHOWN
    game_sound = sound(game, secs, rate)
    for n in range(int(secs * FPS)):
        add(frame(rows, striped(game, n)), bytes(game_sound[n * per:(n + 1) * per]))

    with open(os.path.join(outdir, 'intro.rgb'), 'wb') as f:
        f.write(b''.join(frames))
    with wave.open(os.path.join(outdir, 'intro.wav'), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(audio))


def end_card(rom, outdir, seconds=3.5):
    """Lines of text in the ROM's character set (at $3D00), on black."""
    font = open(rom, 'rb').read()[0x3D00:0x4000]
    lines = [  # (text, colour, scale)
        ('THE HOBBIT', CYAN + 8, 2),
        ('', 0, 1),
        ('Melbourne House, 1982', 7, 1),
        ('ported to C, routine by routine', 7, 1),
        ('', 0, 1),
        ('Your adventure starts', YELLOW + 8, 1),
        ('at Bag End.', YELLOW + 8, 1),
        ('', 0, 1),
        ('make && build/hobbit-clean-sdl', 4 + 8, 1),
    ]
    px = [[0] * W for _ in range(H)]
    height = sum(8 * s for _, _, s in lines) + 4 * (len(lines) - 1)
    y = (H - height) // 2
    for text, colour, scale in lines:
        x = (W - len(text) * 8 * scale) // 2
        for ch in text:
            glyph = font[(ord(ch) - 32) * 8:(ord(ch) - 32) * 8 + 8]
            for gy in range(8):
                for gx in range(8):
                    if glyph[gy] & (0x80 >> gx):
                        for sy in range(scale):
                            for sx in range(scale):
                                px[y + gy * scale + sy][x + gx * scale + sx] = PALETTE[colour]
            x += 8 * scale
        y += 8 * scale + 4
    frame = b''.join(rgb(c) for row in px for c in row)
    with open(os.path.join(outdir, 'end.rgb'), 'wb') as f:
        f.write(frame * int(seconds * FPS))


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    tape, rom, outdir = sys.argv[1:]
    intro(tape, rom, outdir)
    end_card(rom, outdir)


main()
