---
name: project_conffile_inmem_multipair_miscompile
description: "In-mem ConfFile{KVList} with a multi-pair list mixing a runtime const char* value throws std::length_error (xtensa-esp 14.2 codegen); use ConfFile::set() instead"
metadata: 
  node_type: memory
  type: project
  originSessionId: b8752a87-6546-4fb2-9a24-94914d84e1f8
---

`ConfFile(KVList)` (src/conf.h, the in-memory `{{ {"k","v"}, ... }}` ctor used in tests) crashes
with an uncaught `std::length_error` (basic_string range-ctor, `_M_create` size > max_size) when the
initializer_list has **two or more pairs AND at least one std::string value comes from a runtime
`const char*`** (a function param, or `std::to_string(...)`). Observed on xtensa-esp-elf 14.2 / IDF
5.5, -Os. Patterns that WORK: a single-pair list with a runtime value; any all-string-literal
multi-pair list (e.g. test_charger's mqtt confs). So it's a codegen landmine in the mixed
compile-time/runtime initializer_list backing array, not a logic bug.

Symptom: device aborts at boot → terminate() → boot loop; addr2line points at the `ConfFile{...}`
construction line. Bit me building test confs in test_buck.cpp (`{"pwm_driver", drv}`, `std::to_string(pin)`).

**Fix:** added `ConfFile::set(k,v)` (public, in-mem set). Build the conf single-literal-pair then
`.set()` the runtime values. See [[project_runtime_selectable_pwm_driver]].
