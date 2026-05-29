"""idf.py extension: archive the build ELF after `flash`/`app-flash`.

Wraps the stock flash callbacks so a serial flash records build/fugu-firmware.elf
in elf-archive/ (see etc/elf_archive.py), letting a later coredump be
symbolicated against the exact image. The device name comes from $FUGU_DEVICE
(./flash.sh sets it); absent that, the serial port basename is used.

Everything here is best-effort and guarded — a failure must never block a build
or flash.
"""
import os
import subprocess
import sys

_REPO = os.path.dirname(os.path.abspath(__file__))
_ARCHIVER = os.path.join(_REPO, 'etc', 'elf_archive.py')


def _archive(global_args):
    try:
        dev = os.environ.get('FUGU_DEVICE')
        if not dev:
            port = getattr(global_args, 'port', None)
            dev = os.path.basename(port) if port else 'serial'
        build_dir = getattr(global_args, 'build_dir', None) or os.path.join(_REPO, 'build')
        elf = os.path.join(build_dir, 'fugu-firmware.elf')
        bin = os.path.join(build_dir, 'fugu-firmware.bin')
        if not os.path.exists(elf):
            return  # not our firmware (e.g. a test build dir) — nothing to archive
        subprocess.run([sys.executable, _ARCHIVER, 'archive', dev,
                        '--method', 'serial', '--elf', elf, '--bin', bin], check=False)
    except Exception as e:
        print(f'elf-archive: skipped ({e})')


def action_extensions(base_actions, project_path):
    overrides = {}
    for name in ('flash', 'app-flash'):
        base = base_actions.get('actions', {}).get(name)
        if not base or 'callback' not in base:
            continue
        orig = base['callback']

        def make(orig_cb):
            def wrapped(action_name, ctx, global_args, **action_args):
                orig_cb(action_name, ctx, global_args, **action_args)
                _archive(global_args)
            return wrapped

        overrides[name] = dict(base, callback=make(orig))
    return {'actions': overrides}
