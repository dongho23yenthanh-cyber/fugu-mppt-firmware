---
name: cv-min-dead-conf-key
description: "charger.conf `cv_min` is a no-op key; firmware reads `cv_float`. Setting cv_min won't change the float voltage."
metadata: 
  node_type: memory
  type: project
  originSessionId: 6509c06f-da91-4fcc-bb1a-5c0d8efaa58a
---

The charger float/hold voltage is read from conf key **`cv_float`** (charger.h: `cv_min = chargerConf.getFloat("cv_float", 3.325)`). The struct *member* is named `cv_min`, so `status` prints `cv_min=…`, which misleads you into editing a `cv_min` **key** — that key is **never read**.

Incident (2026-05-28): fry & flat "not charging" despite healthy BMS data. On-device had `cv_float=3.25` plus a hand-added `cv_min=3.38`. Firmware used cv_float=3.25 (below the LFP resting plateau ~3.32V), so the EOC-feedback loop pinned Vout to Vbat_fallback (~26V) below the pack (~26.6V) → no charge current; house loads discharged the pack at ibat≈−1.5A. Repo default is cv_float=3.37.

Fix: `set-config charger.conf cv_float 3.38` + `del-config charger.conf cv_min`, then `restart` (charger params load only at boot, main.cpp:215 — set-config alone does NOT reload). Both immediately bulk-charged (ibat +40A, ~0.5–0.6kW).

**Why:** key-name vs struct-member mismatch is a silent landmine; the status display reinforces the wrong key name.
**How to apply:** to change float voltage edit `cv_float`, not `cv_min`. When charging is stuck with cells above the configured float, suspect cv_float set below the LFP plateau. Related: [[project_charger_shared_bms_and_flat_vout_cal]], [[project_recharge_after_full_periodic_sweep]].
