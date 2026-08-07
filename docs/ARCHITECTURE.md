# GBPython Architecture

## The machine

The DMG Game Boy gives us a 4 MHz LR35902, 8 KB of work RAM (WRAM), 8 KB of
video RAM, and whatever the cartridge brings. GBPython ships as an
MBC5 + RAM + battery cart: 64 KB of ROM in four 16 KB banks and 32 KB of
cartridge SRAM in four 8 KB banks, switchable at 0xA000-0xBFFF.

## ROM layout (64 KB, 4 banks)

| File | Bank | Contents |
|---|---|---|
| `gbpython.c` | 0 (fixed) | Main loop, on-screen keyboard |
| `lexer.c` | 0 (fixed) | Tokenizer with INDENT/DEDENT tracking, float literals |
| `runtime.c` | 0 (fixed) | Hot path: SRAM arena allocators, `make_node`, list/dict accessors, string helpers, truthiness |
| `interpreter.c` | 1 (banked) | Environment, class registry, evaluator, `run_interpreter` |
| `parser.c` | 2 (banked) | Recursive-descent parser, ROM-baked stdlib modules (`math`, `random` as gbpython source, compiled at import time) |
| `runtime2.c` | 3 (banked) | Cold path: value rendering, output window, deep equality |
| `float32.c` | 3 (banked) | Soft-float: IEEE-754 single precision in 32-bit integer math (GBDK has no float library for the sm83) |
| `builtins.c` | 3 (banked) | The builtin function dispatcher |
| `ui.c` | 3 (banked) | Splash screen, input editor rendering, `input()` line editor |

Bank 0 is always mapped, so anything in it is callable from anywhere —
the hot allocators and accessors live there and cost nothing to call.
Everything else is behind `BANKED` trampolines, which is fine because it
either runs once per statement (parser), once per print (rendering), or
rarely (float ops, builtins). One trap this creates: a `BANKED` function
cannot read ROM string constants from its *caller's* bank, so text
crossing a bank boundary is staged through WRAM buffers first.

Bank switching for SRAM (`SWITCH_RAM`) is just an MBC register write and
works from any ROM bank, so the bank-0 runtime can freely reach into
banked SRAM.

## SRAM layout (4 x 8 KB banks at 0xA000)

| Bank | Contents |
|---|---|
| 0 | unused (reserved) |
| 1 | Two AST arenas: the **per-run arena** grows up from 0xA000 and is wiped after every run; **def subtrees** are allocated downward from 0xC000 and persist, which is what makes function definitions survive across runs like a real REPL. The parser sets `def_mode` while parsing a `def`, steering every node of that subtree to the persistent side. The two arenas colliding raises `MemoryError`. |
| 2 | String arena. All stored strings (variables, list/dict elements, concat results) live here; string literals live in their AST node in bank 1 and are copied over on first store. Strings are immutable, so pointers are shared freely. |
| 3 | List/tuple/dict/set/object arena. Lists and tuples: `[len:i16][type:u8, value:i32] * len`. Dicts (and sets, and object attribute tables): chained blocks of 4 entries (`[count:i16][next:i16][ktype:u8, kval:i32, vtype:u8, vval:i32] * 4`) so a dict's head address never changes — Python aliasing (`e = d; e[1] = 2` visible through `d`) falls out of that for free. |

Arenas only grow; nothing is freed until `MemoryError` wipes everything
(variables, functions, all arenas) and reports `(state cleared)`.

A hard-won detail: SRAM must stay **enabled** for the whole parse/eval
cycle. On real hardware (and emulators that model it, like PyGameBoy),
writes to disabled SRAM are silently dropped — an early version disabled
SRAM inside the node allocator and corrupted every AST it built.

## Value model

A value is a 32-bit `long` plus a type tag (`TYPE_INT`, `TYPE_STR`,
`TYPE_LIST`, `TYPE_BOOL`, `TYPE_NONE`, `TYPE_DICT`, `TYPE_FLOAT`,
`TYPE_TUPLE`, `TYPE_OBJ`, `TYPE_SET`) carried in the globals
`last_eval_type` / `last_eval_str_bank` (the "value channel") that
`evaluate()` maintains. Floats travel as IEEE-754 bit patterns in the same
slot, with all arithmetic done by the hand-rolled soft-float in
`float32.c`. For strings the value is a pointer into bank 1 (literal in
the AST) or bank 2 (stored); containers are bank-3 pointers. Truthiness,
rendering, comparison, and membership all dispatch on the tag — which is
how `''`, `[]`, `{}` get to be falsy and `3==3` gets to echo `True`.

## Execution

`run_interpreter` (Start button) lexes the 254-byte input buffer, parses a
statement list into the bank-1 arena, and walks it. Control flow uses a
signal register (`exec_signal`): `break`/`continue`/`return` set it and
every sequence/loop/call boundary checks it; errors are just a fourth
signal carrying a message buffer, which is how one mechanism gives both
early returns and exceptions that abort the run.

Function calls (plain calls, methods, and `__init__` all share
`invoke_function`) push locals onto the environment list and record a
frame boundary; assignment searches only the current frame (Python:
assignment creates a local) while reads see the current frame and then
jump straight to the globals — never other frames' locals (lexical
scoping). Locals are freed on return; recursion is capped at depth 16
with `RecursionError`. Instances are bank-3 dicts whose first entry is a
hidden class link under a `TYPE_NONE` key — unreachable from user code,
so aliasing and attribute storage fall out of the dict machinery.

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

`make test` runs ~180 checks in about 15 seconds, including the `examples/`
programs (verified against `# expect:` comments), the `input()` builtin
answered interactively, and a regression test that checks every punctuation
key's highlight glyph — because the splash screen once parked its logo
tiles on top of the inverted font.
