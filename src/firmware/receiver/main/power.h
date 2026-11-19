#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     present;      /* AXP2101 有没有应答 */
    bool     bat_ok;       /* 读到的电压/电量是否可信 */
    bool     charging;     /* 正在充电 */
    bool     vbus;         /* USB 有电 */
    uint16_t mv;           /* 电池电压 mV（0 = 无效） */
    int8_t   percent;      /* 0..100，-1 = 未知 */
    uint8_t  raw_st0;      /* 原始 REG 0x00，调试用 */
    uint8_t  raw_st1;      /* 原始 REG 0x01，调试用 */
    uint8_t  raw_dir;      /* REG 0x01 bit[6:5]：0 待机 / 1 充电 / 2 放电 */
} power_info_t;

/* 探测 AXP2101 并把电池电压 ADC 打开。失败不致命，后续 get 会返回 present=false。 */
void power_init(void);

/* 读一次最新状态（I2C 事务，~1ms）。内部有 1s 缓存，可以随便调。 */
void power_get(power_info_t *out);

/* 把原始寄存器打到日志，用于核对寄存器映射是否正确（只在 init 时调一次）。 */
void power_dump_regs(void);
