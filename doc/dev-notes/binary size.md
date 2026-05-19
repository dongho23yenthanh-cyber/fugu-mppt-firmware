# Binary size

Notes on what's been done and what's still on the table to shrink the firmware image. Run `idf.py size`,
`idf.py size-components`, `idf.py size-files` to inspect.

## Done

- **Drop `<fstream>` from `src/conf.h`** — `std::ifstream` + `std::getline` pulled in `locale_init.o` and the entire
  classic-locale facet table for both `char` and `wchar_t` (`wlocale-inst.o`, `locale-inst.o`, `cxx11-*`). Replaced with
  `fopen`/`fgets`. Saved ~166 KB.
- **`CONFIG_LIBC_NEWLIB_NANO_FORMAT=y`** — replaced full newlib printf (~45 KB across `vfprintf`/`svfprintf`/
  `vfiprintf`/`svfiprintf`) with nano variants (~7.6 KB). Saved ~46 KB. Caveat: `%lld`/`%llu` not supported; `%f`/`%g`
  precision capped at ~9 sig figs.
- **Drop `<sstream>` / `std::stringbuf` everywhere** — required to link at all on xtensa-esp-elf 14.2 (
  `undefined reference to 'basic_stringbuf_nop'`). See CLAUDE.md.

## Candidates (not applied)

### High-confidence, safe for current config

1. **Disable mbedTLS CA cert bundle** — `~70 KB`.
   `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=n`. Currently `=y` with `DEFAULT_FULL` (200 certs). Nothing actually uses HTTPS:
   `src/etc/ota.cpp:176` sets `cert_pem = 0`, all `mqtt.conf` brokers are `mqtt://`, no `https://` anywhere in source.
   Also consider `CONFIG_MBEDTLS_TLS_SERVER=n` (client-only) and possibly `CONFIG_MBEDTLS_TLS_CLIENT=n` if you confirm
   no TLS use.

2. **Disable IPv6 in lwIP** — `~15–20 KB`.
   `CONFIG_LWIP_IPV6=n`. `nd6.c.obj` 9 KB + `ip6.c.obj` 3.7 KB + dual-stack code paths in `sockets.c`/`tcp_in.c`. No
   `AF_INET6` / `in6_` in source. Risk: only if LAN actually serves v6 records for `homeassistant.local` mDNS.

### Medium-confidence

3. **Drop `<sstream>`/`<iomanip>` includes in arduino-esp32 BLE code** — only matters if BLE is actually compiled in.
   Check first.

4. **`CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=0`** — `~5–15 KB`.
   Currently full assertions (`__FILE__/__LINE__` strings in flash). Silent assertions strip them. Trade-off: crashes
   become opaque.

5. **Make sprofiler build-time optional** — `~10 KB`.
   `src/main.cpp:306` runtime-gates `sprofiler_initialize` on `pprof.conf::sprofiler_hz`, but the implementation (
   `libesp32-semihosting-profiler.a`, 10 KB) is always linked. Wrap behind `#ifdef SPROFILER_ENABLE` (CLAUDE.md notes
   it's only useful with OpenOCD attached).

### High-risk

6. **`CONFIG_COMPILER_OPTIMIZATION_SIZE=y`** (`-Os` instead of `-O2`) — `~100–150 KB`.
   Don't do this without measuring `rtcount` numbers before/after — `loopRT` is pinned to core 1, no `vTaskDelay`,
   ADC-sample-to-PWM latency is critical.
