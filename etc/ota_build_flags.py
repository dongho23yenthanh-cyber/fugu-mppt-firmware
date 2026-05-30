"""Inspect a build's sdkconfig.json for feature flags that decide whether an OTA image is safe to
push to a real converter. Pure stdlib so it stays host-unit-testable (see test_ota_build_flags.py)
without ota.py's network/argparse side effects."""
import json
import os


def _build_flag(bin_path, key):
    """Value of the sdkconfig key (CONFIG_<key> without the CONFIG_ prefix) from the sdkconfig.json
    next to bin_path's build dir, as bool; None if the config can't be read/parsed. None means
    'unknown' — callers must treat that as not-safe, never as the flag being off."""
    cfg = os.path.join(os.path.dirname(bin_path), 'config', 'sdkconfig.json')
    try:
        with open(cfg) as f:
            return bool(json.load(f).get(key))
    except (OSError, ValueError):
        return None


def build_has_networking(bin_path):
    """True/False if the build that produced bin_path has CONFIG_FUGU_WITH_NETW, None if unknown."""
    return _build_flag(bin_path, 'FUGU_WITH_NETW')


def build_is_plant_sim(bin_path):
    """True/False if the build that produced bin_path has CONFIG_FUGU_WITH_VCONV, None if unknown.
    With VCONV the real LEDC PwmDriver is swapped for a plant simulator (src/buck.h), so the
    half-bridge never switches: flashing it to a real converter yields 0W while the device still
    samples real sensors and looks alive."""
    return _build_flag(bin_path, 'FUGU_WITH_VCONV')
