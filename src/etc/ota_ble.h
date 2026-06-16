#pragma once

#include <cstddef>
#include <cstdint>

#include "../util.h"

// OTA firmware update pushed over BLE (no WiFi). The host streams the image to a NUS write-no-response
// characteristic; bytes are staged in a PSRAM ring and flushed to the passive OTA partition from the
// network loop. Control + status ride the existing console: `otab` commands in, OTAB_* lines out
// (mirrored to the BLE client). Stubs link when WITH_BLE is off.

#ifdef WITH_BLE
bool otaBleBegin(uint32_t size, const char *sha256hex); // arm: esp_ota_begin(erase) + READY + first CRED
bool otaBleEnd();                                       // finalize: verify sha + set_boot; restarts on OK
void otaBleAbort();                                     // esp_ota_abort, free staging, re-enable ADC (net loop)
void otaBleRequestAbort();                               // ask the net-loop tick to abort (safe from any task)
bool otaBleActive();                                    // true between begin and end/abort
void otaBleStageBytes(const uint8_t *data, size_t len); // FW-char onWrite (host task): copy into ring only
void otaBleTick(time_ms nowMs);                   // network loop: drain ring -> esp_ota_write
#endif