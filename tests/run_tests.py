#!/usr/bin/env python3
"""gbpython test suite: boots the ROM headlessly and checks REPL output.

Note: the interpreter environment persists across repl() calls within one
boot, so each case uses fresh variable names (or relies on reassignment).
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

from gbharness import GBPython, OSK_COLS, OSK_X, OSK_Y

EXAMPLES = Path(__file__).resolve().parent.parent / "examples"


def load_example(path: Path) -> tuple[str, list[str]]:
    """Source with comment/blank lines stripped, plus '# expect:' lines."""
    source_lines: list[str] = []
    expected: list[str] = []
    for line in path.read_text().splitlines():
        if line.lstrip().startswith("# expect:"):
            expected.append(line.split("# expect:", 1)[1].strip())
        elif line.lstrip().startswith("#") or not line.strip():
            continue
        else:
            source_lines.append(line)
    return "\n".join(source_lines), expected

# (label, source, expected output lines)
CASES: list[tuple[str, str, list[str]]] = [
    # arithmetic and echo semantics
    ("int literal", "42", ["> 42"]),
    ("precedence", "1+2*3", ["> 7"]),
    ("parens", "(1+2)*3", ["> 9"]),
    ("subtraction", "10-4-3", ["> 3"]),
    ("true division", "17/5;8/2", ["> 3.4", "> 4.0"]),
    ("floor div int", "17//5", ["> 3"]),
    ("floor division", "7//2;-7//2", ["> 3", "> -4"]),
    ("python modulo", "-7%2;7%-2", ["> 1", "> -1"]),
    ("div by zero", "5/0", ["ZeroDivisionError"]),
    ("modulo", "17%5", ["> 2"]),
    ("unary minus", "-4+10", ["> 6"]),
    ("double negative", "--5", ["> 5"]),
    ("assignment silent", "x=7", []),
    ("assignment", "x=7;x*x", ["> 49"]),
    ("reassignment", "x=1;x=x+41;x", ["> 42"]),
    ("two exprs echo", "1;2", ["> 1", "> 2"]),
    # comparisons and logic (comparisons produce real bools)
    ("comparison eq", "3==3", ["> True"]),
    ("comparison neq", "3!=3", ["> False"]),
    ("comparison lt", "2<9", ["> True"]),
    ("comparison gt", "2>9", ["> False"]),
    ("comparison le", "3<=3", ["> True"]),
    ("comparison ge", "2>=9", ["> False"]),
    ("and", "1 and 7", ["> 7"]),
    ("and short-circuit", "0 and 7", ["> 0"]),
    ("or", "0 or 5", ["> 5"]),
    ("not", "not 0", ["> True"]),
    ("true false", "True+True;False", ["> 2", "> False"]),
    ("logic precedence", "1 or 0 and 0", ["> 1"]),
    ("mixed eq is False", "1=='1'", ["> False"]),
    # None and truthiness
    ("bare None silent", "None", []),
    ("print None", "print(None)", ["None"]),
    ("empty string falsy", "if '': print(1)\nif 'x': print(2)", ["2"]),
    ("empty list falsy", "if []: print(1)\nif [0]: print(2)", ["2"]),
    # exceptions
    ("NameError", "zz", ["NameError: zz"]),
    ("NameError stops run", "print(1);zz;print(2)", ["1", "NameError: zz"]),
    ("IndexError", "[1,2][5]", ["IndexError"]),
    ("TypeError mixed add", "'a'+1", ["TypeError: +"]),
    ("NameError call", "gg(1)", ["NameError: gg"]),
    ("ValueError int", "int('abc')", ["ValueError: int"]),
    # while
    ("while silent", "i=0\nwhile i<5: i=i+1\ni*10", ["> 50"]),
    ("while multi-stmt", "n=0;t=0\nwhile n<4: n=n+1; t=t+n\nt", ["> 10"]),
    # strings
    ("string literal", "'hello'", ["> 'hello'"]),
    ("string var", "s='hi';s", ["> 'hi'"]),
    ("string copy", "a='abc';b=a;b", ["> 'abc'"]),
    # print
    ("print number", "print(3*4)", ["12"]),
    ("print string", "print('gb')", ["gb"]),
    ("print empty", "print()", []),  # blank line indistinguishable on screen
    ("print in loop", "k=0\nwhile k<3: k=k+1; print(k)", ["1", "2", "3"]),
    # if / elif / else, single line
    ("if true", "if 2>1: print(1)", ["1"]),
    ("if false", "if 1>2: print(1)", []),
    ("if else taken", "if 1>2: print(1)\nelse: print(2)", ["2"]),
    ("elif chain", "v=5\nif v==1: print(1)\nelif v==5: print(55)\nelse: print(9)", ["55"]),
    # indented blocks
    (
        "indented while",
        "i=0\nt=0\nwhile i<4:\n    i=i+1\n    t=t+i\nprint(t)",
        ["10"],
    ),
    (
        "indented if else",
        "x=3\nif x>5:\n    print(1)\nelse:\n    print(2)",
        ["2"],
    ),
    (
        "nested blocks",
        "i=0\nwhile i<3:\n    i=i+1\n    if i==2:\n        print(i)",
        ["2"],
    ),
    # for / range
    ("for range 1 arg", "for i in range(3): print(i)", ["0", "1", "2"]),
    ("for range 2 args", "s=0\nfor i in range(2,5): s=s+i\nprint(s)", ["9"]),
    ("for range step", "for i in range(10,0,-4): print(i)", ["10", "6", "2"]),
    ("for sum", "s=0\nfor i in range(101): s=s+i\ns", ["> 5050"]),
    # augmented assignment
    ("aug plus", "x=10;x+=5;x", ["> 15"]),
    ("aug minus mul", "x=9;x-=2;x*=3;x", ["> 21"]),
    ("aug div mod", "x=17;x//=2;x%=5;x", ["> 3"]),
    ("aug truediv", "x=7;x/=2;x", ["> 3.5"]),
    # break / continue / pass
    ("for break", "for i in range(9):\n    if i==3: break\n    print(i)", ["0", "1", "2"]),
    ("for continue", "for i in range(5):\n    if i%2==0: continue\n    print(i)", ["1", "3"]),
    ("while break", "i=0\nwhile True:\n    i+=1\n    if i==4: break\ni", ["> 4"]),
    ("pass", "pass", []),
    # functions
    ("def return", "def add(a,b):\n    return a+b\nprint(add(3,4))", ["7"]),
    ("def recursion", "def fib(n):\n    if n<2: return n\n    return fib(n-1)+fib(n-2)\nfib(10)", ["> 55"]),
    ("def local scope", "g=1\ndef f():\n    g=99\n    return g\nf();g", ["> 99", "> 1"]),
    ("def global read", "g=5\ndef f(n): return g+n\nf(2)", ["> 7"]),
    ("def string arg", "def ex(s):\n    return s+'!'\nex('hi')", ["> 'hi!'"]),
    ("def early return", "def f(n):\n    if n>0: return 1\n    return 2\nf(5);f(-5)", ["> 1", "> 2"]),
    # string operations
    ("string concat", "'ab'+'cd'", ["> 'abcd'"]),
    ("string aug concat", "a='ab';a+='c';a", ["> 'abc'"]),
    ("string compare", "'ab'=='ab';'a'<'b'", ["> True", "> True"]),
    ("string index", "'hello'[1]", ["> 'e'"]),
    ("string index neg", "'abc'[-1]", ["> 'c'"]),
    ("string slice", "'hello'[1:4];'hello'[:2];'hello'[3:]", ["> 'ell'", "> 'he'", "> 'lo'"]),
    ("substring in", "'ell' in 'hello';'z' in 'hi'", ["> True", "> False"]),
    ("len str", "len('hello')", ["> 5"]),
    ("abs", "abs(-7)", ["> 7"]),
    # conversions and more builtins
    ("str of int", "str(42)+'!'", ["> '42!'"]),
    ("int of str", "int('12')+1", ["> 13"]),
    ("str of bool", "str(True)", ["> 'True'"]),
    ("chr ord", "chr(65);ord('A')", ["> 'A'", "> 65"]),
    ("min max args", "min(3,1,2);max(3,1,2)", ["> 1", "> 3"]),
    ("min max sum list", "min([5,2,9]);max([5,2,9]);sum([5,2,9])", ["> 2", "> 9", "> 16"]),
    # lists
    ("list literal", "[1,2,3]", ["> [1, 2, 3]"]),
    ("empty list", "[]", ["> []"]),
    ("list index", "a=[5,6,7];a[1];a[-1]", ["> 6", "> 7"]),
    ("list store", "a=[1,2];a[0]=9;a", ["> [9, 2]"]),
    ("list concat", "a=[1];a+=[2,3];a", ["> [1, 2, 3]"]),
    ("len list", "len([4,5,6])", ["> 3"]),
    ("for in list", "s=0\nfor x in [10,20,30]: s+=x\ns", ["> 60"]),
    ("for in string", "for c in 'abc': print(c)", ["a", "b", "c"]),
    ("list fill loop", "a=[0,0,0,0]\nfor i in range(4): a[i]=i*i\na", ["> [0, 1, 4, 9]"]),
    ("list into def", "def head(l):\n    return l[0]\nhead([42,1])", ["> 42"]),
    ("typed list elements", "print(['a',True,[1,2]])", ["['a', True, [1, 2]]"]),
    ("list content eq", "[1,2]==[1,2];[1,2]==[1,3]", ["> True", "> False"]),
    ("list in", "2 in [1,2,3];5 not in [1,2]", ["> True", "> True"]),
    ("list slice", "[1,2,3,4][1:3]", ["> [2, 3]"]),
    ("list of strings", "w=['ab','cd'];w[1]+'!'", ["> 'cd!'"]),
    # dicts
    ("dict literal", "{'a':1,'b':2}", ["> {'a': 1, 'b': 2}"]),
    ("dict get", "d={'a':1};d['a']", ["> 1"]),
    ("dict set new key", "d={'a':1};d['b']=9;d['b']", ["> 9"]),
    ("KeyError", "d={'a':1};d['zz']", ["KeyError"]),
    ("dict int key", "d={1:'one'};d[1]", ["> 'one'"]),
    ("dict in tests keys", "d={'a':1};'a' in d;'z' in d", ["> True", "> False"]),
    ("dict for-in keys", "d={'x':1,'y':2}\nfor k in d: print(k)", ["x", "y"]),
    ("dict len", "len({'a':1,'b':2})", ["> 2"]),
    ("dict aliasing", "d={};e=d;e[1]=2;d[1]", ["> 2"]),
    ("dict grows past a block", "d={}\nfor i in range(9): d[i]=i*i\nd[8];len(d)", ["> 64", "> 9"]),
    # comparison chaining and multiple assignment
    ("comparison chain", "x=5\n1<=x<=10;10<x<20", ["> True", "> False"]),
    ("multi assign", "a,b=1,2;a;b", ["> 1", "> 2"]),
    ("swap", "a,b=1,2\na,b=b,a\na;b", ["> 2", "> 1"]),
    ("unpack ValueError", "a,b=1,2,3", ["ValueError: unpack"]),
    # REPL persistence: defs survive across runs (arena kept)
    ("persistent def", "def seven():\n    return 7", []),
    ("persistent call", "seven()", ["> 7"]),
    ("arity TypeError", "seven(1)", ["TypeError: seven"]),
    # arena exhaustion wipes state cleanly
    ("MemoryError wipe", "s='x'\nfor i in range(400): s=s+'y'", ["MemoryError", "(state cleared)"]),
    ("fresh after wipe", "q=41;q+1", ["> 42"]),
    # floats (soft-float32)
    ("float literal", "3.14", ["> 3.14"]),
    ("float add", "0.5+0.25", ["> 0.75"]),
    ("float mixed arith", "2*3.5;7%2.5", ["> 7.0", "> 2.0"]),
    ("float compare", "1.5<2;2.5>=2.5", ["> True", "> True"]),
    ("float int eq", "1==1.0", ["> True"]),
    ("float floor div", "7.0//2", ["> 3.0"]),
    ("float neg", "-2.5*2", ["> -5.0"]),
    ("float conversions", "float(3);int(3.9);int(-3.9)", ["> 3.0", "> 3", "> -3"]),
    ("float of str", "float('2.5')+1", ["> 3.5"]),
    ("round", "round(2.6);round(-2.6);round(3)", ["> 3", "> -3", "> 3"]),
    ("str of float", "str(1.5)+'!'", ["> '1.5!'"]),
    ("float in list", "[1.5, 2]", ["> [1.5, 2]"]),
    ("float builtins", "abs(-2.5);max(1,2.5);sum([0.5,0.5])", ["> 2.5", "> 2.5", "> 1.0"]),
    # 32-bit ints
    ("big multiply", "30000*30000", ["> 900000000"]),
    ("big fib iterative", "a,b=0,1\nfor i in range(46): a,b=b,a+b\na", ["> 1836311903"]),
    # recursion depth guard
    ("RecursionError", "def rr(n):\n    return rr(n+1)\nrr(0)", ["RecursionError"]),
    # lexical scoping: other frames' locals are invisible
    ("no dynamic scoping", "def qin(): return qq\ndef qout():\n    qq=5\n    return qin()\nqout()", ["NameError: qq"]),
    # dict equality by content
    ("dict content eq", "{'a':1}=={'a':1};{'a':1}=={'a':2}", ["> True", "> False"]),
    # comparison chains evaluate the middle operand once
    ("chain single eval", "def see(x):\n    print(x)\n    return x\n1<see(2)<3", ["2", "> True"]),
    ("long string", "u='0123456789'\nu=u+u+u+u+u+u\nlen(u)", ["> 60"]),
    # comments
    ("comment line", "# nothing\n5 # five", ["> 5"]),
    # output window scrolls, keeps last 5
    ("output window", "for i in range(7): print(i)", ["2", "3", "4", "5", "6"]),
]


def run() -> int:
    t0 = time.time()
    gb = GBPython()
    print(f"boot ok ({time.time() - t0:.1f}s)")

    failures = 0
    for label, source, expected in CASES:
        t0 = time.time()
        got = gb.repl(source)
        got_trim = [line for line in got if line]
        ok = got_trim == expected
        status = "ok" if ok else "FAIL"
        print(f"  {status:4} {label} ({time.time() - t0:.1f}s)")
        if not ok:
            failures += 1
            print(f"       source:   {source!r}")
            print(f"       expected: {expected}")
            print(f"       got:      {got_trim}")

    examples = sorted(EXAMPLES.glob("*.py"))
    for path in examples:
        source, expected = load_example(path)
        t0 = time.time()
        got = [line for line in gb.repl(source) if line]
        ok = got == expected
        print(f"  {'ok' if ok else 'FAIL':4} example {path.name} ({time.time() - t0:.1f}s)")
        if not ok:
            failures += 1
            print(f"       expected: {expected}")
            print(f"       got:      {got}")

    # End-to-end tests through the real on-screen keyboard.
    osk_cases = [
        ("OSK typing 2+3*4", "2+3*4", ["> 14"]),
        # shift chord (select+A) for uppercase, tap-select for space
        ("OSK shift+space typing", "B=7;B and True", ["> True"]),
    ]
    for label, text, expected in osk_cases:
        t0 = time.time()
        gb.type_source(text)
        got = gb.run_typed()
        got_trim = [line for line in got if line]
        ok = got_trim == expected
        print(f"  {'ok' if ok else 'FAIL':4} {label} ({time.time() - t0:.1f}s)")
        if not ok:
            failures += 1
            print(f"       got: {got_trim}")

    # input(): the program blocks mid-run, we type on the OSK, Start submits.
    t0 = time.time()
    gb.inject("n=input('n?')\nprint(int(n)*2)")
    gb.press("start", hold=3, release=3)
    gb.frames(30)
    gb.move_cursor("4")
    gb.press("a_button", hold=3, release=3)
    gb.move_cursor("2")
    gb.press("a_button", hold=3, release=3)
    gb.press("start", hold=3, release=3)  # submit
    gb.wait_run_done()
    gb.frames(5)
    got = [line.strip() for line in gb.screen()[7:12] if line.strip()]
    ok = got[-1] == "84"
    print(f"  {'ok' if ok else 'FAIL':4} input() interactive ({time.time() - t0:.1f}s)")
    if not ok:
        failures += 1
        print(f"       got: {got}")

    # Cursor wraps horizontally at row ends.
    t0 = time.time()
    gb.move_cursor("1")  # (0, 0)
    gb.press("left", hold=3, release=3)
    wrap_right = gb._cursor() == (0, OSK_COLS - 1)
    gb.press("right", hold=3, release=3)
    wrap_left = gb._cursor() == (0, 0)
    ok = wrap_right and wrap_left
    print(f"  {'ok' if ok else 'FAIL':4} OSK row wraparound ({time.time() - t0:.1f}s)")
    if not ok:
        failures += 1
        print(f"       cursor: {gb._cursor()}, wrap_right={wrap_right}, wrap_left={wrap_left}")

    # Highlighted OSK keys must show inverted font glyphs, not the splash
    # logo (which parks its tiles in slots 128-143, over the inverted
    # copies of ASCII 0x20-0x2F unless the ROM regenerates them).
    t0 = time.time()
    bad = []
    for ch in "!\"%'()*+,-./;:<>?_=[]{}":
        r, c = gb.move_cursor(ch)
        tile = gb.tile_at(OSK_X + c, OSK_Y + r)
        want = (ord(ch) - 0x20) + 128
        if tile != want or gb.tile_data(tile) != bytes(b ^ 0xFF for b in gb.tile_data(tile - 128)):
            bad.append((ch, tile, want))
    ok = not bad
    print(f"  {'ok' if ok else 'FAIL':4} OSK highlight glyphs ({time.time() - t0:.1f}s)")
    if not ok:
        failures += 1
        print(f"       bad tiles: {bad}")

    total = len(CASES) + len(examples) + len(osk_cases) + 3
    print(f"\n{total - failures}/{total} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(run())
