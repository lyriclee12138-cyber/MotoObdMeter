#include "status_led.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>

static const char *TAG = "led";

/* 10 位分辨率 → 占空比 0..1023 */
#define LED_DUTY_MAX   1023
#define LED_FREQ_HZ    5000
#define LED_TICK_MS    20

/* 呼吸周期：2.4 秒一亮一暗，比心跳慢，不刺眼 */
#define BREATH_MS      2400

static volatile led_state_t s_want;
static led_state_t s_cur;

/* 闪烁动画：s_flash_left 次，每 200ms 一个亮灭 */
static int      s_flash_left;
static uint32_t s_flash_start;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void led_duty(uint32_t duty)
{
    if (duty > LED_DUTY_MAX) {
        duty = LED_DUTY_MAX;
    }
#if LED_ACTIVE_LOW
    duty = LED_DUTY_MAX - duty;
#endif
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* 呼吸：用余弦做缓入缓出，比线性像"呼吸" */
static void breath(void)
{
    const uint32_t phase = now_ms() % BREATH_MS;
    const float x = (float)phase / (float)BREATH_MS;          /* 0..1 */
    const float k = (1.0f - cosf(x * 6.2831853f)) * 0.5f;     /* 0..1..0 */
    /* 不要完全熄灭，最低留 3%，否则像在闪 */
    led_duty((uint32_t)(LED_DUTY_MAX * (0.03f + 0.97f * k)));
}

static void led_tick(void)
{
    /* 状态切换：进模拟模式三连闪，其它切换闪一下 */
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

    /* 闪烁优先于常规状态 */
    if (s_flash_left > 0) {
        const uint32_t t = now_ms() - s_flash_start;
        led_duty((t % 200) < 100 ? LED_DUTY_MAX : 0);
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
            led_duty(0);
            break;
        case LED_STATE_SIM:
            led_duty(LED_DUTY_MAX);
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
    ledc_timer_config_t tc = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = LED_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&tc) != ESP_OK) {
        ESP_LOGW(TAG, "LEDC 定时器配置失败，状态灯不可用");
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
        ESP_LOGW(TAG, "LEDC 通道配置失败（GPIO%d 被占用？）", LED_GPIO);
        return;
    }

    s_want = LED_STATE_NO_LINK;
    s_cur  = LED_STATE_NO_LINK;

    /* 上电自检：闪两下。看得到闪，说明引脚和极性都对了；
     * 不闪就是 GPIO 不对，改 status_led.h 里的 LED_GPIO。 */
    s_flash_left  = 2;
    s_flash_start = now_ms();

    xTaskCreate(led_task, "status_led", 2560, NULL, 3, NULL);
    ESP_LOGI(TAG, "状态灯就绪（GPIO%d，%s）", LED_GPIO, LED_ACTIVE_LOW ? "低电平点亮" : "高电平点亮");
}

void status_led_set(led_state_t st)
{
    s_want = st;
}
