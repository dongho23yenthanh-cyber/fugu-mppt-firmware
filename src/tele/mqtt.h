#pragma once

#include <functional>
#include <string>

#include "../conf.h"
#include "../service.h"
#include "mqtt_client.h"

class MqttService : public Service {
public:
    typedef std::function<void(const char *, int len)> MqttMsgCallback;
    esp_mqtt_client_handle_t client{nullptr};

    MqttService() : Service("mqtt", "/littlefs/conf/mqtt.conf", /*requiresNetwork*/ true) {}

    // Wired by main.cpp before start(): does the charger BMS subscriptions + sets onConnected,
    // keeping mppt/home-assistant dependencies out of this translation unit. Called inside
    // onStart() with the loaded conf, so `service reload mqtt` re-subscribes.
    std::function<void(const ConfFile &)> preStart{nullptr};

private:
    std::unordered_map<std::string, MqttMsgCallback> mqttMsgHandlers{};
    bool mqttConnected = false;

public:
    bool isConnected()const  { return mqttConnected; }
    void (*onConnected)() = nullptr;

    void subscribeTopic(const std::string &topic, MqttMsgCallback fn);

    void init(const ConfFile &conf);
    void close();

    bool onStart() override;
    void onStop() override;

    void _handleEvent(esp_event_base_t base, int32_t event_id, void *event_data);

    // Test-only: deliver a synthetic message to the registered handler for `topic`,
    // bypassing the esp_mqtt event loop. Used by test/test_charger.cpp. Asserts the
    // topic has been subscribed.
    void _invokeForTest(const std::string &topic, const char *data, int len);
};

extern MqttService MQTT;



