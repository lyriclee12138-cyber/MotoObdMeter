#include "perf.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "perf";

/* 每刷完一帧给一次，主循环靠它跟屏幕节奏 */
static SemaphoreHandle_t s_frame_sem;

static volatile uint64_t s_render_us;      /* RENDER_START -> RENDER_READY */
static volatile uint64_t s_flush_us;       /* FLUSH_START  -> FLUSH_FINISH */
static volatile uint64_t s_frame_us;       /* 相邻两次 RENDER_START 的间隔 */
static volatile uint32_t s_frames;         /* 实际绘制帧数 */
static volatile uint32_t s_lock_wait_us;   /* 主循环抢锁总耗时 */
static volatile uint32_t s_update_us;      /* 主循环锁内总耗时 */
static volatile uint32_t s_ticks;          /* 主循环轮数 */

static uint64_t now_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

static void disp_event_cb(lv_event_t *e)
{
    static uint64_t t_render = 0;
    static uint64_t t_flush = 0;
    static uint64_t t_prev_render = 0;

    lv_event_code_t code = lv_event_get_code(e);
    uint64_t t = now_us();

    switch (code) {
    case LV_EVENT_RENDER_START:
        if (t_prev_render) {
            s_frame_us += t - t_prev_render;
        }
        t_prev_render = t;
        t_render = t;
        s_frames++;
        break;
    case LV_EVENT_RENDER_READY:
        if (t_render) {
            s_render_us += t - t_render;
            t_render = 0;
        }
        break;
    case LV_EVENT_FLUSH_START:
        t_flush = t;
        break;
    case LV_EVENT_FLUSH_FINISH:
        if (t_flush) {
            s_flush_us += t - t_flush;
            t_flush = 0;
        }
        break;
    case LV_EVENT_REFR_READY:
        if (s_frame_sem) {
            xSemaphoreGive(s_frame_sem);
        }
        break;
    default:
        break;
    }
}

void perf_attach(lv_display_t *disp)
{
    if (s_frame_sem == NULL) {
        s_frame_sem = xSemaphoreCreateBinary();
    }
    lv_display_add_event_cb(disp, disp_event_cb, LV_EVENT_ALL, NULL);
    ESP_LOGI(TAG, "perf 已挂上显示事件");
}

bool perf_wait_frame(uint32_t timeout_ms)
{
    if (s_frame_sem == NULL) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return false;
    }
    return xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void perf_tick(uint32_t lock_wait_ms, uint32_t update_ms)
{
    s_lock_wait_us += lock_wait_ms * 1000u;
    s_update_us += update_ms * 1000u;
    s_ticks++;
}

void perf_report(void)
{
    static uint32_t last_report_ms = 0;
    uint32_t now_ms = (uint32_t)(now_us() / 1000);
    if (last_report_ms == 0) {
        last_report_ms = now_ms;
        return;
    }
    uint32_t dt = now_ms - last_report_ms;
    if (dt < 2000) {
        return;
    }

    uint32_t frames = s_frames;
    uint32_t ticks = s_ticks;
    if (frames == 0 || ticks == 0) {
        last_report_ms = now_ms;
        return;
    }

    uint32_t render_ms = (uint32_t)(s_render_us / frames / 1000u);
    uint32_t flush_ms = (uint32_t)(s_flush_us / frames / 1000u);
    uint32_t frame_ms = (uint32_t)(s_frame_us / (frames > 1 ? frames - 1 : 1) / 1000u);
    uint32_t lock_ms = s_lock_wait_us / ticks / 1000u;
    uint32_t upd_ms = s_update_us / ticks / 1000u;

    ESP_LOGI(TAG, "绘制 %2lu fps | 渲染 %3lums 刷屏 %3lums 帧间隔 %3lums | 主循环 抢锁 %3lums 锁内 %2lums | %lu 帧/%lu 轮",
             (unsigned long)(frames * 1000u / dt),
             (unsigned long)render_ms, (unsigned long)flush_ms, (unsigned long)frame_ms,
             (unsigned long)lock_ms, (unsigned long)upd_ms,
             (unsigned long)frames, (unsigned long)ticks);

    s_render_us = 0;
    s_flush_us = 0;
    s_frame_us = 0;
    s_frames = 0;
    s_lock_wait_us = 0;
    s_update_us = 0;
    s_ticks = 0;
    last_report_ms = now_ms;
}
