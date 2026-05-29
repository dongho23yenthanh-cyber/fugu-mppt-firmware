#pragma once

// Pull-and-flash a firmware image over HTTP(S). Returns false on failure; on success the device
// reboots and never returns.
bool doOta(const char *url);
