/* bsync beacon node — Phase 0 injector + Phase 1 softAP timebase.
 *
 * Runs a hidden softAP (hw-stamped beacons every 100 TU) and, for the Phase 0
 * experiment, injects raw beacon-subtype frames with a sentinel timestamp via
 * esp_wifi_80211_tx every 20 ms. A sniffer decides: sentinel echoed verbatim ->
 * no hw stamping of injected frames; monotonic TSF -> Phase 2 unlocked.
 * See ../../plans/bsync-beacon-node.md.
 */
#include <string.h>
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define CHANNEL 13
#define LED_GPIO 21              // XIAO ESP32-S3 user LED, active low
#define INJECT_PERIOD_US 20000
#define SENTINEL 0xDEADBEEFCAFEBABEULL

static const char *TAG = "bsync-node";

static uint8_t mac[6];
static uint8_t frm[64];
static int frmLen;
static volatile uint32_t nTx, nErr;
static volatile esp_err_t lastErr;

static void build_frame(void) {
    uint8_t *p = frm;
    p[0] = 0x80; p[1] = 0x00;            // fc: beacon
    p[2] = 0; p[3] = 0;                  // duration
    memset(p + 4, 0xff, 6);              // DA broadcast
    memcpy(p + 10, mac, 6);              // SA
    memcpy(p + 16, mac, 6);              // BSSID
    p[22] = 0; p[23] = 0;                // seq (en_sys_seq)
    uint64_t ts = SENTINEL;
    memcpy(p + 24, &ts, 8);              // timestamp (the field under test)
    p[32] = 100; p[33] = 0;              // beacon interval [TU]
    p[34] = 0x11; p[35] = 0x04;          // capability: ESS|Privacy|ShortSlot
    p[36] = 0; p[37] = 0;                // SSID IE, hidden (len 0)
    static const uint8_t rates[] = {1, 4, 0x82, 0x84, 0x8b, 0x96};
    memcpy(p + 38, rates, sizeof rates);
    static const uint8_t ds[] = {3, 1, CHANNEL};
    memcpy(p + 44, ds, sizeof ds);
    frmLen = 47;
}

static void inject_cb(void *arg) {
    esp_err_t e = esp_wifi_80211_tx(WIFI_IF_AP, frm, frmLen, true);
    if (e == ESP_OK) nTx++; else { nErr++; lastErr = e; }
}

void app_main(void) {
    gpio_config_t io = {.pin_bit_mask = 1ULL << LED_GPIO, .mode = GPIO_MODE_OUTPUT,
                        .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&io);
    gpio_set_level(LED_GPIO, 0);  // solid on: app entered
    ESP_LOGI(TAG, "bsync-beacon boot");

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_country_code("DE", false));  // ch 12/13 legal
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t apc = {.ap = {
        .ssid = "bsync-p0",
        .ssid_len = 8,
        .password = "notasecret1",
        .channel = CHANNEL,
        .authmode = WIFI_AUTH_WPA2_PSK,
        .ssid_hidden = 1,
        .max_connection = 1,
        .beacon_interval = 100,
    }};
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(84));
    // the default AP_START handler starts DHCP async; wait for it, then stop for good
    for (int i = 0; i < 50; ++i) {
        esp_netif_dhcp_status_t st;
        if (esp_netif_dhcps_get_status(ap, &st) == ESP_OK && st == ESP_NETIF_DHCP_STARTED)
            break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    esp_netif_dhcps_stop(ap);   // beacons only, no services

    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, mac));
    build_frame();

    ESP_LOGI(TAG, "AP up: bssid=" MACSTR " channel=%d — softAP beacons 100 TU + "
             "injected sentinel beacons every %d ms", MAC2STR(mac), CHANNEL,
             INJECT_PERIOD_US / 1000);

    esp_timer_handle_t t;
    const esp_timer_create_args_t ta = {.callback = inject_cb, .name = "inject"};
    ESP_ERROR_CHECK(esp_timer_create(&ta, &t));
    ESP_ERROR_CHECK(esp_timer_start_periodic(t, INJECT_PERIOD_US));

    int led = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_set_level(LED_GPIO, led ^= 1);  // 0.5 Hz heartbeat: AP + injector running
        if (led)
            ESP_LOGI(TAG, "inject: tx=%lu err=%lu lastErr=0x%x",
                     (unsigned long)nTx, (unsigned long)nErr, (unsigned)lastErr);
    }
}
