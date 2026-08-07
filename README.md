# GBPython

A Python interpreter that runs on the Nintendo Game Boy (DMG).

Programs are typed on an on-screen keyboard and executed by a tree-walking
interpreter running on the Game Boy's 4 MHz LR35902 CPU, with the AST,
strings, lists, and dicts arena-allocated in banked cartridge SRAM. It is,
within the limits of a machine with 8 KB of work RAM, a real Python: real
`True`/`False`/`None`, real exceptions, floor division, dicts, closures'
poorer cousin (functions with frame-scoped locals and recursion), and a REPL
that echoes exactly like CPython's.

![GBPython REPL](docs/screenshot.png)

```python
def fib(n):
    if n<2: return n
    return fib(n-1)+fib(n-2)
```

Press Start. Then just call `fib(10)` in your next program — definitions
persist across runs, like a real REPL.

## Language

- **Types**: 16-bit signed ints, strings, lists (heterogeneous, nestable),
  dicts, `True` / `False`, `None`
- **Operators**: `+ - * / // %` with Python semantics — floor division
  (`-7//2 == -4`), modulo takes the divisor's sign (`-7%2 == 1`).
  `/` behaves like `//` (there are no floats).
- **Comparisons** produce real bools (`3==3` echoes `True`), chain like
  Python (`1 <= x <= 10`), compare strings lexicographically and lists by
  content; mixed-type `==` is `False`, mixed-type ordering is a `TypeError`
- **Membership**: `in` / `not in` for lists, dict keys, and substrings
- **Logic**: `and` / `or` / `not` with Python value semantics and Python
  truthiness (`''`, `[]`, `{}`, `0`, `None` are falsy)
- **Control flow**: `if` / `elif` / `else`, `while`,
  `break` / `continue` / `pass`,
  `for i in range(...)` (1-3 args), `for x in <list|string|dict>`
- **Functions**: `def name(a, b):` with `return`, recursion, arity checking,
  and Python-faithful scoping (assignment in a function creates a local;
  reads fall through to globals). Max 4 parameters. Definitions persist
  across runs.
- **Assignment**: plain, augmented (`+= -= *= /= %=`), multiple
  (`a, b = 1, 2`), and the swap idiom (`a, b = b, a`)
- **Strings**: `+` concatenation (max 32 chars), indexing and slicing
  (`s[i]`, `s[1:4]`, negative indices), iteration
- **Lists**: `[1, 'a', [2]]`, indexing, slicing, `a[i] = v`, `+`
  concatenation, iteration
- **Dicts**: `{'a': 1}`, string/int/bool keys, `d[k]`, `d[k] = v`,
  `KeyError`, key iteration, aliasing works (`e = d; e[1] = 2` is visible
  through `d`)
- **Exceptions** (reported, then the run stops): `NameError: x`,
  `ZeroDivisionError`, `IndexError`, `KeyError`, `TypeError`, `ValueError`,
  `SyntaxError`, and `MemoryError` (which wipes all state)
- **Builtins**: `print()`, `input()` (pauses the program and reads a line
  from the on-screen keyboard), `len()`, `abs()`, `str()`, `int()`,
  `chr()`, `ord()`, `min()`, `max()`, `sum()`
- Indentation-based blocks (spaces), or single-line suites after `:` with
  `;`-separated statements; `#` comments
- REPL echo: top-level expression statements echo as `> value`; `None`
  results echo nothing, exactly like CPython

**Known deviations from CPython**: no floats (`/` floors), strings cap at
32 chars, ints are 16-bit and wrap, the middle operand of a comparison
chain evaluates twice, dict `==` is identity, functions can read enclosing
call frames' locals, and there are no classes, tuples, sets, or `import`.

The output pane shows the last 5 lines printed. Programs are limited to a
254-byte input buffer; the input pane shows the tail of longer programs.

## Controls

| Button | Action |
|---|---|
| D-pad | Move keyboard cursor (wraps at row ends) |
| A | Type character (↵ = newline) |
| Select + A | Shift: type uppercase letter |
| B | Backspace |
| Select (tap) | Space |
| Start | Run program (or submit, inside `input()`) |

## Building

Requires [GBDK-2020](https://github.com/gbdk-2020/gbdk-2020) (expected at
`~/gbdk/gbdk`, override with `GBDK_HOME`).

```sh
make        # builds gbpython.gb
make test   # runs the headless emulator test suite
```

The ROM is MBC5 + 32 KB RAM + battery; it runs in any DMG emulator that
models SRAM banking correctly (and on real hardware via a flash cart).

## Testing

The suite ([tests/run_tests.py](tests/run_tests.py)) boots the ROM headlessly
inside [PyGameBoy](https://github.com/Obscuretone/pygameboy) (expected as a
sibling checkout at `../pygameboy`), injects programs into the input buffer
using addresses parsed from the build's `.noi` symbol file, presses Start,
and asserts on text decoded from the background tilemap. Several tests drive
the real on-screen keyboard end-to-end — including one that answers an
`input()` prompt mid-program. The programs in [examples/](examples/) run as
part of the suite, checked against their `# expect:` comments.

```
132/132 passed
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how the interpreter
fits in 32 KB of ROM and 8 KB of cartridge SRAM.
