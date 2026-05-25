#pragma once

// BleConsoleService: BLE (NimBLE NUS) serial console, the fallback transport when Wi-Fi is down.
// Compiled only with -DWITH_BLE; otherwise this header is empty. Disabled by default so the BLE
// radio only comes up on boards that opt in via ble.conf. No Wi-Fi precondition.

#ifdef WITH_BLE

#include "service.h"
#include "console_ble.h"
#include "util.h"
#ifdef WITH_NETW
#include "tele/telemetry.h"
#else
#include "storage/key-value.h"
extern KeyValueStorage nvs;
#endif

class BleConsoleService : public Service {
public:
    BleConsoleService() : Service("ble", "/littlefs/conf/ble.conf", /*requiresNetwork*/ false,
                                  /*enabledDefault*/ true) {
    }

    std::string statusDetail() const override { return bleConsoleConnected() ? "connected" : ""; }

protected:
    bool onStart() override {
        ConfFile c{"/littlefs/conf/ble.conf", /*no_warn_if_not_open*/ true};
#ifdef WITH_NETW
        std::string hn = getHostname();
#else
        nvs.open();
        std::string hn = nvs.readString("hostname", "fugu");
        nvs.close();
#endif
        bleConsoleBegin(        hn.starts_with("fugu-") ? hn : ("fugu-" + hn),
                        c.getString("ble_security", "justworks"),
                        (uint32_t) c.getLong("ble_passkey", 0));
        return true;
    }

    void onStop() override { bleConsoleEnd(); }
    void onTick() override { bleConsoleLoop(wallClockMs()); }
};

inline BleConsoleService bleConsoleService;

#endif // WITH_BLE
