# Porting guide

How routines are ported and checked. README.md has the overview; read
`port/cpu.h`, `port/routines.h`, and `port/parser.c` / `port/sentence.c`
(finished examples) before starting.

## Sources

- `reference/pobtastic-hobbit/sources/hobbit.skool`: the disassembly.
  Addresses are hex (`$7960`). Annotations are good in places and absent
  in others; some data is disassembled as code and some code as data
  (`$8271`-`$82B2` is a table followed by code, for instance), so check
  odd-looking instructions against the bytes (`tools/mem.bin` is the
  memory image at startup: `tools/mkimage.py TAPE tools/mem.bin`).
- `tools/refs.py ADDR...`: every instruction that reaches an address.
- `tools/dictionary.py TAPE`: the vocabulary with word classes.

## What a ported routine is

A C function `void name(Cpu *c)` that does what the original does from
its entry address to its return, to the registers in `c`, the flags, the
memory at `c->mem` (the original 64K map), and the stack at `c->sp`. It
is registered in its file's table:

    const PortRoutine executor_routines[] = {
        {0x7960, "Execute", p_execute, OUT_REGS | OUT_ZF | OUT_CF},
        ...
        {0, NULL, NULL, 0},
    };

When the game reaches a registered address, hybrid mode runs the C
function instead and then pops the return address, as RET would. Check
mode runs both from the same state and compares all of memory (except
stack scratch below the return address), SP, PC, what was printed, keys
used, and the registers in the `OUT_` mask.

**Be faithful, not just equivalent.** Leave every register, flag and byte
of memory the way the original leaves it, including scratch values and
garbage, unless you have read every caller and are sure it is not used.
The default mask is `OUT_REGS | OUT_ZF | OUT_CF` (add `OUT_SF`/`OUT_PF`
if a caller tests them, `OUT_ALT` if the routine uses EXX). The memory
check is always strict: buffers, counters, scratch bytes all count.
Remember the random number generator reads all of memory, so a stray
byte difference changes the game later.

## Which addresses to register

Only addresses where every way in leaves a return address on top of the
stack: `CALL`s, and jumps that act as tail calls (`JP X` as the last
thing a routine does, with nothing of its own left on the stack). Check
with `tools/refs.py`. Jumps back to the entry from inside the routine
itself are fine (the C version runs the whole routine). An address that
other code reaches by falling through, or by a jump with something
pushed, must not be registered; leave it as original code, or register
the routine that contains it.

Register every routine in your range that is CALLed, not just the big
ones: each is then checked on its own. Also register continuation points:
addresses that ported code hands over to with `cpu_tail`/`cpu_jump` (for
instance after popping a return address, the stack top is the caller's
caller's return address, so the code from there behaves as a routine).
The goal is a build without the emulator, where every address the C code
reaches must have a C version: `tools/audit.py` lists the ones that do
not yet.

## Calling other code

- Another routine (yours or anyone's, ported or not): `cpu_call_at(c,
  site)`, where site is the address of the original `CALL nn` instruction
  you are translating, with the registers set up as the original does. It
  runs that CALL, so the stack gets the real return address (the random
  number generator reads stack bytes too), and it copes with callees that
  take arguments off the stack (some `$72DD` messages) or never come back
  (a death restarts the game: your routine is then abandoned, as the
  original's was). A conditional CALL can be run the same way (the flags
  decide, as in the original), or test the condition in C and then
  `cpu_call_at`. The CALL at site always runs as original code, so
  this works when site is your own routine's entry address. `cpu_call(c, addr)` pushes a sentinel instead of the
  real return address: use it only where there is no CALL to point at. Do
  not call another file's C functions directly; it keeps routines
  independently checkable.
- Helpers that are part of your routine and not registered: plain static
  C functions.
- Original code that does not come back the normal way (a jump into the
  middle of other code, an indirect jump to code you have not ported):
  `cpu_jump(c, addr, resume)` runs it until it reaches `resume` with the
  stack as it is now (returns true; carry on in C), or until it leaves
  your routine's frame (returns false; return at once). `cpu_tail` is
  the same with no resume point.
- Indirect jumps (`JP (HL)`, `JP (IY)`): read the target from memory as
  the original does, `switch` on the targets you have ported, and
  `cpu_tail` to anything else.
- I/O ports: `c->in_at(c, port, pc)` for an IN at address pc (always use
  this for the keyboard: the test typist presses keys only when the game
  is reading them at a waiting point, which it recognises by pc),
  `c->out(c, port, value)` for OUT (the border, the printer).
- Watched addresses: where your C passes through `$86A1` (a character
  printed in the text window), `$85B8` (on the input line), `$8B93` (a
  GetKey scan) or `$6CAC` (the seed is set) itself, rather than through
  `cpu_call_at`/`cpu_jump` or by being registered there, call
  `cpu_at(c, addr)` with the registers as the original has them. The
  transcripts are made from these.
- The game's start (`$6C00`) and restart point (`$6C27`) are registered
  with `PORT_TOP` (fifth field of the table entry): they never return, and
  any jump to them, from anywhere, abandons everything running, as the
  original's `JP $6C27` / `LD SP,$5EFF` does. Code that restarts the game
  just does `cpu_tail(c, 0x6C27)`.

## Stack quirks

The original sometimes pops a return address to escape a level, or leaves
something pushed across a RET, or keeps state on the stack between
routines. Where the effect stays inside your routine, structured C is
fine (see `fail()` in `port/sentence.c`). Where it reaches outside
(something left on the stack when you return, a value read off the
stack that a caller pushed), use `push16`/`pop16`/`rd16(c, c->sp)` on the
real stack, so the result is the same. `ex_sp_hl` is EX (SP),HL.

## Writing the code

Translate closely first: keep the original's labels as `goto` targets
(`L7A2E:`), registers as `c->a` etc., flags through the `op_` helpers in
`cpu.h`. Name things (`#define` addresses at the top of the file, with a
comment) as you work out what they are, and write a comment for each
routine saying what it does. Do not restructure beyond what you are sure
of. Watch for self-modifying code (writes into code addresses): read the
operand from memory when it can change.

Do not edit files outside your own `port/` file(s), new test scripts
`tests/scripts/AREA-*.txt` and their expected output, unless asked. Need
a helper in `cpu.h`? Write it as a static function in your file and say
so in your report.

## Testing

While working, `make quick BUILD=build/AREA ONLY=...` (a dozen scripts, under
a second); the full `make test` before saying something is done.

Use your own build directory and limit check/hybrid to your routines:

    make BUILD=build/AREA
    make test BUILD=build/AREA ONLY=7960,79B6,7AED
    BUILD_DIR=$PWD/build/AREA HOBBIT_ONLY=7960,... \
        tests/coverage.sh "Hobbit, The v1.2 (1982)(Melbourne House).tzx" 48.rom port/AREA.c
    BUILD_DIR=$PWD/build/AREA HOBBIT_ONLY=7960,... \
        tests/fuzz.sh "Hobbit, The v1.2 (1982)(Melbourne House).tzx" 1 4 600

`make test` needs every script to give the expected transcript in check
and native modes with no mismatches (`FULL=1` adds hybrid, the original
alone and a determinism rerun). Write scripts that reach your
code (`tests/scripts/AREA-*.txt`, first line `#opts --seed N`); the first
run writes their expected output (`WROTE`), which you should read to see
that the game did what you meant. Aim for full line coverage of your
file, and explain what is left. Fuzz with at most 4 seeds at a time (the
machine is shared). For a mismatch: `--dump-mismatch FILE` saves the state
before the call, `build/AREA/hobbit-replay FILE` traces the original from
there, and `--trace-keys` shows the typing.

Known behaviour of the original: `SAY "HELLO` then `@` crashes it (a
reset); some fuzz seeds reach infinite loops in the object code around
`$9B9C`-`$9F0F` (reported as hangs by `tests/fuzz.sh`); dying needs a key
(`@key  `) before the next command.
