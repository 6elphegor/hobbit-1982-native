# The clean edition

The faithful port (`port/`) reproduces the original down to the Z80
registers and the bytes it leaves on the stack, because the original's
random number generator reads all of memory. The clean edition
(`clean/`) is the same game in ordinary C, with a proper random number
generator (`clean/rng.c`), which frees it from all of that.

It runs on its own: `build/hobbit-clean` (headless, from a script, as
`hobbit-ref` runs the original) and `build/hobbit-clean-sdl` (in a
window) are built from `clean/` and a host, with no `port/` code and no
Z80 code run.

It fixes the original's bugs (below). The faithful port keeps them all;
the clean edition keeps them too with `--original-bugs`
(`original_bugs` in `clean/data.h`), which is how it is checked against
the faithful port, so each fix is written as the original's behaviour
behind that switch and the fixed behaviour after it.

## What clean code looks like

- Functions with real arguments and return values; no Z80 registers, no
  `Cpu`, no flags, no stack.
- The game's data stays in the memory image, in the tape's formats, and is
  reached through `clean/data.h`: record structs (`ObjectRecord`,
  `LocationRecord`, ...) and named variables (`V_ACTION`, ...). Modules
  name their own in their headers.
- Structured control flow, named things, a comment per function saying
  what it does. The original's stack tricks (a routine popping its
  caller's return address to abandon it, say) become return values.
- Keep every effect the game can see: the same writes to game data, the
  same text printed, the same keys read, in the same order, and the same
  random numbers asked for in the same order with the same ranges (the
  generator is shared, so asking in a different order changes the game).
  With `original_bugs`, the bugs too: where the original reads what a
  lookup left behind (a nonexistent object compares as `$FF`, a
  nonexistent location's name is read from the message being printed),
  the clean code says so and does the same.

## The bugs it fixes

Without `--original-bugs` (the default), the clean edition fixes these.
The faithful port, and the clean edition with the switch, keep them.

| Bug in the original | Fixed | Test |
| --- | --- | --- |
| `SAY "HELLO` refused, then `@` sends its tokens to the parser without the quote check: it returns into a stacked word and the Spectrum resets | `@` refuses a refused line again ("what ?") | `crash-repeat` |
| A word out of place after ALL ... EXCEPT (`TAKE ALL EXCEPT QUICKLY`, `ALL BUT AN ALL`) moves the parser above its first record, onto the location table at `$B9E0`: places then point into the ROM (darkness, hangs) | the word is refused | `sentences` |
| A finished object or exit search, resumed, steps past the end of the index into the records: objects that do not exist (`$C0`, `$FF`); killing one empties everything in nothing into the spider web, the web into itself, and the next walk loops | an ended search stays ended | `fixes-object` |
| A token class past `$C0` (junk read after the line) runs the jump table into the ROM | the end of the line | (fuzzing) |
| A line of more words than the 32 token slots writes on over PrintMsg's variables and the code after ($70E2 on), which the original then runs: the speedrunners' "game end glitch" (`OP CH`, `,.,.,.` x20 and `F F F F F F`, `F`, `PUT TRE CH` wins the game; TASVideos #9695) | refused ("what ?") | |
| A word over 20 letters (read from garbage) overflows the buffer at `$74A6` into code | stopped at 20 | |
| `GO INTO` an object whose exit is not "down" jumps to `$B301`, message text, as code | that message is printed | |
| `TIE ROPE TO X` with X not carried: the take overwrites the register holding the rope's record, so the actor is put in itself (`$A2A2`) and the next walk loops | the rope is carried | `fixes-tie` |
| RUN where no direction has an exit tries for ever | the action fails | `actions3-goblins`, `actions3-goblin2` |
| With no object, `$95DF` reads object flags from ROM address `$0007` | nothing (no visible change with the Sinclair ROM) | |
| `PUT X IN RIVER` takes the location of the last character to have had its turn as the actor's: "i cannot do that" | the actor's location: swept away | `fixes-river` |
| Fight rolls (`$9213`) take the carry of adding a negative roll for an overflow: rolls mostly come out as 0, fights end in one blow | v ± 10, clamped to 0-255 | `drawing-lake`, `objects-contents` |
| The lift check (`$8D1A`) tests the sign of what is left: strong characters are told they carry too much | compared as numbers | `fixes-lift` |
| "take X out of Y" works for anything carried (`$8CE0` tests the wrong flag) | only if X is in Y | `actions1-home` |
| "which ?" about the second object asks with the phrase's first byte and its address: "which ting ?", and the answer fails | "which troll ?" | `fixes-which` |
| A returning goblin (`$A448`) compares your location with a register left at 0, so "the goblin is here" is never said | its new location | (by reading) |
| Closing a container you are in (`CLIMB INTO CHEST`, `CLOSE CHEST`): it is dark, you cannot see it to open it, nor climb out, for good | you cannot close it on yourself | `fixes-chest` |

Not changed: `$9B44` treating an object used with itself ("put the box in
the box") as acting on oneself: that is a guard, not a bug. And the map's
secret route: at the start one of four exits (in a quarter of games, east
from Long Lake, `$22`) is taken out, and put back only when Elrond reads
the map (`$97AD`, `$A7C4`). Version 1.1 was reported unwinnable that way;
in 1.2 it is the puzzle (Elrond reads the moon-letters, as in the book),
though if Elrond dies first that game cannot be won.

Tests: `tests/expected-fixed` has the transcripts that a fix changes (the
`fixed` mode of `tests/run.sh`; every other script must give
`tests/expected-clean` there too); the scenes the tests start from are
saved games made at test time (`#scene EDITS` in a script: the game
saved at the start, edited by `tools/editsave.py`), so that no game data
is kept in the repository. With the fixes, no fuzzed game (400 seeds) crashes, hangs
or prints memory as text.

## The modules

`game.c` (the start, the restart, the main loop and the commands of
their own), `parser.c`, `executor.c`, `actions.c`, `characters.c` (the
turn, the characters' scripts, the events), `objects.c`, `text.c`,
`screen.c`, `drawing.c`, `rng.c`. They call each other directly, through
their headers.

`handlers.c`: the game's data names routines by their address in the
original (each object's handlers for actions, the default handler of each
action, the timed events', the locations' and those in the characters'
scripts); `run_routine(addr)` looks them up in a table of the clean
functions (every address the tape's data names, from a scan of its
tables and scripts, and the others reached the same way). An address not
in the table is a crash (as it is in the original, which would run
whatever is there).

## The machine: `platform.h`

What the game asks of the machine, supplied by the host:

- `device_in`, `device_out`: the keyboard's half-rows, the border, the ZX
  Printer. Each read names the original's IN instruction (`at`), by which a
  test machine tells what kind of wait it is.
- `device_at`: the game is where the original would be at an address (a
  character printed, the seed chosen): test machines watch these.
- `device_random_byte`: a byte that differs from run to run (the seed).
- `device_tape`: a tape block saved, loaded or verified.
- `device_restart`, `device_crash`, `device_hang`: start the game again
  (every death, QUIT and tape error), the original jumping where there is
  no code (its crashes), and the original looping for ever (its known
  infinite loops); none returns.

Hosts: `ref/cleanhost.c` (the clean edition alone, on the test machine's
Spectrum for its keyboard, typist, tape file and screen; used by
`ref/cleanrun.c`, which is `hobbit-clean`, and `native/hobbit-clean.c`),
and `ref/hybrid.c` (inside `hobbit-ref`, while the faithful port is
there: through the Cpu of the routine running).

## The adapters, and how it is checked

Each clean routine can also stand in for a faithful one, at the same
address, through an adapter: a function `a_name(Cpu *c)` at the bottom of
its file that takes the faithful routine's inputs from the registers,
calls the clean code, and puts its results where the faithful callers
look for them. The adapters (and the tables of them, `clean.c`) are
compiled only with `CLEAN_ADAPTERS`, which every build defines but the
clean edition's own.

`make test RNG=clean` runs every script three ways (`FULL=1`: and
`clean`) and needs each to give
the transcript in `tests/expected-clean` (written by the faithful port run
with the clean generator, `hobbit-ref --native --rng clean`):

- `alone`: `hobbit-clean --original-bugs`.
- `fixed`: `hobbit-clean`, the bugs fixed: `tests/expected-fixed` where a
  fix changes the transcript.
- `clean` (with `FULL=1`): `hobbit-ref --native --clean`, the clean
  routines in the faithful ones' places (through the adapters).
- `check-clean`: `hobbit-ref --native --check-clean`: for each clean
  routine called by faithful code (the first 20 calls of each, and each
  of those 4 more times with other inputs), both versions run from the
  same state, and their effects are compared: game memory (except the
  stack and the scratch its module declares), the registers in the
  adapter's `outputs` mask, what was printed and the keys read. The run
  carries on from the faithful result. `--check-limit 0` checks every
  call (then only the outermost routines are compared: the clean ones
  call each other directly); `--clean-skip LIST` leaves routines to the
  faithful port, so that the routines inside them are checked instead.
  `--dump-mismatch FILE` works here too.

An adapter's `outputs` mask is what its callers really use (read them
with `tools/refs.py`), and its `fixed` mask the inputs the check must not
mutate: those every caller sets to one of a few constants (which the
adapter decodes), or keeps in a range the original never leaves. Say why
at the adapter. Scratch: bytes a routine uses only internally (a byte of
its own code it rewrites, say) can be kept in C locals instead; list
their addresses in the module's `_scratch` table with the reason.

`tests/fuzz-clean.sh TAPE FIRST LAST` compares `hobbit-clean
--original-bugs` with `hobbit-ref --native --rng clean` on random games
(`tools/fuzz.py`), and checks that the fixed edition neither crashes nor
hangs on them.

## Known differences from the faithful port (with --original-bugs)

- When the game, already off the rails (its tables damaged by random
  play: a location that does not exist, a location table entry
  overwritten), prints memory as text: where that memory is what the
  original's stack has written over (the clean edition writes no stack),
  or where a message found there takes parameters, which the original
  pops from whatever is on its stack, or runs into bytes the original
  executes as code. 9 of 400 fuzzed games (`tests/fuzz-clean.sh`) end so;
  the other 391 give the faithful port's transcript exactly.
- With `--original-bugs`, `goblin` (`$A448`) compares your location with
  the B register its caller left: the clean edition passes 0, which is
  what B held on every one of 3,628 calls traced in the faithful port
  (the tests and 40 fuzzed games), but no proof that it always is.
- The seed: the original took it from the refresh register R; the clean
  edition takes any byte the machine gives (`device_random_byte`), and the
  test machine's `--seed` replaces it. (The two scripts that test the
  faithful port's R give `--seed` in a `#clean-opts` line.)

## Testing while working

    make BUILD=build/clean-AREA
    make quick BUILD=build/clean-AREA RNG=clean     # the clean edition alone, under a second
    make test BUILD=build/clean-AREA RNG=clean      # everything, ~10 s
    build/clean-AREA/hobbit-ref --rom 48.rom --seed N --native --check-clean TAPE script
    build/clean-AREA/hobbit-clean --rom 48.rom --seed N TAPE script
