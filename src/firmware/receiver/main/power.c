#include "power.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "power";

/* AXP2101（X-Powers PMU），固定 7 位地址 0x34。
 * 注意：微雪这块 BSP **没有** PMU 驱动，只有触摸在用 I2C，所以这里自己写最小实现。
 * 寄存器定义来自 AXP2101 datasheet / XPowersLib。 */
#define AXP2101_ADDR        0x34

#define REG_STATUS1         0x00    /* bit5 VBUS good, bit3 BATFET on */
#define REG_STATUS2         0x01    /* bit[6:5] 充电状态: 0=未充 1=预充 2=恒流 3=恒压 */
#define REG_ADC_CH_CTRL     0x30    /* bit0 = 电池电压 ADC 使能 */
#define REG_BAT_VOL_H       0x34    /* 14 bit，1 mV/LSB，高字节只用 bit[5:0] */
#define REG_BAT_VOL_L       0x35
#define REG_FUEL_GAUGE      0xA4    /* 电量百分比，bit[6:0] */

/* 单节锂电的电压→电量曲线（无库仑计时的兜底），(mV, %) 降序 */
static const struct { uint16_t mv; uint8_t pct; } k_curve[] = {
    {4200, 100}, {4100, 90}, {4000, 78}, {3900, 62},
    {3800, 45},  {3700, 30}, {3600, 15}, {3450, 5}, {3300, 0},
};

static i2c_master_dev_handle_t s_dev;
static bool s_ok;
static power_info_t s_cache;
static int64_t s_last_us;

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 100);
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_dev, b, 2, 100);
}

static int8_t pct_from_mv(uint16_t mv)
{
    if (mv >= k_curve[0].mv) {
        return 100;
    }
    for (size_t i = 0; i + 1 < sizeof(k_curve) / sizeof(k_curve[0]); i++) {
        if (mv >= k_curve[i + 1].mv) {
            uint16_t hi = k_curve[i].mv, lo = k_curve[i + 1].mv;
            int8_t phi = (int8_t)k_curve[i].pct, plo = (int8_t)k_curve[i + 1].pct;
            int32_t k = (int32_t)(mv - lo) * 1000 / (int32_t)(hi - lo);
            return (int8_t)(plo + (int32_t)(phi - plo) * k / 1000);
        }
    }
    return 0;
}

void power_dump_regs(void)
{
    if (!s_ok) {
        ESP_LOGW(TAG, "AXP2101 未就绪，跳过寄存器转储");
        return;
    }
    uint8_t v[8] = {0};
    if (rd(REG_STATUS1, v, 2) == ESP_OK && rd(REG_ADC_CH_CTRL, &v[2], 1) == ESP_OK &&
            rd(REG_BAT_VOL_H, &v[3], 2) == ESP_OK && rd(REG_FUEL_GAUGE, &v[5], 1) == ESP_OK) {
        ESP_LOGI(TAG, "原始寄存器 0x00=%02X 0x01=%02X 0x30=%02X 0x34=%02X 0x35=%02X 0xA4=%02X",
                 v[0], v[1], v[2], v[3], v[4], v[5]);
    } else {
        ESP_LOGW(TAG, "寄存器读取失败");
    }
}

void power_init(void)
{
    s_ok = false;
    s_dev = NULL;

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGW(TAG, "I2C 总线还没初始化，电量不可用");
        return;
    }

    /* 先探一下 0x34 在不在（有 ACK 才算） */
    if (i2c_master_probe(bus, AXP2101_ADDR, 200) != ESP_OK) {
        ESP_LOGW(TAG, "0x34 没有应答，板上没有 AXP2101，电量按「未知」处理");
        return;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "添加 I2C 设备失败");
        return;
    }

    /* 打开电池电压 ADC（0x30 bit0），否则 0x34 一直读 0 */
    uint8_t adc = 0;
    if (rd(REG_ADC_CH_CTRL, &adc, 1) == ESP_OK) {
        if ((adc & 0x01) == 0) {
            wr(REG_ADC_CH_CTRL, (uint8_t)(adc | 0x01));
            ESP_LOGI(TAG, "电池电压 ADC 之前是关的，已打开（0x30: %02X → %02X）", adc, adc | 0x01);
        }
    }

    s_ok = true;
    ESP_LOGI(TAG, "AXP2101 已就绪");
    power_dump_regs();
}

void power_get(power_info_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = (power_info_t){ .percent = -1 };

    if (!s_ok) {
        return;
    }

    int64_t now = esp_timer_get_time();
    if (now - s_last_us < 300000 && s_last_us != 0) {        /* 300ms 缓存 */
        *out = s_cache;
        return;
    }
    s_last_us = now;

    power_info_t r = { .present = true, .percent = -1 };

    uint8_t st[2] = {0};
    if (rd(REG_STATUS1, st, 2) != ESP_OK) {
        ESP_LOGW(TAG, "读状态失败");
        *out = r;
        return;
    }
    /* REG0x00 bit5 = VBUS good */
    r.vbus = (st[0] & 0x20) != 0;

    /* REG0x01 bit[6:5] 是「电池电流方向」，不是充电状态：
     *   00 = 待机（插着电、电池满、不充不放）
     *   01 = 充电
     *   10 = 放电   ← 拔掉电源时是这个
     *   11 = 保留
     * 老代码写成 != 0，把「放电」也算成充电，导致拔电后一直显示「充电中」。 */
    uint8_t dir = (uint8_t)((st[1] >> 5) & 0x03);
    r.charging = (dir == 0x01);
    r.raw_st0 = st[0];
    r.raw_st1 = st[1];
    r.raw_dir = dir;

    uint8_t vol[2] = {0};
    if (rd(REG_BAT_VOL_H, vol, 2) == ESP_OK) {
        uint16_t mv = (uint16_t)(((vol[0] & 0x3F) << 8) | vol[1]);
        if (mv >= 2500 && mv <= 4600) {
            r.mv = mv;
            r.bat_ok = true;
        }
    }

    /* 优先用库仑计，读不到或明显不合理就用电压曲线兜底 */
    uint8_t fg = 0;
    if (rd(REG_FUEL_GAUGE, &fg, 1) == ESP_OK) {
        uint8_t p = fg & 0x7F;
        if (p <= 100) {
            r.percent = (int8_t)p;
        }
    }
    if ((r.percent < 0 || r.percent > 100) && r.bat_ok) {
        r.percent = pct_from_mv(r.mv);
    }

    s_cache = r;
    *out = r;

    /* 每秒一次，用 DEBUG 级别（默认不输出）：查插拔电源状态时把日志级别调到 DEBUG 即可 */
    ESP_LOGD(TAG, "st0=%02X st1=%02X → vbus=%d chg=%d mv=%u pct=%d",
             st[0], st[1], (int)r.vbus, (int)r.charging, (unsigned)r.mv, (int)r.percent);
}
