"""idf.py extension: archive the build ELF after flash for coredump symbolication.

Thin shim — the implementation lives in the vendored idf-devtools submodule
(github.com/fl4p/idf-devtools). It wraps flash/app-flash to record build/<app>.elf
via elf_archive.py; device name from $FUGU_DEVICE (or $ELF_ARCHIVE_DEVICE), else
the serial-port basename. ./flash.sh sets $FUGU_DEVICE for you.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'etc', 'idf-devtools'))

from elf_archive_ext import action_extensions  # noqa: E402,F401
