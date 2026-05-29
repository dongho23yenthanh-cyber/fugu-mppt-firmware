#include "telnet_service.h"

#include <Arduino.h> // String, Serial
#include <WiFi.h>
#include <ESPmDNS.h>
#include <lwip/sockets.h>

#include "../logging.h"   // set_logging_telnet
#include "../etc/version.h"
#include "telemetry.h"    // getHostname, handleCommand

extern unsigned long lastTimeOutUs;

void TelnetService::closeConnection() {
    if (telnet.isConnected()) {
        telnet.flush();
        telnet.disconnectClient();
    }
}

void TelnetService::beginClose() {
    if (!telnet.isConnected()) return;
    telnet.flush();
    // Half-close: send our FIN now but keep the socket readable so closePending() can watch the peer
    // close in response. A full disconnectClient() here would shut the fd and abort that observation.
    shutdown(telnet.getClient().fd(), SHUT_WR);
}

bool TelnetService::onStart() {
    if (!WiFi.isConnected()) return false;
    setupTelnet();
    return true;
}

void TelnetService::onStop() {
    // Must drop the log sink before the server dies, or logging.cpp keeps writing to a stopped
    // ESPTelnet (telnet doubles as a log sink, see onConnect / set_logging_telnet).
    set_logging_telnet(nullptr);
    closeConnection();
    telnet.stop(true);
}

void TelnetService::onTick() { telnet.loop(); }

void TelnetService::setupTelnet() {
    telnet.onConnect(onConnect);
    telnet.onDisconnect(onDisconnect);
    telnet.onInputReceived([](String str) {
        // Captureless lambda (function-pointer callback) -> reach the server via the global service.
        auto &t = telnetService.telnet;
        if (str == "ping") {
            t.println("> pong");
            Serial.println("- Telnet: pong");
        } else if (str == "exit") {
            t.println("goodbye!");
            t.flush();
            t.disconnectClient();
        } else {
            // Telnet feeds whole lines straight to handleCommand (not via loopConsole), so emit the
            // OK/ERR confirmation here the way loopConsole does for UART/USB/BLE.
            bool ok = handleCommand(str);
            t.println(String(ok ? "OK: " : "ERR: ") + str);
        }
    });

    Serial.print("- Telnet: ");
    telnet.stop();
    if (telnet.begin(23)) {
        MDNS.addService("telnet", "tcp", 23);
        ESP_LOGI("telnet", "Telnet server running.");
    } else {
        ESP_LOGE("telnet", "Telnet server start error");
    }
}

void TelnetService::onConnect(String ip) {
    auto &t = telnetService.telnet;
    ESP_LOGI("telnet", "Client %s connected", ip.c_str());
    t.println("\nWelcome to " + String(getHostname().c_str()) + " (" +
              WiFi.localIP().toString().c_str() + ")");
    t.println(format_version());
    t.println("(Use ^] + q  to disconnect.)");

    set_logging_telnet(&t);
    lastTimeOutUs = 0; // trigger output
}

void TelnetService::onDisconnect(String ip) {
    set_logging_telnet(nullptr);
    ESP_LOGI("telnet", "Client %s disconnected", ip.c_str());
}
