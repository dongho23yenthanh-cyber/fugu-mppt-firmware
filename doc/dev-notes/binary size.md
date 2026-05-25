# Binary size

Notes on what's been done and what's still on the table to shrink the firmware image. Run `idf.py size`,
`idf.py size-components`, `idf.py size-files` to inspect.

## Done

- **Drop `<fstream>` from `src/conf.h`** — `std::ifstream` + `std::getline` pulled in `locale_init.o` and the entire
  classic-locale facet table for both `char` and `wchar_t` (`wlocale-inst.o`, `locale-inst.o`, `cxx11-*`). Replaced with
  `fopen`/`fgets`. Saved ~166 KB.
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

### Requires source audit first

3. **`CONFIG_LIBC_NEWLIB_NANO_FORMAT=y`** — `~46 KB`.
   Replaces full newlib printf (~45 KB across `vfprintf`/`svfprintf`/`vfiprintf`/`svfiprintf`) with nano variants
   (~7.6 KB). Tried once (commit `3c9fb60`, reverted in `d51a62c`): crashed in `wait_for_wifi()` because nano-printf
   silently mis-parses the `%hh` and `%ll` length modifiers and walks the va_args off the rails — `ESP_LOGI("tele",
   "... RSSI %hhi IP %s", WiFi.RSSI(), ip.c_str())` read the sign-extended RSSI as the `%s` pointer
   (`0xffffffd4`) → `LoadProhibited` in `memchr`. Before retrying, audit every format string in `src/` for `%hh*`,
   `%ll*`, `%j*`, `%z*`, `%t*` and replace with plain `%d`/`%u`/`%x` (everything gets varargs-promoted to `int`
   anyway). Known offenders: `telemetry.cpp`, `metering.h`, `util.cpp`, `i2c.h`, `ina226.h`, `ads.h`, `cooling.h`,
   `charger.h`, `viz/led.h`, `viz/lcd.cpp`, `adc/sampling.h`, `adc/mock.h`. Also drops `%f`/`%g` precision past
   ~9 sig figs.

### Medium-confidence

4. **Drop `<sstream>`/`<iomanip>` includes in arduino-esp32 BLE code** — only matters if BLE is actually compiled in.
   Check first.

5. **`CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=0`** — `~5–15 KB`.
   Currently full assertions (`__FILE__/__LINE__` strings in flash). Silent assertions strip them. Trade-off: crashes
   become opaque.

6. **Make sprofiler build-time optional** — `~10 KB`.
   `src/main.cpp:306` runtime-gates `sprofiler_initialize` on `pprof.conf::sprofiler_hz`, but the implementation (
   `libesp32-semihosting-profiler.a`, 10 KB) is always linked. Wrap behind `#ifdef SPROFILER_ENABLE` (CLAUDE.md notes
   it's only useful with OpenOCD attached).

### High-risk

7. **`CONFIG_COMPILER_OPTIMIZATION_SIZE=y`** (`-Os` instead of `-O2`) — `~100–150 KB`.
   Don't do this without measuring `rtcount` numbers before/after — `loopRT` is pinned to core 1, no `vTaskDelay`,
   ADC-sample-to-PWM latency is critical.

```
- 
VFS / FAT / SPIFFS / SD: Disable if only using LittleFS.
- 
Console / UART: If you only use your own console, disable CONFIG_ESP_CONSOLE_UART_DEFAULT.
```

file size:
- For more headroom you could switch to CONFIG_BT_CTRL_LPCLK_SEL_EXT_32K_XTAL (lets the 40 MHz crystal sleep too) — but only if the
  Fugu2 actually has a 32.768 kHz crystal populated. I left it on main-XTAL since that's safe regardless.




# matrix build

```
  ┌─────────┬──────────┬───────────┬─────────────┬───────────────────┐
  │ target  │ WITH_BLE │ WITH_NETW │    size     │   Δ vs baseline   │
  ├─────────┼──────────┼───────────┼─────────────┼───────────────────┤
  │ esp32s3 │ 1        │ 1         │ 1,804,032 B │ baseline          │
  ├─────────┼──────────┼───────────┼─────────────┼───────────────────┤
  │ esp32s3 │ 0        │ 1         │ 1,552,432 B │ −252 KB (BLE)     │
  ├─────────┼──────────┼───────────┼─────────────┼───────────────────┤
  │ esp32s3 │ 1        │ 0         │ 1,515,776 B │ −288 KB (NETW)    │
  ├─────────┼──────────┼───────────┼─────────────┼───────────────────┤
  │ esp32s3 │ 0        │ 0         │ 1,255,136 B │ −549 KB (both)    │
  ├─────────┼──────────┼───────────┼─────────────┼───────────────────┤
  │ esp32   │ 1        │ 1         │ 1,790,096 B │ 4% partition free │
  ├─────────┼──────────┼───────────┼─────────────┼───────────────────┤
  │ esp32   │ 0        │ 1         │ 1,534,720 B │ —                 │
  └─────────┴──────────┴───────────┴─────────────┴───────────────────┘
```


* The map shows esp_app_desc.c.obj actually contributes ~1 KB — the 185 KB is an esp_idf_size attribution artifact. Let me see what's really filling .rodata.

