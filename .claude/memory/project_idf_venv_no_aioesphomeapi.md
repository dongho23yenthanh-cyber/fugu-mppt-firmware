---
name: idf-venv-no-aioesphomeapi
description: ESP-IDF tools venv must NOT contain aioesphomeapi — its cryptography>=48 pin breaks idf.py (ESP-IDF needs <45); console proxy deps go in ./.venv
metadata: 
  node_type: memory
  type: project
  originSessionId: 7db4b632-67c8-4dc9-b39d-5e956d328c96
---

The ESP-IDF 5.5 tools venv (`~/.espressif/python_env/idf5.5_py3.14_env`) requires `cryptography>=2.1.4,<45` (per `~/.espressif/espidf.constraints.v5.5.txt`). `aioesphomeapi` (the optional console dep for `EspHomeBleTransport` / `fugu_console.py --ble-proxy`, declared in `etc/fugu/requirements.txt`) requires `cryptography>=48`. They cannot share one venv.

Symptom: `. ./idf-export.sh` fails python-deps check — "Requirement 'cryptography<45,>=2.1.4' was not met. Installed version: 48.0.0".

Fix: keep them separate. ESP-IDF venv → `pip install 'cryptography>=2.1.4,<45' --only-binary cryptography` (resolves to 44.0.3 cp39-abi3 wheel, no source build even on Python 3.14) and `pip uninstall aioesphomeapi`. Run `fugu_console.py --ble-proxy` from the project `./.venv` (Python 3.14, has aioesphomeapi + cryptography 48).

**Why:** Python 3.14 is newer than ESP-IDF 5.5 officially supports; cryptography 48 is the first release with cp314 wheels, but 44.x ships forward-compatible abi3 wheels so the downgrade works. The conflict only appears because aioesphomeapi was pip-installed into the IDF venv by mistake.

**How to apply:** if `idf-export.sh` fails on cryptography, downgrade cryptography in the IDF venv and remove aioesphomeapi from it — don't install esphome/console deps there. See also [[reference_esphome_ble_proxy]], [[project_fugu_py_shared_console]].
