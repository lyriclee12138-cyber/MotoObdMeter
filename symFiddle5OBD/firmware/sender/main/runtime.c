#include "runtime.h"

#include <stdbool.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "runtime";

#define NVS_NS      "symrun"
#define NVS_KEY_MIN "min"

/* 每累计 30 分钟落盘一次。
 * NVS 有磨损均衡，但没必要每分钟写一次——一年 50 万次写会把分区
 * 的擦写寿命吃得很难看。30 分钟一次约 1.7 万次/年，安全。
 * 掉电时最多损失 30 分钟，对保养提醒这种量级完全无所谓。 */
#define COMMIT_EVERY_MIN 30u

#define DT_CLAMP_MS 5000u

static uint32_t s_ms_frac;     /* 不足 1 分钟的毫秒余量 */
static uint32_t s_minutes;     /* 累计分钟 */
static uint32_t s_committed;
static bool     s_nvs_ok;

void runtime_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open 失败，运行时长不会持久化");
        return;
    }
    s_nvs_ok = true;

    uint32_t v = 0;
    if (nvs_get_u32(h, NVS_KEY_MIN, &v) == ESP_OK) {
        s_minutes = v;
        ESP_LOGI(TAG, "从 NVS 恢复运行时长 %lu min (%.1f h)",
                 (unsigned long)v, (double)(v / 60.0f));
    } else {
        ESP_LOGI(TAG, "无历史运行时长，从 0 开始");
    }
    s_committed = s_minutes;
    nvs_close(h);
}

void runtime_tick(bool bus_alive, uint32_t dt_ms)
{
    if (!bus_alive || dt_ms == 0) {
        return;
    }
    if (dt_ms > DT_CLAMP_MS) {
        dt_ms = DT_CLAMP_MS;
    }

    s_ms_frac += dt_ms;
    if (s_ms_frac >= 60000u) {
        uint32_t add = s_ms_frac / 60000u;
        s_minutes += add;
        s_ms_frac -= add * 60000u;

        if (s_minutes - s_committed >= COMMIT_EVERY_MIN) {
            runtime_commit();
        }
    }
}

uint16_t runtime_get_minutes(void)
{
    return (s_minutes > 65535u) ? 65535u : (uint16_t)s_minutes;
}

void runtime_commit(void)
{
    if (!s_nvs_ok) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_u32(h, NVS_KEY_MIN, s_minutes) == ESP_OK) {
        nvs_commit(h);
        s_committed = s_minutes;
    }
    nvs_close(h);
}
