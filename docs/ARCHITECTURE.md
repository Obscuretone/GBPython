# GBPython Architecture

## The machine

The DMG Game Boy gives us a 4 MHz LR35902, 8 KB of work RAM (WRAM), 8 KB of
video RAM, and whatever the cartridge brings. GBPython ships as an
MBC5 + RAM + battery cart: 32 KB of ROM in two 16 KB banks and 32 KB of
cartridge SRAM in four 8 KB banks, switchable at 0xA000-0xBFFF.

## ROM layout

| File | Bank | Contents |
|---|---|---|
| `gbpython.c` | 0 (fixed) | Splash, on-screen keyboard, input editor, `input()` line editor |
| `lexer.c` | 0 (fixed) | Tokenizer with INDENT/DEDENT tracking |
| `runtime.c` | 0 (fixed) | SRAM arena allocators, list/dict accessors, string helpers, value rendering, output window, error machinery |
| `interpreter.c` | 1 (banked) | Parser, environment, evaluator, `run_interpreter` |

Bank 0 is always mapped, so anything in it is callable from anywhere —
that's why the lexer and runtime live there: the bank-1 parser/evaluator
calls them constantly with zero overhead. The only cross-bank entry point
is `run_interpreter`, declared `BANKED`; GBDK generates the trampoline that
switches ROM banks around the call.

Bank switching for SRAM (`SWITCH_RAM`) is just an MBC register write and
works from any ROM bank, so the bank-0 runtime can freely reach into
banked SRAM.

## SRAM layout (4 x 8 KB banks at 0xA000)

| Bank | Contents |
|---|---|
| 0 | unused (reserved) |
| 1 | Two AST arenas: the **per-run arena** grows up from 0xA000 and is wiped after every run; **def subtrees** are allocated downward from 0xC000 and persist, which is what makes function definitions survive across runs like a real REPL. The parser sets `def_mode` while parsing a `def`, steering every node of that subtree to the persistent side. The two arenas colliding raises `MemoryError`. |
| 2 | String arena. All stored strings (variables, list/dict elements, concat results) live here; string literals live in their AST node in bank 1 and are copied over on first store. Strings are immutable, so pointers are shared freely. |
| 3 | List and dict arena. Lists: `[len:i16][type:u8, value:i16] * len`. Dicts: chained blocks of 4 entries (`[count:i16][next:i16][ktype,kval,vtype,vval] * 4`) so a dict's head address never changes — Python aliasing (`e = d; e[1] = 2` visible through `d`) falls out of that for free. |

Arenas only grow; nothing is freed until `MemoryError` wipes everything
(variables, functions, all arenas) and reports `(state cleared)`.

A hard-won detail: SRAM must stay **enabled** for the whole parse/eval
cycle. On real hardware (and emulators that model it, like PyGameBoy),
writes to disabled SRAM are silently dropped — an early version disabled
SRAM inside the node allocator and corrupted every AST it built.

## Value model

A value is a 16-bit `int` plus a type tag (`TYPE_INT`, `TYPE_STR`,
`TYPE_LIST`, `TYPE_BOOL`, `TYPE_NONE`, `TYPE_DICT`) carried in the globals
`last_eval_type` / `last_eval_str_bank` (the "value channel") that
`evaluate()` maintains. For strings the int is a pointer into bank 1
(literal in the AST) or bank 2 (stored); for lists/dicts it's a bank-3
pointer. Truthiness, rendering, comparison, and membership all dispatch on
the tag — which is how `''`, `[]`, `{}` get to be falsy and `3==3` gets to
echo `True`.

## Execution

`run_interpreter` (Start button) lexes the 254-byte input buffer, parses a
statement list into the bank-1 arena, and walks it. Control flow uses a
signal register (`exec_signal`): `break`/`continue`/`return` set it and
every sequence/loop/call boundary checks it; errors are just a fourth
signal carrying a message buffer, which is how one mechanism gives both
early returns and exceptions that abort the run.

Function calls push locals onto the environment list and record a frame
boundary; assignment searches only the current frame (Python: assignment
creates a local) while reads search the whole chain (fall through to
globals). Locals are freed on return. Recursion works; `fib(10)` is in the
test suite.

The environment and function registry are `malloc`'d in WRAM. The output
window keeps the last 5 printed lines in WRAM and repaints them, so runaway
output can never scroll the GBDK console and wreck the screen layout.

## Testing

`tests/gbharness.py` boots the ROM inside PyGameBoy headlessly and:

- injects program text straight into the input buffer (address parsed from
  the `.noi` symbol file the build emits — the Makefile's `-Wl-m -Wl-j`
  matters, stale symbols once cost an afternoon),
- presses Start and polls `input_len` (the ROM zeroes it when a run
  finishes),
- decodes the background tilemap back into text (tile = ASCII − 0x20;
  the GBDK console uses signed 0x8800 tile addressing),
- and for the end-to-end tests, drives the on-screen keyboard with the
  d-pad, verifying every cursor move against the ROM's own cursor
  variables and every keypress against the input buffer, so a dropped
  joypad edge gets retried instead of corrupting the test.

`make test` runs ~130 checks in about 15 seconds, including the `examples/`
programs (verified against `# expect:` comments), the `input()` builtin
answered interactively, and a regression test that checks every punctuation
key's highlight glyph — because the splash screen once parked its logo
tiles on top of the inverted font.
