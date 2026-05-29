#!/usr/bin/env python3
"""Thin wrapper -> the vendored idf-devtools submodule's provision.py.

Build a littlefs image from a board config dir and flash it via parttool.py.

Usage:
    ./provision.py <board-name-under-config/>
    ./provision.py <path/to/dir/containing/conf/>

Env:
    ESPPORT    - serial port (required)
    IDF_TARGET - if set, must match board.conf::mcu when present
"""
import os
import sys

_IMPL = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     'etc', 'idf-devtools', 'provision.py')
os.execv(sys.executable, [sys.executable, _IMPL, *sys.argv[1:]])
