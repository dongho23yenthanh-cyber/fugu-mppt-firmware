"""ELF symbol lookup for the `peek <symbol>` console command and `sym <pattern>` listing.

Both verbs are handled client-side in fugu_console.py: `peek <symbol>[+offset] [len]` resolves to
`peek 0x<addr> <len>` before it's sent to the device; `sym <pattern>` is purely local and never
goes on the wire. The ELF on disk must match the flashed image — there's no build-id check yet.
"""

import os
import re
import shutil
import subprocess


# `peek <symbol>[+offset]` — keep symbol alphabet narrow but include common C/C++ chars.
_PEEK_TARGET_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_$:.]*)(?:\+(0x[0-9a-fA-F]+|\d+))?$")

# Build dirs in this repo, newest-first by convention (default `build/` wins).
_BUILD_DIRS = (
    "build", "cmake-build-idf5.5-esp32s3", "build-esp32",
    "build-vconv", "build_ble", "build-nowifi-noble",
)


def find_elf(explicit: str | None = None) -> str | None:
    """Find the firmware ELF. `explicit` (or $FUGU_ELF) wins; else search common build dirs."""
    if explicit:
        return explicit if os.path.exists(explicit) else None
    env = os.environ.get("FUGU_ELF")
    if env and os.path.exists(env):
        return env
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    hits = []
    for sub in _BUILD_DIRS:
        p = os.path.join(root, sub, "fugu-firmware.elf")
        if os.path.exists(p):
            hits.append((os.path.getmtime(p), p))
    if not hits:
        return None
    hits.sort(reverse=True)  # newest mtime first
    return hits[0][1]


def _find_tool(tool: str) -> str | None:
    """Locate a binutils tool from the xtensa toolchain; fall back to llvm-/system equivalents."""
    for name in (f"xtensa-esp32s3-elf-{tool}", f"xtensa-esp-elf-{tool}", f"xtensa-esp32-elf-{tool}"):
        p = shutil.which(name)
        if p:
            return p
    base = os.path.expanduser("~/.espressif/tools/xtensa-esp-elf")
    if os.path.isdir(base):
        for ver in sorted(os.listdir(base), reverse=True):
            p = os.path.join(base, ver, f"xtensa-esp-elf/bin/xtensa-esp32s3-elf-{tool}")
            if os.path.exists(p):
                return p
    return shutil.which(f"llvm-{tool}") or shutil.which(tool)


def find_nm() -> str | None:
    return _find_tool("nm")


_SIMPLE_BASE_RE = re.compile(r"^[A-Za-z_][\w:$.]*")


def _add_demangled_aliases(syms: dict[str, tuple[int, int]]) -> None:
    """For every `_Z*` mangled name, run c++filt and add a simple-identifier alias when unambiguous.

    `_Z8setupCliv` demangles to `setupCli()` — alias `setupCli` lets the user type the natural
    function name. Methods `Ns::Cls::meth(args)` get an alias `Ns::Cls::meth`. Aliases that
    collide with an existing key (real global or another mangled symbol's base) are dropped so we
    never silently pick the wrong address.
    """
    mangled = [n for n in syms if n.startswith("_Z")]
    if not mangled:
        return
    cxxfilt = _find_tool("c++filt")
    if not cxxfilt:
        return
    try:
        out = subprocess.check_output(
            [cxxfilt], input="\n".join(mangled), text=True, stderr=subprocess.DEVNULL)
    except (subprocess.CalledProcessError, OSError):
        return
    demangled = out.splitlines()
    if len(demangled) != len(mangled):
        return
    # candidates[alias] = (rank, mangled-or-"-ambig"). rank 0 = pure `name(args)` or bare data
    # symbol; rank 1 = something nested (lambda inside a function, local static, etc). A lower-rank
    # candidate beats higher-rank ones, so the real `setupCli` wins over its inner lambdas.
    candidates: dict[str, tuple[int, str]] = {}
    for m, d in zip(mangled, demangled):
        if d == m:
            continue
        match = _SIMPLE_BASE_RE.match(d)
        if not match:
            continue
        alias = match.group(0)
        if alias == m or alias in syms:
            continue
        tail = d[len(alias):]
        pure = (tail == "") or (tail.startswith("(") and "::" not in tail)
        rank = 0 if pure else 1
        prev = candidates.get(alias)
        if prev is None or rank < prev[0]:
            candidates[alias] = (rank, m)
        elif rank == prev[0] and prev[1] != m:
            candidates[alias] = (rank, "")  # genuine collision at the same rank — drop
    for alias, (_, m) in candidates.items():
        if m and alias not in syms:
            syms[alias] = syms[m]


# (elf_path -> (mtime, {name: (addr, size)}))
_CACHE: dict[str, tuple[float, dict[str, tuple[int, int]]]] = {}


def load_symbols(elf_path: str) -> dict[str, tuple[int, int]]:
    """Parse `nm -S --defined-only <elf>` once per (path, mtime). Returns {name: (addr, size)}."""
    if not elf_path or not os.path.exists(elf_path):
        raise FileNotFoundError(f"ELF not found: {elf_path!r}")
    mtime = os.path.getmtime(elf_path)
    cached = _CACHE.get(elf_path)
    if cached and cached[0] == mtime:
        return cached[1]
    nm = find_nm()
    if not nm:
        raise FileNotFoundError(
            "no nm found; source idf-export.sh or install the xtensa toolchain")
    out = subprocess.check_output(
        [nm, "-S", "--defined-only", elf_path],
        text=True, stderr=subprocess.DEVNULL)
    syms: dict[str, tuple[int, int]] = {}
    for line in out.splitlines():
        parts = line.split(None, 3)
        if len(parts) == 4:
            addr_hex, size_hex, _t, name = parts
        elif len(parts) == 3:
            addr_hex, _t, name = parts
            size_hex = "0"
        else:
            continue
        try:
            addr = int(addr_hex, 16)
            size = int(size_hex, 16)
        except ValueError:
            continue
        # nm sometimes emits the same name twice (weak + strong); keep the one with size.
        prev = syms.get(name)
        if prev is None or (prev[1] == 0 and size > 0):
            syms[name] = (addr, size)
    _add_demangled_aliases(syms)
    _CACHE[elf_path] = (mtime, syms)
    return syms


def resolve(target: str, symbols: dict[str, tuple[int, int]]) -> tuple[int, int, str]:
    """`<symbol>[+offset]` -> (addr, size_hint_after_offset, base_name). Raises KeyError/ValueError."""
    m = _PEEK_TARGET_RE.match(target)
    if not m:
        raise ValueError(f"bad peek target: {target!r}")
    name, off = m.group(1), m.group(2)
    if name not in symbols:
        raise KeyError(f"symbol not found: {name!r}")
    addr, size = symbols[name]
    if off:
        off_n = int(off, 16) if off.startswith("0x") else int(off)
        addr += off_n
        size = max(0, size - off_n)
    return addr, size, name


def is_hex_address(token: str) -> bool:
    """True if `token` looks like an address literal already (skip ELF lookup)."""
    if token.startswith(("0x", "0X")):
        return all(c in "0123456789abcdefABCDEF" for c in token[2:]) and len(token) > 2
    # bare hex is ambiguous with bare-named symbols; require 0x prefix.
    return False


def preprocess_peek(cmd: str, elf_path: str | None) -> str:
    """If `cmd` is `peek <symbol>[+off] [len]`, rewrite to `peek 0x<addr> <len>`. Else passthrough."""
    parts = cmd.strip().split()
    if len(parts) < 2 or parts[0] != "peek":
        return cmd
    target = parts[1]
    if is_hex_address(target):
        return cmd
    if not elf_path:
        raise FileNotFoundError(
            "no firmware ELF found; build, set $FUGU_ELF, or pass --elf to resolve symbols")
    syms = load_symbols(elf_path)
    addr, size_hint, _ = resolve(target, syms)
    if len(parts) >= 3:
        len_arg = parts[2]
    elif size_hint > 0:
        len_arg = str(min(size_hint, 256))
    else:
        len_arg = "4"  # matches firmware default; typed u32 print is what users want most often
    return f"peek 0x{addr:08x} {len_arg}"


def format_sym_list(elf_path: str | None, pattern: str, limit: int = 40) -> str:
    """Render `sym <pattern>` output. `/regex/` runs as regex; else case-insensitive substring."""
    if not elf_path:
        return "sym: no firmware ELF found; build, set $FUGU_ELF, or pass --elf"
    try:
        syms = load_symbols(elf_path)
    except Exception as e:
        return f"sym: {e}"
    if not pattern:
        return f"sym: expected <pattern> (substring, or /regex/) — {len(syms)} symbols loaded"
    if len(pattern) >= 2 and pattern.startswith("/") and pattern.endswith("/"):
        try:
            rx = re.compile(pattern[1:-1])
        except re.error as e:
            return f"sym: bad regex: {e}"
        hits = [(n, a, s) for n, (a, s) in syms.items() if rx.search(n)]
    else:
        needle = pattern.lower()
        hits = [(n, a, s) for n, (a, s) in syms.items() if needle in n.lower()]
    # Largest-first puts data objects (which are what people usually peek) above tiny stubs.
    hits.sort(key=lambda t: (-t[2], t[0]))
    lines = [f"  0x{a:08x} {s:>7}  {n}" for n, a, s in hits[:limit]]
    if not hits:
        lines.append(f"  (no symbols matching {pattern!r})")
    elif len(hits) > limit:
        lines.append(f"  ... {len(hits) - limit} more (refine pattern)")
    return "\n".join(lines)
