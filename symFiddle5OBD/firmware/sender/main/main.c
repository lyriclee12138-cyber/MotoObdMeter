#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "telemetry.h"
#include "can_link.h"
#include "obd_poll.h"
#include "status_led.h"
#include "uplink.h"
#include "runtime.h"
#include "odo.h"
#include "app_power.h"
#include "version.h"

static const char *TAG = "main";

#define UPLINK_PERIOD_MS 100u   /* 10 Hz 上行。ESP-NOW 上限远高于此，
                                 * 但 10Hz 对仪表已经足够，还能省电 */

/* 板载 BOOT 键（GPIO0 拉低）。注意：**复位时**按住它会进下载模式、
 * 程序根本不跑；所以这里检测的是"开机之后再按住"，用来临时切模拟数据。 */
#define SIM_BUTTON_GPIO  0
#define SIM_HOLD_MS      800    /* 按住这么久才算数，防误触 */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---------- 模拟数据源 ----------
 * 不接车、只用充电宝供电时，也能验证「ESP-NOW → 屏幕」这条链路。
 * 曲线和接收端 fake_source.c 一样：冷车怠速 → 起步 → 巡航 → 拉高转 → 回落，
 * 20 秒一轮。 */
static const struct {
    uint16_t t_ms;
    uint16_t rpm;
    int8_t   clt;
} k_sim[] = {
    {     0, 1450, 41 },
    {  3000, 1500, 44 },
    {  6500, 5200, 58 },
    { 11000, 5600, 78 },
    { 14000, 9000, 84 },
    { 16500, 9200, 86 },
    { 18500, 3200, 83 },
    { 20000, 1450, 80 },
};
#define SIM_N       (sizeof(k_sim) / sizeof(k_sim[0]))
#define SIM_LOOP_MS 20000u

static void sim_fill(telemetry_t *o, uint32_t ms)
{
    uint32_t t = ms % SIM_LOOP_MS;

    /* 找 t 落在哪一段。注意用 i+1 < SIM_N 而不是 i+2 < SIM_N：
     * 后者永远用不到最后一段，收油回落那段会被跳过（表现为转速卡住再跳变）。 */
    size_t i = 0;
    while (i + 1 < SIM_N && t >= k_sim[i + 1].t_ms) {
        i++;
    }

    uint16_t rpm;
    int8_t   clt;

    if (i + 1 >= SIM_N) {
        rpm = k_sim[SIM_N - 1].rpm;
        clt = k_sim[SIM_N - 1].clt;
    } else {
        uint32_t span = (uint32_t)(k_sim[i + 1].t_ms - k_sim[i].t_ms);
        uint32_t k    = (t > k_sim[i].t_ms) ? (t - k_sim[i].t_ms) : 0u;
        if (span == 0) {
            span = 1;
        }
        if (k > span) {
            k = span;
        }

        int32_t dr = (int32_t)k_sim[i + 1].rpm - (int32_t)k_sim[i].rpm;
        int32_t dc = (int32_t)k_sim[i + 1].clt - (int32_t)k_sim[i].clt;

        rpm = (uint16_t)((int32_t)k_sim[i].rpm + dr * (int32_t)k / (int32_t)span);
        clt = (int8_t)((int32_t)k_sim[i].clt + dc * (int32_t)k / (int32_t)span);
    }

    o->rpm     = rpm;
    o->coolant = clt;
    o->tps     = (uint8_t)(18 + rpm / 120);
    o->intake  = 32;
    o->volt_dv = 141;
    o->fuel    = FUEL_INVALID;
    o->flags   = TF_RPM | TF_COOLANT | TF_TPS | TF_IAT | TF_VOLT;
}

static bool     s_sim;
static uint32_t s_low_since;
static bool     s_edge_done;     /* 本次按住已经触发过切换，松手才复位 */

/* BOOT 键：按住满 SIM_HOLD_MS 就**切换**一次模拟模式，松手后保持。
 *
 * 早先写的是"按住才发"——手指得一直按着，测试时很别扭，松开就退回真实 OBD。
 * 现在改成按一下切换：按 1 秒进入、再按 1 秒退出。
 *
 * 注意 GPIO0 按住本身不会关机也不会复位（它只是个普通输入）；
 * 唯一的影响是**复位瞬间**按住会进下载模式、程序根本不跑。 */
static bool sim_tick(void)
{
    const uint32_t now = now_ms();
    const bool low = (gpio_get_level(SIM_BUTTON_GPIO) == 0);

    if (low) {
        if (s_low_since == 0) {
            s_low_since = (now == 0) ? 1u : now;
        } else if (!s_edge_done && (now - s_low_since) >= SIM_HOLD_MS) {
            s_edge_done = true;
            s_sim = !s_sim;
            app_power_hold_awake(s_sim);
            if (s_sim) {
                ESP_LOGW(TAG, "== 进入模拟数据模式（松手保持，再按住约 1 秒退出）==");
                ESP_LOGW(TAG, "   上行发假数据，仅用于验证 ESP-NOW / 屏幕");
            } else {
                ESP_LOGI(TAG, "== 退出模拟数据模式，恢复真实 OBD 数据 ==");
            }
        }
    } else {
        s_low_since = 0;
        s_edge_done = false;
    }

    return s_sim;
}

/* 临时诊断：1 = app_main 只打一行日志就原地待着，用来判断
 * "app_main 到底有没有被调用"。排查完改回 0。 */
#define SENDER_MINIMAL_BOOT 0

void app_main(void)
{
#if SENDER_MINIMAL_BOOT
    ESP_LOGI(TAG, ">>> 最小启动测试：app_main 已执行 <<<");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
    ESP_LOGI(TAG, "=== Fiddle5 OBD sender (CAN / ISO 15765-4) ===");
    ESP_LOGI(TAG, "固件版本 %s（编译于 %s）", version_string(), version_build_time());
    ESP_LOGI(TAG, "接线核对: 模块3.3V->3V3 GND->GND TX->GPIO%d RX->GPIO%d CANH->OBD6 CANL->OBD14",
             CAN_TX_GPIO, CAN_RX_GPIO);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* BOOT 键做输入（内部上拉，按下 = 低） */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << SIM_BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ONLY,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    ESP_ERROR_CHECK(can_link_init());

    /* 上电先被动听 3 秒总线：这一步不发任何 OBD 请求，只看物理层通不通。
     * 收不到就一定是接线 / 共地 / 极性 / 波特率的问题。 */
    const int sniffed = can_link_sniff(3000);
    if (sniffed <= 0) {
        ESP_LOGW(TAG, "总线上 3 秒内一帧都没收到");
        ESP_LOGW(TAG, "  只插了充电宝、没接车 -> 正常，可以继续");
        ESP_LOGW(TAG, "  接了车但收不到       -> 查 CANH(6)/CANL(14) 是否接反、GND 是否共地、点火是否打开");
    } else {
        ESP_LOGI(TAG, "总线物理层 OK：3 秒收到 %d 帧", sniffed);
    }

    runtime_init();
    odo_init();
    ESP_ERROR_CHECK(uplink_init());
    status_led_init();
    obd_poll_init();
    app_power_init();

    /* 真实数据。模拟模式下只借用它的里程/运行时长语义，
     * rpm/水温/节气门/电压全部由 sim_fill 覆盖。 */
    static telemetry_t s_real;
    memset(&s_real, 0, sizeof(s_real));
    s_real.fuel  = FUEL_INVALID;
    s_real.flags = TF_RUNTIME;   /* 运行计时总是有效，其余等首次读数再置位 */

    uint32_t last_send = now_ms();
    uint32_t last_tick = last_send;

    while (1) {
        can_link_service();

        const bool sim = sim_tick();

        /* 状态灯：模拟模式常亮（进入时三连闪），接收端在线则灭，否则呼吸。
         * "在线"靠接收端回的心跳判断——广播单向，发送成功不代表对方收到。 */
        if (sim) {
            status_led_set(LED_STATE_SIM);
        } else if (uplink_peer_alive()) {
            status_led_set(LED_STATE_LINKED);
        } else {
            status_led_set(LED_STATE_NO_LINK);
        }

        /* 这一步可能阻塞约 120ms（等 ECU 响应），所以 dt 不能按固定值算。
         * 模拟模式下不发请求，但 TWAI 仍在线（日志里能看总线是否活着）。 */
        if (!sim) {
            obd_poll_tick(&s_real);
        }

        const bool alive = obd_poll_bus_alive();

        uint32_t now = now_ms();
        uint32_t dt = now - last_tick;
        last_tick = now;

        runtime_tick(alive, dt);
        s_real.run_min = runtime_get_minutes();

        if (now - last_send >= UPLINK_PERIOD_MS) {
            last_send = now;

            telemetry_t out = s_real;
            if (sim) {
                sim_fill(&out, now);
                out.run_min = s_real.run_min;
            }

            esp_err_t e = uplink_send(&out);
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "esp_now_send: %s", esp_err_to_name(e));
            }

            /* 每 10 秒一行状态（10Hz × 10s = 100 包） */
            if ((out.seq % 100u) == 0u) {
                if (out.flags & TF_ODO) {
                    ESP_LOGI(TAG, "rpm=%u clt=%d tps=%u v=%.1f odo=%.1fkm run=%umin flags=0x%04X%s",
                             (unsigned)out.rpm, (int)out.coolant, (unsigned)out.tps,
                             (double)(out.volt_dv / 10.0f), (double)(out.odo_dam / 10.0f),
                             (unsigned)out.run_min, (unsigned)out.flags,
                             sim ? " [SIM]" : "");
                } else {
                    ESP_LOGI(TAG, "rpm=%u clt=%d tps=%u v=%.1f odo=-- run=%umin flags=0x%04X%s",
                             (unsigned)out.rpm, (int)out.coolant, (unsigned)out.tps,
                             (double)(out.volt_dv / 10.0f),
                             (unsigned)out.run_min, (unsigned)out.flags,
                             sim ? " [SIM]" : "");
                }
            }
        }

        app_power_tick(alive);

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
