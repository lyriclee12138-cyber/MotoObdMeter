#include "can_diag.h"

#include <stdarg.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/twai.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "version.h"

#define DIAG_TX_GPIO  4
#define DIAG_RX_GPIO  5

/* 测试用的 ID。0x6A5 不在 OBD 诊断范围（0x7DF 功能广播 / 0x7E0~0x7E7 物理），
 * 车上没有节点会认领它，发出去不会被当成诊断请求。 */
#define DIAG_TEST_ID  0x6A5

/* 用 printf 而不是 ESP_LOG：手机上 ESP_LOG 的 "I (12345) diag: " 前缀
 * 要占掉小半行，长句子会被折行折得没法读。这里自己控制格式。 */
static void P(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\r\n");
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void rx_gpio_input(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << DIAG_RX_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

/* ---------- 1. RX 线是否真的连着收发器 ----------
 *
 * "GPIO5 恒为高" 有两种截然相反的原因：
 *   a) 收发器正常输出隐性电平（线通着，只是总线上没数据）
 *   b) GPIO5 悬空，被内部上拉拉高（杜邦线松了 / 模块没供电）
 *
 * 用内部上下拉（约 45kΩ）区分：收发器的 RXD 是推挽输出，45k 拉不动它；
 * 悬空的引脚一拉就变。
 *
 * 返回 1=外部强驱动（线通）  0=悬空  -1=被强下拉（短路到地） */
static int t_rx_line(void)
{
    rx_gpio_input();

    gpio_set_pull_mode(DIAG_RX_GPIO, GPIO_PULLDOWN_ONLY);
    esp_rom_delay_us(3000);
    const int pd = gpio_get_level(DIAG_RX_GPIO);

    gpio_set_pull_mode(DIAG_RX_GPIO, GPIO_PULLUP_ONLY);
    esp_rom_delay_us(3000);
    const int pu = gpio_get_level(DIAG_RX_GPIO);

    gpio_set_pull_mode(DIAG_RX_GPIO, GPIO_FLOATING);

    if (pd == 1 && pu == 1) {
        return 1;
    }
    if (pd == 0 && pu == 1) {
        return 0;
    }
    if (pd == 0 && pu == 0) {
        return -1;
    }
    return 0;                      /* 弱驱动，按"没接好"处理 */
}

/* ---------- 2. 裸采 RX，看有没有边沿 ---------- */
static uint32_t t_rx_sample(int *high_pct, int ms)
{
    rx_gpio_input();

    uint32_t high = 0, n = 0, edges = 0;
    int last = gpio_get_level(DIAG_RX_GPIO);
    const uint32_t t0 = now_ms();

    while (now_ms() - t0 < (uint32_t)ms) {
        for (int i = 0; i < 2000; i++) {
            const int v = gpio_get_level(DIAG_RX_GPIO);
            if (v) {
                high++;
            }
            if (v != last) {
                edges++;
                last = v;
            }
            n++;
        }
    }

    *high_pct = n ? (int)((uint64_t)high * 100u / n) : 0;
    return edges;
}

/* ---------- 3. TX 通路：NO_ACK 自发，位错误说明发不出去 ----------
 *
 * NO_ACK 模式不要求对端回 ACK，所以"没人应答"不会算错。
 * 但如果 TX 线断着，控制器发出的显性位读不回来（收到的是隐性），
 * 会立刻产生位错误并计入错误计数器。
 * 所以位错误为 0 ≈ TX 线通着、收发器能把信号打上总线。 */
static int t_tx_path(void)
{
    twai_general_config_t g =
        TWAI_GENERAL_CONFIG_DEFAULT(DIAG_TX_GPIO, DIAG_RX_GPIO, TWAI_MODE_NO_ACK);
    g.tx_queue_len = 4;
    g.rx_queue_len = 4;
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g, &t, &f) != ESP_OK) {
        return -1;
    }
    if (twai_start() != ESP_OK) {
        twai_driver_uninstall();
        return -1;
    }

    twai_message_t m = {0};
    m.identifier       = DIAG_TEST_ID;
    m.data_length_code = 1;
    m.data[0]          = 0x55;

    for (int i = 0; i < 3; i++) {
        twai_transmit(&m, pdMS_TO_TICKS(20));
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    twai_status_info_t st;
    twai_get_status_info(&st);
    const int err = (int)st.tx_error_counter;

    twai_stop();
    twai_driver_uninstall();
    return err;
}

/* ---------- 4. ACK 测试：总线上到底有没有别的节点 ----------
 *
 * NORMAL 模式下，CAN 控制器必须收到别人回的一个 ACK 位才算发送成功。
 * 没有节点应答时控制器会自动重发，tx_error_counter 一路涨到 128。
 * 所以 tx_err==0 就说明：线确实搭在车上，而且有 ECU 醒着。
 *
 * 返回 0 表示"有应答"。 */
static int t_ack(uint32_t *tx_err_out)
{
    *tx_err_out = 0xFFFFFFFFu;

    twai_general_config_t g =
        TWAI_GENERAL_CONFIG_DEFAULT(DIAG_TX_GPIO, DIAG_RX_GPIO, TWAI_MODE_NORMAL);
    g.tx_queue_len = 4;
    g.rx_queue_len = 4;
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g, &t, &f) != ESP_OK) {
        return -1;
    }
    if (twai_start() != ESP_OK) {
        twai_driver_uninstall();
        return -1;
    }

    twai_message_t m = {0};
    m.identifier       = DIAG_TEST_ID;
    m.data_length_code = 1;
    m.data[0]          = 0x55;

    for (int i = 0; i < 5; i++) {
        twai_transmit(&m, pdMS_TO_TICKS(20));   /* 返回值只代表进了队列，不代表发成功 */
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    twai_status_info_t st;
    twai_get_status_info(&st);
    *tx_err_out = st.tx_error_counter;

    twai_stop();
    twai_driver_uninstall();
    return (*tx_err_out == 0) ? 0 : 1;
}

/* ---------- 5. 按某个速率监听 ---------- */
static int t_rate(const twai_timing_config_t *t, int ms,
                  uint32_t *rx_err, uint32_t ids[], int *nid)
{
    *rx_err = 0xFFFFFFFFu;

    twai_general_config_t g =
        TWAI_GENERAL_CONFIG_DEFAULT(DIAG_TX_GPIO, DIAG_RX_GPIO, TWAI_MODE_LISTEN_ONLY);
    g.tx_queue_len = 1;
    g.rx_queue_len = 64;
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g, t, &f) != ESP_OK) {
        return -1;
    }
    if (twai_start() != ESP_OK) {
        twai_driver_uninstall();
        return -1;
    }

    int cnt = 0;
    const uint32_t t0 = now_ms();

    while (now_ms() - t0 < (uint32_t)ms) {
        twai_message_t m;
        if (twai_receive(&m, pdMS_TO_TICKS(5)) == ESP_OK) {
            if (nid != NULL && *nid < 3) {
                int dup = 0;
                for (int i = 0; i < *nid; i++) {
                    if (ids[i] == m.identifier) {
                        dup = 1;
                    }
                }
                if (!dup) {
                    ids[(*nid)++] = m.identifier;
                }
            }
            cnt++;
        }
    }

    twai_status_info_t st;
    twai_get_status_info(&st);
    *rx_err = st.rx_error_counter;

    twai_stop();
    twai_driver_uninstall();
    return cnt;
}

/* ---------- 主流程：一轮 ---------- */
void can_diag_run(void)
{
    static int round;

    round++;

    if (round == 1) {
        P("");
        P("=================================");
        P(" CAN 诊断 %s", version_string());
        P(" 插上 OBD，钥匙拧到 ON");
        P(" 发动机运转更好（ECU 才持续发帧）");
        P("=================================");
    }

    P("");
    P("-- #%d -------------------", round);

    /* 1. RX 线 */
    const int line = t_rx_line();
    const char *line_s = (line == 1) ? "通" : (line == 0) ? "悬空" : "短路";

    /* 2. 裸采 */
    int hp = 0;
    const uint32_t edges = t_rx_sample(&hp, 800);

    P("RX线 %s | 裸采 高%d%% 边沿%lu", line_s, hp, (unsigned long)edges);

    /* 3. TX 通路 */
    const int txerr = t_tx_path();
    if (txerr < 0) {
        P("TX路 测试失败（驱动装不了）");
    } else {
        P("TX路 位错误%d%s", txerr, (txerr == 0) ? "" : " ← 发不出去");
    }

    /* 4. ACK */
    uint32_t ack_err = 0;
    const int ack_ret = t_ack(&ack_err);
    const int has_peer = (ack_ret == 0);
    if (ack_ret < 0) {
        P("ACK 测试失败");
    } else {
        P("ACK %s  tx_err=%lu", has_peer ? "有应答" : "无应答",
          (unsigned long)ack_err);
    }

    /* 5. 速率。500k 是 ELM327 报出来的值，先测它；
     *    只有它也收不到才继续往下试，省时间。 */
    uint32_t ids[3] = {0};
    int nid = 0;
    uint32_t e500 = 0;
    const twai_timing_config_t t500 = TWAI_TIMING_CONFIG_500KBITS();
    const int n500 = t_rate(&t500, 1800, &e500, ids, &nid);
    P("500k %d帧 rx_err=%lu", n500, (unsigned long)e500);

    int n250 = -1, n125 = -1, n1m = -1;

    if (n500 == 0) {
        uint32_t e250 = 0;
        const twai_timing_config_t t250 = TWAI_TIMING_CONFIG_250KBITS();
        n250 = t_rate(&t250, 1000, &e250, ids, &nid);
        P("250k %d帧 rx_err=%lu", n250, (unsigned long)e250);

        if (n250 == 0) {
            uint32_t e125 = 0, e1m = 0;
            const twai_timing_config_t t125 = TWAI_TIMING_CONFIG_125KBITS();
            n125 = t_rate(&t125, 600, &e125, NULL, NULL);
            P("125k %d帧", n125);

            const twai_timing_config_t t1m = TWAI_TIMING_CONFIG_1MBITS();
            n1m = t_rate(&t1m, 600, &e1m, NULL, NULL);
            P("1M   %d帧", n1m);
        }
    }

    /* 6. 判定 */
    P("");
    if (line == 0) {
        P(">>> RX 线没接到收发器");
        P("    查 GPIO5 ← 模块 RX 这根线");
        P("    再查模块 3.3V / GND 有没有电");
    } else if (line == -1) {
        P(">>> RX 被短路到地");
        P("    线可能破皮碰到车架了");
    } else if (txerr > 0) {
        P(">>> TX 线没通：MCU 发的信号出不去");
        P("    查 GPIO4 → 模块 TX 这根线");
        P("    （不交叉：GPIO4 接模块 TX）");
    } else if (n500 > 0 || n250 > 0 || n125 > 0 || n1m > 0) {
        const int best = (n500 > 0) ? 500 : (n250 > 0) ? 250 : (n125 > 0) ? 125 : 1000;
        P(">>> 收到数据！物理层正常");
        if (nid > 0) {
            P("    首帧 ID: 0x%03lX 0x%03lX 0x%03lX",
              (unsigned long)ids[0], (unsigned long)ids[1], (unsigned long)ids[2]);
        }
        if (best != 500) {
            P("    但速率是 %dk，不是 500k", best);
        }
        if (n500 > 0 && e500 > 0) {
            P("    不过 rx_err=%lu 偏高，查接触", (unsigned long)e500);
        }
    } else if (has_peer) {
        P(">>> 线接对了（有 ECU 应答），");
        P("    但车上至今没发帧");
        P("    → 钥匙档位太低，ECU 还在休眠");
        P("    → 发动机打着再测一次");
    } else {
        P(">>> 总线上没有任何节点应答");
        P("    1. OBD 6/14 是不是压到线皮");
        P("       （剥太短，端子咬的是塑料）");
        P("    2. 插错孔？CANH=上排第6，");
        P("       CANL=下排第6（14号）");
        P("    3. CANH/CANL 对调再试");
        P("       （对调不烧东西，放心）");
    }

    P("");
    vTaskDelay(pdMS_TO_TICKS(2000));
}
