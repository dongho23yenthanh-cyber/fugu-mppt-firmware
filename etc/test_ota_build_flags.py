#!/usr/bin/env python3
"""Host unit tests for ota.py's build-flag guards (plant-sim / networking detection).

Pure logic, no device or network — run with plain `python3 etc/test_ota_build_flags.py`.
Guards the OTA pusher from flashing a bench VCONV (plant-sim) image to a real converter.
"""
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ota_build_flags import build_is_plant_sim, build_has_networking  # noqa: E402


def _bin_with_config(tmp, cfg):
    """Lay out <tmp>/build/{fugu-firmware.bin, config/sdkconfig.json=cfg}; return the bin path.
    cfg is a dict to write as sdkconfig.json, the string 'corrupt' for invalid JSON, or None to
    omit the file entirely (missing config)."""
    cfgdir = os.path.join(tmp, 'build', 'config')
    os.makedirs(cfgdir, exist_ok=True)
    binp = os.path.join(tmp, 'build', 'fugu-firmware.bin')
    open(binp, 'wb').close()
    if cfg == 'corrupt':
        with open(os.path.join(cfgdir, 'sdkconfig.json'), 'w') as f:
            f.write('{not valid json')
    elif cfg is not None:
        with open(os.path.join(cfgdir, 'sdkconfig.json'), 'w') as f:
            json.dump(cfg, f)
    return binp


# (label, cfg, want_sim, want_netw) — None means 'unknown', which callers must treat as not-safe.
CASES = [
    ("vconv bench build",  {"FUGU_WITH_VCONV": True,  "FUGU_WITH_NETW": True},  True,  True),
    ("production build",   {"FUGU_WITH_VCONV": False, "FUGU_WITH_NETW": True},  False, True),
    ("no-network build",   {"FUGU_WITH_VCONV": False, "FUGU_WITH_NETW": False}, False, False),
    ("vconv key absent",   {"FUGU_WITH_NETW": True},                            False, True),
    ("missing sdkconfig",  None,                                                None,  None),
    ("corrupt sdkconfig",  'corrupt',                                           None,  None),
]


def main():
    fails = 0
    for label, cfg, want_sim, want_netw in CASES:
        with tempfile.TemporaryDirectory() as tmp:
            binp = _bin_with_config(tmp, cfg)
            got_sim = build_is_plant_sim(binp)
            got_netw = build_has_networking(binp)
        ok = got_sim == want_sim and got_netw == want_netw
        fails += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}: sim={got_sim} netw={got_netw}"
              + ("" if ok else f"  (want sim={want_sim} netw={want_netw})"))
    print(f"\n{'ALL PASS' if not fails else f'{fails} FAILED'}")
    return fails


if __name__ == "__main__":
    sys.exit(1 if main() else 0)
