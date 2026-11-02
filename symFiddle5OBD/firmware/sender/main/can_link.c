#include "can_link.h"

#include <stdio.h>
#include <string.h>
#include "driver/twai.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "can";

static volatile uint32_t s_last_rx_ms;
static bool s_started;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

esp_err_t can_link_init(void)
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_GPIO, CAN_RX_GPIO,
                                                          TWAI_MODE_NORMAL);
    g.rx_queue_len = 32;   /* 一次多帧响应最多几十帧，队列给够 */
    g.tx_queue_len = 8;

    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();

    /* 收全部帧，在软件里按 ID 过滤。
     * 理由：硬件过滤器配置错一次就是"完全收不到帧"，而这类 bug 极难
     * 从现象上分辨是接线问题还是滤波器问题。总线上帧率很低，软件过滤
     * 的 CPU 代价可以忽略。 */
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&g, &t, &f);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    err = twai_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "TWAI up @500k  TX=GPIO%d RX=GPIO%d", CAN_TX_GPIO, CAN_RX_GPIO);
    return ESP_OK;
}

esp_err_t can_link_send(uint32_t id, const uint8_t *data, uint8_t dlc)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (dlc > 8) {
        dlc = 8;
    }

    twai_message_t m;
    memset(&m, 0, sizeof(m));
    m.identifier = id;
    m.extd = 0;
    m.rtr = 0;
    m.data_length_code = dlc;
    if (data != NULL && dlc > 0) {
        memcpy(m.data, data, dlc);
    }

    return twai_transmit(&m, pdMS_TO_TICKS(20));
}

int can_link_recv(can_frame_t *out, uint32_t timeout_ms)
{
    if (!s_started || out == NULL) {
        return -1;
    }

    twai_message_t m;
    esp_err_t err = twai_receive(&m, pdMS_TO_TICKS(timeout_ms));

    if (err == ESP_ERR_TIMEOUT) {
        return 0;
    }
    if (err != ESP_OK) {
        return -1;
    }
    if (m.rtr) {
        return 0;   /* 远程帧不关心 */
    }

    out->id  = m.identifier;
    out->dlc = m.data_length_code;
    memcpy(out->data, m.data, 8);
    s_last_rx_ms = now_ms();
    return 1;
}

void can_link_service(void)
{
    if (!s_started) {
        return;
    }

    twai_status_info_t st;
    if (twai_get_status_info(&st) != ESP_OK) {
        return;
    }

    /* bus-off 是唯一会永久静默的故障：总线上错误计数超限后控制器
     * 主动脱离，不收也不发。不处理的话表现为"跑一会儿就没数据了"，
     * 而且断电重启才恢复。
     *
     * s_recovering 是必要的：bus-off 状态下每轮循环都会读到同一个状态，
     * 不加以区分的话会反复调 initiate_recovery，把日志刷满。 */
    static bool s_recovering;

    if (st.state == TWAI_STATE_BUS_OFF && !s_recovering) {
        ESP_LOGW(TAG, "bus-off detected, initiating recovery");
        s_recovering = true;
        twai_initiate_recovery();
        return;
    }

    if (st.state == TWAI_STATE_STOPPED) {
        ESP_LOGI(TAG, "controller stopped, restarting");
        if (twai_start() == ESP_OK) {
            s_recovering = false;
            ESP_LOGI(TAG, "restarted");
        }
    }
}

uint32_t can_link_last_rx_ms(void)
{
    return s_last_rx_ms;
}

int can_link_sniff(uint32_t ms)
{
    if (!s_started) {
        return -1;
    }

    enum { LOG_MAX = 24, ID_MAX = 16 };
    uint32_t ids[ID_MAX];
    int n_ids = 0;
    int total = 0;
    int logged = 0;

    uint32_t start = now_ms();
    while ((now_ms() - start) < ms) {
        can_frame_t f;
        int r = can_link_recv(&f, 20);
        if (r <= 0) {
            continue;
        }
        total++;

        if (logged < LOG_MAX) {
            ESP_LOGI(TAG, "  <- id=0x%03lX dlc=%u  %02X %02X %02X %02X %02X %02X %02X %02X",
                     (unsigned long)f.id, (unsigned)f.dlc,
                     f.data[0], f.data[1], f.data[2], f.data[3],
                     f.data[4], f.data[5], f.data[6], f.data[7]);
            logged++;
        }

        bool seen = false;
        for (int i = 0; i < n_ids; i++) {
            if (ids[i] == f.id) {
                seen = true;
                break;
            }
        }
        if (!seen && n_ids < ID_MAX) {
            ids[n_ids++] = f.id;
        }
    }

    ESP_LOGI(TAG, "嗅探 %lums：%d 帧 / %d 个不同 ID", (unsigned long)ms, total, n_ids);
    if (n_ids > 0) {
        char line[96];
        int n = 0;
        line[0] = '\0';
        for (int i = 0; i < n_ids && n < (int)sizeof(line) - 8; i++) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, "0x%03lX ", (unsigned long)ids[i]);
        }
        ESP_LOGI(TAG, "  ID 列表: %s", line);
    }
    return total;
}
