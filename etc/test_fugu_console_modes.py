#!/usr/bin/env python3
"""Host unit test for fugu_console.resolve_command_mode (mode/stdin/REPL resolution).

Pure logic, no device — run with plain `python3 etc/test_fugu_console_modes.py`. Stubs the optional
`serial` dep so the module imports in a bare environment (the transport import is unconditional).
"""
import os
import sys
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.modules.setdefault("serial", types.ModuleType("serial"))  # pyserial may be absent here

from fugu_console import resolve_command_mode as r  # noqa: E402

# (label, args, expected) — args: explicit_cmds, use_stdin, want_test, want_coredump, stdin_is_tty
#                           expected: (read_stdin, active, delimit)
CASES = [
    ("single -c (tty)",        ([" mem "], False, False, False, True),  (False, True, False)),
    ("single -c (piped)",      (["mem"],   False, False, False, False), (False, True, False)),
    ("multi -c",               (["a", "b"], False, False, False, True), (False, True, True)),
    ("--stdin (tty)",          ([],        True,  False, False, True),  (True,  True, True)),
    ("auto-batch (piped)",     ([],        False, False, False, False), (True,  True, True)),
    ("REPL (tty, no mode)",    ([],        False, False, False, True),  (False, False, False)),
    ("--test wins over pipe",  ([],        False, True,  False, False), (False, False, False)),
    ("--coredump wins/pipe",   ([],        False, False, True,  False), (False, False, False)),
    ("-c + pipe stays single", (["x"],     False, False, False, False), (False, True, False)),
    ("--stdin + -c combine",   (["x"],     True,  False, False, True),  (True,  True, True)),
]


def main():
    fails = 0
    for label, args, expected in CASES:
        got = r(*args)
        ok = got == expected
        print(f"  [{'PASS' if ok else 'FAIL'}] {label}" + ("" if ok else f"  got {got} != {expected}"))
        fails += not ok
    print(f"\n{len(CASES) - fails}/{len(CASES)} passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
