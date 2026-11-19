#include "app_power.h"

#include "runtime.h"
#include "odo.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "power";

static uint32_t s_last_alive_ms;
static bool     s_hold;        /* 调试期禁止深睡 */
static bool     s_seen_bus;    /* 本次上电后是否见过总线 */

void app_power_hold_awake(bool hold)
{
    s_hold = hold;
}

void app_power_init(void)
{
    s_last_alive_ms = (uint32_t)(esp_timer_get_time() / 1000);

    /* 定时唤醒是这里的关键选择。
     * 另一种做法是把收发器 RXD 接 GPIO 做边沿唤醒，延迟更低，但收发器
     * 待机时仍要供电，静态电流反而更大——对"别把电瓶耗干"这个目标来说
     * 是负优化。定时唤醒纯固件实现，更省电也更好调。 */
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGI(TAG, "定时唤醒，回到运行态探测总线");
    } else if (cause != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "唤醒原因: %d", (int)cause);
    }
}

void app_power_tick(bool bus_alive)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    if (s_hold) {
        s_last_alive_ms = now;
        return;
    }

    if (bus_alive) {
        s_last_alive_ms = now;
        s_seen_bus = true;
        return;
    }

    const uint32_t limit = s_seen_bus ? IDLE_SLEEP_MS : FIRST_BOOT_MS;
    if ((now - s_last_alive_ms) < limit) {
        return;
    }

    ESP_LOGI(TAG, "总线静默 %us，落盘后进入深度睡眠（%us 后唤醒）",
             (unsigned)(limit / 1000), (unsigned)WAKE_PERIOD_S);

    runtime_commit();
    odo_commit();

    esp_sleep_enable_timer_wakeup((uint64_t)WAKE_PERIOD_S * 1000000ULL);
    esp_deep_sleep_start();
}
