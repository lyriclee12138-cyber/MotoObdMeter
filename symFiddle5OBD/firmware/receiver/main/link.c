#include "link.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "heartbeat.h"

static const char *TAG = "link";

/* 回调在 Wi-Fi 任务里跑，主循环在另一个任务里读，所以要保护。
 * 临界区里只做 23 字节拷贝和几个计数，spinlock 完全够用，不必上 mutex。 */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static telemetry_t s_last;
static uint32_t    s_rx_ms;
static uint32_t    s_count;
static uint32_t    s_lost;
static uint32_t    s_bad;
static int8_t      s_rssi;
static bool        s_ever;

/* ---------- 心跳（回给发送端，让它知道我们在线） ----------
 * 发送端没法从 esp_now_send() 的返回值判断我们收没收到，只能我们主动回报。 */
static uint8_t  s_peer[6];          /* 发送端 MAC，从收到的包里学 */
static volatile bool s_peer_seen;
static bool     s_peer_added;       /* 是否已 add_peer */
static uint32_t s_hb_last_ms;
static uint16_t s_hb_seq;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (data == NULL || len != (int)sizeof(telemetry_t)) {
        s_bad++;
        return;
    }

    telemetry_t t;
    memcpy(&t, data, sizeof(t));

    if (t.magic != TELEMETRY_MAGIC || t.version != TELEMETRY_VERSION) {
        /* 同信道里可能有别的 ESP-NOW 设备；版本不一致也要拒收——
         * 字段布局变了还硬解，会把水温读成节气门。 */
        s_bad++;
        return;
    }

    portENTER_CRITICAL(&s_mux);
    if (s_ever) {
        const uint32_t gap = (uint32_t)(t.seq - s_last.seq);
        /* gap == 0 是重包，gap 巨大说明发送端重启过（seq 从 1 重来），
         * 都不该算丢包 */
        if (gap > 1u && gap < 1000u) {
            s_lost += gap - 1u;
        }
    }
    s_last  = t;
    s_rx_ms = now_ms();
    s_count++;
    s_ever  = true;
    portEXIT_CRITICAL(&s_mux);

    if (info != NULL && info->rx_ctrl != NULL) {
        s_rssi = (int8_t)info->rx_ctrl->rssi;
    }

    /* 顺手学会发送端的地址，心跳要发回给它 */
    if (info != NULL && !s_peer_seen) {
        memcpy(s_peer, info->src_addr, 6);
        s_peer_seen = true;
    }
}

esp_err_t link_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "netif_init: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event_loop: %s", esp_err_to_name(err));
        return err;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_init: %s", esp_err_to_name(err));
        return err;
    }

    /* 不连热点，STA 只是为了拿到 MAC 和一个可控的信道。
     * 信道必须和发送端写死的一致：两端不同信道时的表现是"完全收不到"，
     * 而不是"偶尔丢包"，很容易被误判成硬件故障。 */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    err = esp_wifi_set_channel(LINK_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_channel: %s", esp_err_to_name(err));
        return err;
    }

    /* 关省电：射频一休眠，ESP-NOW 的收包延迟会飙到几百毫秒，
     * 表现为仪表一顿一顿的 */
    esp_wifi_set_ps(WIFI_PS_NONE);

    err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_now_register_recv_cb(recv_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register_recv_cb: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "ESP-NOW 就绪，等待发送端（信道 %d，帧长 %u 字节）",
             LINK_CHANNEL, (unsigned)sizeof(telemetry_t));
    return ESP_OK;
}

bool link_get(link_state_t *out)
{
    if (out == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_mux);
    out->t     = s_last;
    out->rx_ms = s_rx_ms;
    out->count = s_count;
    out->lost  = s_lost;
    out->bad   = s_bad;
    out->ever  = s_ever;
    portEXIT_CRITICAL(&s_mux);

    out->rssi = s_rssi;

    return out->ever && ((now_ms() - out->rx_ms) < LINK_STALE_MS);
}

/* 主循环低频调用：每 500ms 给发送端回一个心跳。
 *
 * 发送端靠这个判断"圆屏还在不在"——ESP-NOW 单向广播，光看 esp_now_send()
 * 的返回值是看不出对端死活的。
 */
void link_service(void)
{
    if (!s_peer_seen) {
        return;                       /* 还没收到过包，不知道发给谁 */
    }

    const uint32_t now = now_ms();
    if (s_hb_last_ms != 0 && (now - s_hb_last_ms) < HB_PERIOD_MS) {
        return;
    }
    s_hb_last_ms = now;

    /* 第一次见到发送端时才能 add_peer。放在这里而不是回调里：
     * recv_cb 跑在 Wi-Fi 任务上下文，不适合调 esp_now_add_peer。*/
    if (!s_peer_added) {
        esp_now_peer_info_t pi = {0};
        memcpy(pi.peer_addr, s_peer, 6);
        pi.channel = LINK_CHANNEL;
        pi.encrypt = false;
        pi.ifidx   = WIFI_IF_STA;
        if (esp_now_add_peer(&pi) != ESP_OK) {
            return;
        }
        s_peer_added = true;
        ESP_LOGI(TAG, "记住发送端 %02X:%02X:%02X:%02X:%02X:%02X，开始回心跳",
                 s_peer[0], s_peer[1], s_peer[2], s_peer[3], s_peer[4], s_peer[5]);
    }

    heartbeat_t hb;
    hb.magic = HB_MAGIC;
    hb.kind  = HB_KIND_RX;
    hb.seq   = s_hb_seq++;
    esp_now_send(s_peer, (const uint8_t *)&hb, sizeof(hb));
}
