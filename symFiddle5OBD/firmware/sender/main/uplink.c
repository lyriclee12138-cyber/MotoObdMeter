#include "uplink.h"

#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_now.h"

static const char *TAG = "uplink";

static uint32_t s_seq;
static bool s_ready;

static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

esp_err_t uplink_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_init: %s", esp_err_to_name(err));
        return err;
    }

    /* 不用连热点，STA 模式只是为了拿到一个可用的 MAC 和射频通道 */
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_start: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_channel(UPLINK_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_channel: %s", esp_err_to_name(err));
        return err;
    }

    /* 关掉省电，否则射频会在 beacon 间隔休眠，ESP-NOW 延迟抖动很大 */
    esp_wifi_set_ps(WIFI_PS_NONE);

    err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init: %s", esp_err_to_name(err));
        return err;
    }

    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, BCAST, 6);
    peer.channel = UPLINK_CHANNEL;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;

    err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "add_peer: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "ESP-NOW ready, ch=%d, bcast", UPLINK_CHANNEL);
    return ESP_OK;
}

esp_err_t uplink_send(telemetry_t *t)
{
    if (!s_ready || t == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    t->magic   = TELEMETRY_MAGIC;
    t->version = TELEMETRY_VERSION;
    t->seq     = ++s_seq;

    return esp_now_send(BCAST, (const uint8_t *)t, sizeof(*t));
}
