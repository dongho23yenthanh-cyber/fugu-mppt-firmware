#!/usr/bin/env python3
"""conf-tool — discover Fugu MPPT chips on the LAN, view/diff/sync their /littlefs/conf files.

Example:
    conf-tool.py --hosts '.+' --local-conf config/fmetal
"""

import argparse
import ftplib
import os
import re
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

# Make `etc.fugu...` importable when invoked as a script from anywhere.
_REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
if _REPO not in sys.path:
    sys.path.insert(0, _REPO)

from etc.fugu.discover import discover_scope_servers


# --------------------------------------------------------------------------- #
# Round-trippable conf parser. Lines that are blank / pure comment / malformed
# are kept verbatim; key=value lines remember their key and value so we can
# rewrite just the value while preserving leading indent and trailing comment.
# --------------------------------------------------------------------------- #

@dataclass
class ConfLine:
    raw: str
    key: Optional[str] = None
    value: Optional[str] = None


@dataclass
class ConfFile:
    path: str
    lines: List[ConfLine] = field(default_factory=list)

    def values(self) -> Dict[str, str]:
        return {l.key: l.value for l in self.lines if l.key is not None}

    def set(self, key: str, new_value: str) -> None:
        for l in self.lines:
            if l.key == key:
                l.raw = _replace_value_in_line(l.raw, key, new_value)
                l.value = new_value
                return
        if self.lines and not self.lines[-1].raw.endswith('\n'):
            self.lines[-1].raw += '\n'
        self.lines.append(ConfLine(raw=f"{key}={new_value}\n", key=key, value=new_value))

    def write(self) -> None:
        with open(self.path, 'w') as f:
            for l in self.lines:
                f.write(l.raw)


def parse_conf(path: str) -> ConfFile:
    cf = ConfFile(path=path)
    with open(path, 'r') as f:
        for raw in f:
            stripped = raw.split('#', 1)[0].strip()
            if not stripped or '=' not in stripped:
                cf.lines.append(ConfLine(raw=raw))
                continue
            k, _, v = stripped.partition('=')
            cf.lines.append(ConfLine(raw=raw, key=k.strip(), value=v.strip()))
    return cf


def _replace_value_in_line(raw: str, key: str, new_value: str) -> str:
    pattern = re.compile(
        r'^(?P<prefix>\s*' + re.escape(key) + r'\s*=\s*)'
        r'(?P<value>[^#\r\n]*?)'
        r'(?P<rest>[ \t]*(?:#[^\r\n]*)?(?:\r?\n)?)$'
    )
    m = pattern.match(raw)
    if not m:
        return f"{key}={new_value}\n"
    return m.group('prefix') + new_value + m.group('rest')


# --------------------------------------------------------------------------- #
# FTP helpers — the conf dir is shallow, so a flat list+RETR is enough.
# --------------------------------------------------------------------------- #

def ftp_list_files(ftp: ftplib.FTP) -> List[str]:
    # Use NLST on the current dir. The embedded server ignores path arguments
    # to NLST, and MLSD on it returns LIST-style text that ftplib mis-parses.
    names = []
    for path in ftp.nlst():
        n = os.path.basename(path) or path
        if n and n not in ('.', '..'):
            names.append(n)
    return sorted(set(names))


def download_files(ftp: ftplib.FTP, remote_dir: str, local_dir: str,
                   ext: Optional[str] = None) -> List[str]:
    os.makedirs(local_dir, exist_ok=True)
    ftp.cwd('/' + remote_dir.lstrip('/'))
    paths = []
    for n in ftp_list_files(ftp):
        if ext and not n.endswith(ext):
            continue
        dest = os.path.join(local_dir, n)
        try:
            with open(dest, 'wb') as f:
                ftp.retrbinary(f'RETR {n}', f.write)
        except ftplib.error_perm as e:
            os.path.exists(dest) and os.remove(dest)
            print(f"  skip {n}: {e}")
            continue
        paths.append(dest)
        print(f"  down {n}")
    return paths


def upload_file(ftp: ftplib.FTP, local_path: str, remote_dir: str, remote_name: str) -> None:
    ftp.cwd('/' + remote_dir.lstrip('/'))
    with open(local_path, 'rb') as f:
        ftp.storbinary(f'STOR {remote_name}', f)
    print(f"  up   {remote_dir}/{remote_name}")


# --------------------------------------------------------------------------- #
# View / diff / merge UI
# --------------------------------------------------------------------------- #

def view_conf_files(conf_dir: str) -> None:
    for fn in sorted(os.listdir(conf_dir)):
        path = os.path.join(conf_dir, fn)
        if not os.path.isfile(path):
            continue
        print(f"\n# ===== {fn} =====")
        with open(path) as f:
            sys.stdout.write(f.read())
    sys.stdout.flush()


def diff_and_merge(device_cf: ConfFile, local_cf: ConfFile) -> bool:
    """Interactively merge differences from local into device_cf. Returns True if device_cf changed."""
    dvals = device_cf.values()
    lvals = local_cf.values()

    diffs: List[Tuple[str, Optional[str], str]] = []
    for k, lv in lvals.items():
        dv = dvals.get(k)
        if dv != lv:
            diffs.append((k, dv, lv))
    if not diffs:
        return False

    name = os.path.basename(device_cf.path)
    print(f"\n--- {name} -- {len(diffs)} diff(s) ---")
    kw = max(len(k) for k, _, _ in diffs)
    for i, (k, dv, lv) in enumerate(diffs, 1):
        marker = '+' if dv is None else '~'
        dv_s = '(absent)' if dv is None else repr(dv)
        print(f"  [{i:2}] {marker} {k:<{kw}}  device={dv_s:30}  local={lv!r}")

    print("pick keys to take from local: 'a'=all, ''=skip file, or indices/keys (comma/space-separated)")
    try:
        sel = input("> ").strip()
    except EOFError:
        return False
    if not sel:
        return False

    if sel == 'a':
        chosen = list(range(len(diffs)))
    else:
        chosen = []
        for tok in re.split(r'[,\s]+', sel):
            if not tok:
                continue
            try:
                chosen.append(int(tok) - 1)
            except ValueError:
                for i, (k, _, _) in enumerate(diffs):
                    if k == tok:
                        chosen.append(i)
                        break
                else:
                    print(f"  ?? unknown: {tok}")

    chosen = [i for i in chosen if 0 <= i < len(diffs)]
    if not chosen:
        return False
    for i in chosen:
        k, _, lv = diffs[i]
        device_cf.set(k, lv)
        print(f"  set {k} = {lv}")
    return True


# --------------------------------------------------------------------------- #
# Per-host workflow
# --------------------------------------------------------------------------- #

def handle_host(addr: str, name: str, args: argparse.Namespace) -> None:
    print(f"\n===== {name} ({addr}) =====")
    local_dl = os.path.join(args.dl_dir, f"{name}{addr}")
    conf_dl = os.path.join(local_dl, args.remote_conf)

    if os.path.isdir(conf_dl):
        for fn in os.listdir(conf_dl):
            p = os.path.join(conf_dl, fn)
            if os.path.isfile(p):
                os.remove(p)

    ftp = ftplib.FTP(addr, args.user, args.password, timeout=args.timeout)
    try:
        print(f"downloading {args.remote_conf}/ -> {conf_dl}")
        download_files(ftp, args.remote_conf, conf_dl, ext='.conf')

        if args.view or not args.local_conf:
            view_conf_files(conf_dl)
            if not args.local_conf:
                return

        local_conf = args.local_conf
        if os.path.isdir(os.path.join(local_conf, args.remote_conf)):
            local_conf = os.path.join(local_conf, args.remote_conf)
        if not os.path.isdir(local_conf):
            print(f"!! local conf dir does not exist: {local_conf}")
            return

        modified: List[Tuple[str, str]] = []  # (local_path, remote_filename)
        for fn in sorted(os.listdir(local_conf)):
            if not fn.endswith('.conf'):
                continue
            lp = os.path.join(local_conf, fn)
            dp = os.path.join(conf_dl, fn)
            if not os.path.exists(dp):
                print(f"\n--- {fn} -- not on device; queued for upload as new ---")
                modified.append((lp, fn))
                continue
            device_cf = parse_conf(dp)
            local_cf = parse_conf(lp)
            if diff_and_merge(device_cf, local_cf):
                device_cf.write()
                modified.append((dp, fn))

        if not modified:
            print("\nno changes to upload.")
            return

        print(f"\nready to upload {len(modified)} file(s):")
        for lp, fn in modified:
            print(f"  {lp} -> {args.remote_conf}/{fn}")

        if args.no_upload:
            print("--no-upload set; aborting.")
            return
        try:
            ok = input("confirm upload? [y/N] ").strip().lower()
        except EOFError:
            ok = ''
        if ok not in ('y', 'yes'):
            print("aborted.")
            return
        for lp, fn in modified:
            upload_file(ftp, lp, args.remote_conf, fn)
    finally:
        ftp.close()


# --------------------------------------------------------------------------- #
# main
# --------------------------------------------------------------------------- #

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--hosts', default='.+', help='regex matching host name or IP (default: .+)')
    ap.add_argument('--local-conf',
                    help='local config directory to diff against (e.g. config/fmetal). '
                         'If omitted, only views downloaded files.')
    ap.add_argument('--dl-dir', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dl'),
                    help='where to store downloaded configs (default: ./dl)')
    ap.add_argument('--remote-conf', default='conf', help='remote subdirectory (default: conf)')
    ap.add_argument('--user', default='user')
    ap.add_argument('--password', default='password')
    ap.add_argument('--timeout', type=float, default=4.0)
    ap.add_argument('--view', action='store_true', help='also print the downloaded files')
    ap.add_argument('--no-upload', action='store_true', help='diff/merge but never upload')
    args = ap.parse_args()

    try:
        host_re = re.compile(args.hosts)
    except re.error as e:
        sys.exit(f"--hosts: invalid regex: {e}")

    print('discovering hosts on LAN...')
    hosts = discover_scope_servers()
    if not hosts:
        sys.exit('no hosts discovered.')

    matched = [(a, p, n) for a, p, n in hosts if host_re.search(a) or host_re.search(n)]
    if not matched:
        sys.exit(f"no host matched /{args.hosts}/  (discovered: {[h[2] for h in hosts]})")

    print(f"matched {len(matched)} host(s): {', '.join(n for _, _, n in matched)}")
    for addr, _, name in matched:
        try:
            handle_host(addr, name, args)
        except KeyboardInterrupt:
            print("\ninterrupted.")
            return 130
        except Exception as e:
            print(f"!! {name} ({addr}): {type(e).__name__}: {e}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
