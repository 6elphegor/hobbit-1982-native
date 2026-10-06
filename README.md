<div align="center">

# 🐉 The Hobbit, native

**The 1982 ZX Spectrum adventure, ported to C one routine at a time.**<br>
No Z80 emulation: just the game, running natively, matching the
original transcript for transcript.

![C](https://img.shields.io/badge/C-11-00599C?style=flat-square&logo=c&logoColor=white)
![SDL2](https://img.shields.io/badge/SDL-2-1d4f91?style=flat-square)
![Routines](https://img.shields.io/badge/routines_ported-338%2F338-2ea44f?style=flat-square)
![Tests](https://img.shields.io/badge/test_suite-~10s-2ea44f?style=flat-square)
![ZX Spectrum](https://img.shields.io/badge/ZX_Spectrum-48K-d70000?style=flat-square)

<br>

<img src="docs/demo.gif" width="640" alt="A 48K Spectrum switched on, LOAD &quot;&quot; typed, the tape loading behind the title screen, then Bag End drawing itself, Gandalf killed, Thorin unmoved, and the trolls">

<sub>Switch on, <code>LOAD ""</code>, and the tape's stripes roll round the loading screen,
all rendered from the tape's own data (<a href="docs/demo.mp4">▶ with its sound</a>, the only
sound the game ever makes). Then Bag End draws itself, Gandalf hands over the map, Thorin sings
about gold, Gandalf dies of the player's curiosity, and some trolls have an opinion of their own.</sub>

</div>

---

*The Hobbit* (Melbourne House, 1982) was the adventure that talked back.
Characters who walk off on their own business, a parser that understood
`ASK ELROND TO READ THE MAP`, pictures drawn line by line, and a world
that carries on whether you act or not. This project rebuilds all of it
in C, using pobtastic's annotated
[SkoolKit disassembly](https://github.com/pobtastic/hobbit) as the map.

There are two editions:

<table>
<tr>
<th width="50%">🔬 The faithful port: <code>build/hobbit</code></th>
<th width="50%">✨ The clean edition: <code>build/hobbit-clean-sdl</code></th>
</tr>
<tr valign="top">
<td>

Every one of the original's 338 routines, in C, keeping its register
interface and memory map. It reproduces the original down to the flags
it leaves on the stack, because the game's random number generator reads
*all of memory*. Each routine is checked against the original Z80 code
on every call.

</td>
<td>

The same game in ordinary C: real functions, real arguments, a proper
random number generator, and **the original's bugs fixed**: the crashes,
the infinite loops, the corrupted memory, and the plain mistakes.
`--original-bugs` puts every one of them back.

</td>
</tr>
</table>

## 🚀 Playing

```sh
make
build/hobbit-clean-sdl --rom 48.rom "Hobbit, The v1.2 (1982)(Melbourne House).tzx"
```

> [!IMPORTANT]
> **Bring your own tape.** None of the game's data (dictionary, objects,
> characters, messages, pictures) is in this repository: it is all read,
> when the program starts, from your own copy of the tape,
> `Hobbit, The v1.2 (1982)(Melbourne House).tzx`, which must be v1.2 (the
> loader checks a CRC of the game block). A 48K ROM (`48.rom`) is
> optional, but gives results that match real hardware.

Needs a C compiler and SDL2 (`brew install sdl2`, `apt install libsdl2-dev`).

| Key | Does |
| --- | --- |
| letters, space, Enter | type, as on the Spectrum |
| Backspace | delete |
| ← ↑ → ↓ (first key of a line) | go west, north, east, south |
| Ctrl or Alt | SYMBOL SHIFT: Ctrl+M `.`, Ctrl+N `,`, Ctrl+P `"`, Ctrl+2 `@` (repeat the last command) |
| hold N at the title | text only, no pictures |
| Esc | quit |

`SAVE` and `LOAD` use `hobbit-save.tap`; `--scale N` sets the window
size. The game runs at the original's speed (the idle timer that types
`WAIT` for you keeps its timing), except that pictures draw at once.
`build/hobbit` plays the faithful port with the same options.

## 🐛 The bugs the clean edition fixes

Forty years of players have walked into these. All are kept in the
faithful port; the full table, with tests, is in [`docs/CLEAN.md`](docs/CLEAN.md).

- 💥 **`SAY "HELLO` then `@`** sends half a sentence past the quote check, and the Spectrum resets.
- 🌀 **`TIE ROPE TO SWORD`** with both on the floor puts you inside yourself, and the game hangs.
- 🏃 **The speedrunners' "game end glitch"**: 32 words in a line overwrite the game's own code ([TASVideos #9695](https://tasvideos.org/9695S)).
- ⚔️ **Fights end in one blow**: a negative roll is clamped to zero, so strength barely matters.
- 🏔️ **Strong characters can't lift anything**: the weight check tests a sign flag.
- 🌊 **`PUT X IN RIVER`** reads the location of whoever moved last, and says "i cannot do that".
- ❓ **"which ting ?"**: asking which of two objects you meant prints an address as a word.
- 📦 **Climb into the chest and close it**: it's dark, you can't see the lid, and you're in there for good.

## 🧪 How it is checked

`hobbit-ref` is a minimal 48K Spectrum (a vendored Z80 core) that runs
the original game from a script of commands and prints what it writes.
With the ported routines it has three more modes:

- **`--check`**: on every call of a ported routine, the original and the
  C version both run from the same state, and their results are compared:
  the output registers, SP and PC, all 64K of memory, what was printed,
  and the keys read. The run carries on from the original's result, so a
  mismatch is reported without derailing what follows. `--strict-stack`
  also compares what each routine leaves below its return address, and
  every flag, including the undocumented bits 3 and 5 of F: a pushed AF
  can reach memory, and the RNG reads all of memory, so over a long game
  they change what the characters do.
- **`--hybrid`**: the C routines in place of the originals, the Z80
  running whatever is not ported.
- **`--native`**: only the C code, no Z80 at all. `build/hobbit` is this
  with a window.

The clean edition is checked three ways: with `--original-bugs` its
transcripts must match the faithful port's; each clean routine, put in
the faithful one's place, must give the same transcripts; and
`--check-clean` runs both versions of each routine from the same state,
also with mutated inputs, and compares their effects.

```sh
make quick                  # 12 scripts, native, under 1 s
make test STRICT=1          # 62 scripts, every call checked, ~10 s
make test RNG=clean         # the clean edition, three ways, ~11 s
make test STRICT=1 FULL=1   # also hybrid, the original alone, and a determinism rerun
```

Fuzzing with random commands from the game's vocabulary: natively, 11 of
12 long games give exactly the original's transcript, crashes and hangs
included. The clean edition with `--original-bugs` matches the faithful
port on 391 of 400. With the bugs fixed, none of the 400 crashes, hangs
or prints memory as text.

<details>
<summary><b>More tools</b></summary>

```sh
tests/coverage.sh TAPE.tzx 48.rom                 # lines of port/ no test reaches
HOBBIT_NATIVE=1 tests/fuzz.sh TAPE.tzx 1 12 1000  # random commands: check mode, and native vs original
tests/fuzz-clean.sh TAPE.tzx 1 100                # the clean edition alone vs the faithful port
tools/make-demo.sh                                # docs/demo.gif and docs/demo.mp4
tools/refs.py ADDR...                             # every instruction that reaches an address
tools/audit.py                                    # addresses the C code reaches with no C version
```

`hobbit-ref --dump-mismatch FILE` saves the state before the first
mismatching call, and `hobbit-replay FILE` prints the original's path
from there. Put a Spectrum 48K ROM at `48.rom` for results that match
real hardware: the game's random number generator reads the ROM too.
Tests use it when it is there (`tests/expected-rom`).

</details>

## 🔍 Things found on the way

- 🎲 **The random number generator (`$9CA8`) reads the whole 64K**: ROM,
  screen and game state. What's on the screen is part of the game's
  behaviour, which is why the faithful port must match every byte.
- ⌨️ **The random seed is the Z80's refresh register**, read after the
  "press any key" wait: on real hardware, how long you wait at the title
  decides your game.
- ⏱️ **The idle timer counts keyboard scans**, not time.
- 🕹️ **Hidden keys**: as the first key of a line, 5-8 type a direction
  and ENTER; 0 deletes; CAPS SHIFT+0 clears the line; `@` repeats the
  last command.
- 🗺️ **The map's secret route**: in a quarter of games one exit is taken
  out at the start and put back only when Elrond reads the moon-letters.
  Version 1.1 was reported unwinnable that way; in 1.2 it's the puzzle.
- 📍 **A null pointer at `$95DF`** reads ROM byte `$0007` as object flags.
- 🧨 **A long enough sentence** builds parser records over the location
  table: locations 7-11 now point into the ROM, with no exits, and
  Gandalf's random walk there tries for ever.

<details>
<summary><b>Where the faithful port can differ from the original</b></summary>

- When the original overwrites its own code: a word of more than 20
  letters overflows the output buffer at `$74A6` into the code that
  follows, and a line of more than 31 words overflows the tokens at
  `$709C` into the code from `$70E2`. The original runs the damaged code
  (the speedrunners' glitch, which `hobbit-ref` without `--native`
  reproduces); the native program carries on as if the code were intact.
- The seed after a restart: native code doesn't count instruction
  fetches, so R differs. Scripts that test restarts use `--seed`.
- AF' is not kept (no code reads it before writing it), nor the Z80's
  internal MEMPTR, from which `BIT n,(HL)` sets flag bits 3 and 5.

The clean edition with `--original-bugs` differs where a game already
off the rails prints memory that the original's stack has written over:
the clean edition has no Z80 stack (9 of 400 fuzzed games). See
[`docs/CLEAN.md`](docs/CLEAN.md).

</details>

<details>
<summary><b>The game's data, as worked out while porting</b></summary>

- **Objects**: index at `$C063` ([id][record pointer], `$FF` ends;
  characters are `$3C` up: Gandalf `$3E`, Thorin `$3F`). Record: +0 number
  of locations (listed from +`$10`), +1 container, +2 size, +3 weight,
  +4 bits 4-7 the "contains" message, +5 strength, +6 defence, +7
  attributes (7 visible, 6 a character, 5 open, 4 light, 3 broken/dead,
  2 full, 1 liquid, 0 locked), +8 name (three dictionary words), then
  (action, handler) pairs to `$FF` after the locations; action 0 runs
  every turn.
- **Locations**: one word per location at `$B9E0`, pointing at the record:
  +0 attributes (7 lit), +1 capacity, +2 name, then exits from +`$0A` as
  [direction][door][destination] to `$FF` (directions 1-10: N S E W NE
  NW SE SW up down).
- **Characters**: `$CACB`, 7 bytes each (id, entries, current entry,
  script table); scripts at `$C8xx`-`$CA83`, run by `$980E`.
- **Timed events**: `$CA84`, 7 bytes each (flag, countdown, routine at
  zero, threshold, routine below it); location events at `$C78E`.
- **The current command**: action `$B6E7`, objects `$B6E8`/`$B6E9`, actor
  `$B6EA` (0 = you), their records at `$B708`/`$B70A`/`$B70C`. Every
  handler runs twice: first to try (`$B6FA` = 0; `$9D44` abandons it
  there), then to do it.
- **Actions**: `$AB4B` + 8 × id: the words that make the action's message.
- **Speech**: 8 slots of `$19` bytes at `$B738`: what characters were told.

</details>

<details>
<summary><b>What is ported, file by file</b></summary>

Everything the game runs: 338 routines. The only addresses the C code
hands over to without a C version are the original's own crashes
(`tools/audit.py`): `$B301`, message text that one handler can jump into,
and `$824B`, reached only when the drawing code is partly disabled.
Eleven agents ported the areas in parallel, one file each.

| File | Range | Routines | What |
| --- | --- | --- | --- |
| `port/parser.c` | `$6DD6`-`$6FD2` | 3 | The input line, tokenizer and dictionary lookup |
| `port/text.c` | `$70E2`-`$7573` | 30 | Action records, the idle-timer keyboard read, messages and their tokens |
| `port/sentence.c` | `$7585`-`$793C`, `$8251`-`$838E` | 4 | The sentence parser and the special words |
| `port/executor.c` | `$7960`-`$7F77` | 38 | Carrying out sentences: object matching, "which ?" questions, orders to characters |
| `port/actions1.c` | `$8C4B`-`$93D9` | 43 | Look, take, drop, move, run, follow, throw, talk, shoot, death, inventory, attack, give, open, close, fill, eat, drink, break |
| `port/actions2.c` | `$93DA`-`$9A84` | 29 | Examine, light, location descriptions, the turn, the character script interpreter |
| `port/objects.c` | `$9A85`-`$A137` | 58 | Object, location and character lookups, containment, exits, the random number generator |
| `port/actions3.c` | `$A138`-`$A540` | 33 | Exits, tie, untie, burn, locks, the ring, capture, goblins |
| `port/events.c` | `$A541`-`$AB52`, `$C7A4`-`$C7FB` | 53 | Climbing in and out, and the characters' and places' events: trolls, Gollum, the dragon, the barrels, the spiders, Elrond, the magic door |
| `port/io.c` | `$8576`-`$8C4A` | 16 | The text window, the input line, scrolling, the two fonts, the ZX Printer, GetKey |
| `port/drawing.c` | `$7F78`-`$8250` | 16 | The pictures: lines, flood fill, attribute painting |
| `port/meta.c` | `$8391`-`$8575` | 12 | SAVE, LOAD, QUIT, HELP, SCORE, PAUSE |
| `port/main.c` | `$6C00`-`$6DD5` | 3 | The start, the restart point, the main loop |

</details>

## 🗂️ Layout

| Path | What |
| --- | --- |
| [`port/`](port) | The faithful port: each routine keeps the original's register interface (`port/cpu.h`); `port/routines.c` lists them by address. |
| [`clean/`](clean) | The clean edition: the game in ordinary C ([`docs/CLEAN.md`](docs/CLEAN.md)). |
| [`native/`](native) | The SDL programs: `hobbit.c` (faithful) and `hobbit-clean.c` (clean), sharing `window.c`. |
| [`common/`](common) | Loads the game from the TZX into its 64K memory map, as the loader does. |
| [`ref/`](ref) | The reference machine: a minimal 48K Spectrum that runs the original code, with scripted typing and text capture; the hybrid and checking modes; the clean edition's headless host. |
| [`tests/`](tests) | Command scripts, expected transcripts, and the test, coverage and fuzz runners. |
| [`tools/`](tools) | Python helpers: tape image, dictionary, save editing, fuzz scripts, cross-references, the demo. |
| [`docs/PORTING.md`](docs/PORTING.md) | The rules for porting a routine and testing it. |

## 🙏 Credits

- *The Hobbit* by **Philip Mitchell** and **Veronika Megler**, Melbourne
  House, 1982. This project contains none of the game; you need your own copy.
- [**pobtastic/hobbit**](https://github.com/pobtastic/hobbit): the
  annotated SkoolKit disassembly this port follows, address by address.
- [**superzazu/z80**](https://github.com/superzazu/z80): the Z80 core in
  `third_party/z80` (MIT), used only by the reference machine.
- The [TASVideos](https://tasvideos.org) runners, for finding the glitch
  that ends the game.
