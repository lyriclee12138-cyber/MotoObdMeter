#include "status_led.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <string.h>

#if LED_USE_WS2812
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#else
#include "driver/ledc.h"
#endif

static const char *TAG = "led";

#define LED_TICK_MS    20
#define BREATH_MS      2400      /* 呼吸周期 */

/* 三种状态的颜色（RGB） */
#define C_NO_LINK_R    0
#define C_NO_LINK_G    40
#define C_NO_LINK_B    255        /* 蓝：没连上 */
#define C_SIM_R        255
#define C_SIM_G        90
#define C_SIM_B        0           /* 橙：模拟数据 */

static volatile led_state_t s_want;
static led_state_t s_cur;

/* 闪烁动画 */
static int      s_flash_left;
static uint32_t s_flash_start;

static bool     s_ok;             /* 驱动初始化成功 */

#if LED_USE_WS2812
static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
#else
#define LED_DUTY_MAX   1023
#endif

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* 设置颜色。scale = 0..255 的亮度系数，用来做呼吸 */
static void led_rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t scale)
{
    if (!s_ok) {
        return;
    }
    const uint8_t rr = (uint8_t)((uint32_t)r * scale / 255u);
    const uint8_t gg = (uint8_t)((uint32_t)g * scale / 255u);
    const uint8_t bb = (uint8_t)((uint32_t)b * scale / 255u);

#if LED_USE_WS2812
    /* WS2812 的字节顺序是 GRB，不是 RGB，发错了颜色会串（红蓝互换） */
    uint8_t buf[3] = { gg, rr, bb };
    rmt_transmit_config_t tc = { .loop_count = 0 };
    if (rmt_transmit(s_chan, s_enc, buf, sizeof(buf), &tc) == ESP_OK) {
        /* 24 bit × 1.25us ≈ 30us，等它发完，避免和下次调用排队打架 */
        rmt_tx_wait_all_done(s_chan, 50);
    }
#else
    /* 单色 LED 只能表达亮度，用三色里最大的那个当亮度 */
    uint16_t v = rr > gg ? rr : gg;
    if (bb > v) {
        v = bb;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0,
                  (uint32_t)v * LED_DUTY_MAX / 255u);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
#endif
}

static void led_off(void)
{
    led_rgb(0, 0, 0, 0);
}

/* 呼吸：整数三角波，**故意不用浮点**。
 * ESP32-S3 上任务里的浮点依赖 FPU 上下文保存，某些 FreeRTOS 配置下
 * 自定义任务做浮点运算会直接崩（panic 循环）。亮度曲线差一点点无所谓。 */
static void breath(void)
{
    const uint32_t phase = now_ms() % BREATH_MS;
    uint32_t k = (phase < (BREATH_MS / 2)) ? (phase * 2) : ((BREATH_MS - phase) * 2);
    /* k: 0..BREATH_MS → 亮度 8..255 */
    const uint8_t scale = (uint8_t)(8u + (247u * k) / BREATH_MS);
    led_rgb(C_NO_LINK_R, C_NO_LINK_G, C_NO_LINK_B, scale);
}

static void led_tick(void)
{
    /* 状态切换：进模拟模式三连闪，离开模拟模式闪一下 */
    if (s_want != s_cur) {
        const led_state_t old = s_cur;
        s_cur = s_want;
        if (s_want == LED_STATE_SIM) {
            s_flash_left = 3;
            s_flash_start = now_ms();
        } else if (old == LED_STATE_SIM) {
            s_flash_left = 1;
            s_flash_start = now_ms();
        }
    }

    if (s_flash_left > 0) {
        const uint32_t t = now_ms() - s_flash_start;
        const bool on = (t % 200) < 100;
        if (on) {
            if (s_cur == LED_STATE_SIM) {
                led_rgb(C_SIM_R, C_SIM_G, C_SIM_B, 255);
            } else {
                led_rgb(C_NO_LINK_R, C_NO_LINK_G, C_NO_LINK_B, 255);
            }
        } else {
            led_off();
        }
        if (t >= 200) {
            s_flash_start = now_ms();
            s_flash_left--;
        }
        return;
    }

    switch (s_cur) {
        case LED_STATE_NO_LINK:
            breath();
            break;
        case LED_STATE_LINKED:
            led_off();
            break;
        case LED_STATE_SIM:
            led_rgb(C_SIM_R, C_SIM_G, C_SIM_B, 255);
            break;
        default:
            break;
    }
}

static void led_task(void *arg)
{
    (void)arg;
    while (1) {
        led_tick();
        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
    }
}

void status_led_init(void)
{
#if !LED_ENABLE
    ESP_LOGW(TAG, "状态灯已禁用（LED_ENABLE=0）");
    return;
#endif
#if LED_USE_WS2812
    rmt_tx_channel_config_t ch = {
        .gpio_num          = LED_GPIO,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = 10000000,      /* 10MHz → 0.1us/tick */
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    esp_err_t e = rmt_new_tx_channel(&ch, &s_chan);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "RMT 通道创建失败: %s（GPIO%d）", esp_err_to_name(e), LED_GPIO);
        return;
    }
    ESP_LOGI(TAG, "[1/3] RMT 通道 OK");

    /* WS2812 单总线时序 @10MHz：
     *   0 码 = 高 0.4us + 低 0.9us
     *   1 码 = 高 0.8us + 低 0.5us
     * 高位先发（msb_first） */
    rmt_bytes_encoder_config_t be = {
        .bit0 = { .level0 = 1, .duration0 = 4, .level1 = 0, .duration1 = 9 },
        .bit1 = { .level0 = 1, .duration0 = 8, .level1 = 0, .duration1 = 5 },
        .flags = { .msb_first = 1 },
    };
    e = rmt_new_bytes_encoder(&be, &s_enc);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "RMT 编码器创建失败: %s", esp_err_to_name(e));
        return;
    }
    ESP_LOGI(TAG, "[2/3] 编码器 OK");
    e = rmt_enable(s_chan);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "RMT 使能失败: %s", esp_err_to_name(e));
        return;
    }
    ESP_LOGI(TAG, "[3/3] RMT 使能 OK");
#else
    ledc_timer_config_t tc = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&tc) != ESP_OK) {
        return;
    }
    ledc_channel_config_t cc = {
        .gpio_num   = LED_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    if (ledc_channel_config(&cc) != ESP_OK) {
        return;
    }
#endif

    s_ok = true;
    s_want = LED_STATE_NO_LINK;
    s_cur  = LED_STATE_NO_LINK;

    /* 上电自检闪两下：看得到闪，说明引脚和灯型都对 */
    s_flash_left  = 2;
    s_flash_start = now_ms();

    xTaskCreate(led_task, "status_led", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "状态灯就绪（GPIO%d，%s）", LED_GPIO,
             LED_USE_WS2812 ? "WS2812 RGB" : "单色 LED");
}

void status_led_set(led_state_t st)
{
#if LED_ENABLE
    s_want = st;
#else
    (void)st;
#endif
}
