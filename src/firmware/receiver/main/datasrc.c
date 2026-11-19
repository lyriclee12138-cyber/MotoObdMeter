#include "datasrc.h"

#include "esp_log.h"
#include "fake_source.h"
#include "link.h"
#include "nvs.h"

static const char *TAG = "datasrc";

#define NVS_NS        "symui"
#define NVS_KEY_DEMO  "demo"

static bool s_demo;
static bool s_inited;

static uint32_t s_last_seq;
static bool     s_have_seq;

void datasrc_init(void)
{
    if (s_inited) {
        return;
    }

    /* 链路初始化失败不致命：界面照常跑，状态一直显示未连接 */
    if (link_init() == ESP_OK) {
        ESP_LOGI(TAG, "ESP-NOW 链路就绪");
    } else {
        ESP_LOGW(TAG, "ESP-NOW 链路初始化失败，只能看演示模式");
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, NVS_KEY_DEMO, &v) == ESP_OK) {
            s_demo = (v != 0);
        }
        nvs_close(h);
    }

    s_inited = true;
    ESP_LOGI(TAG, "演示模式 %s", s_demo ? "开" : "关");
}

bool datasrc_demo(void)
{
    return s_demo;
}

void datasrc_set_demo(bool on)
{
    if (s_demo == on) {
        return;
    }
    s_demo = on;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_DEMO, on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "演示模式 → %s", on ? "开" : "关");
}

void datasrc_read(uint32_t ms, uint16_t *rpm, int8_t *clt)
{
    *rpm = 0;
    *clt = INT8_MIN;

    if (s_demo) {
        fake_source_get(ms, rpm, clt);
        return;
    }

    link_state_t ls;
    if (!link_get(&ls)) {
        /* 断连：绝不能用上一包的旧值顶替，否则骑手看到的是"上次的转速" */
        return;
    }

    if (ls.t.flags & TF_RPM) {
        *rpm = ls.t.rpm;
    }
    if (ls.t.flags & TF_COOLANT) {
        *clt = ls.t.coolant;
    }
}

void datasrc_status(datasrc_status_t *out)
{
    out->demo = s_demo;

    link_state_t ls;
    const bool fresh = link_get(&ls);

    out->link_up   = fresh;
    out->ever      = ls.ever;
    out->rx_count  = ls.count;
    out->lost      = ls.lost;
    out->bad       = ls.bad;
    out->rssi      = ls.rssi;
    out->can       = CAN_SNIFF_UNKNOWN;

    if (s_demo) {
        /* 演示模式下不判断真实链路 */
        out->can = CAN_SNIFF_UNKNOWN;
        return;
    }

    if (fresh) {
        /* 收到包了：看有没有有效字段，就知道发送端那边的 CAN 通没通 */
        out->can = (ls.t.flags & TF_RPM) ? CAN_SNIFF_OK : CAN_SNIFF_EMPTY;
    } else if (ls.ever) {
        /* 曾经收到过，现在超时 —— 也是未知，但 UI 上会显示"断连" */
        out->can = CAN_SNIFF_UNKNOWN;
    }
}
