#pragma once
#include <stdbool.h>

/* 低功耗管理。
 * 供电取自 OBD 16 脚常电，所以深度睡眠是硬要求而不是优化项：
 * 不睡的话 7Ah 电瓶撑不过一周。 */

#define IDLE_SLEEP_MS      60000u   /* 总线静默这么久就睡 */
#define FIRST_BOOT_MS      600000u  /* 开机后一次总线都没见过 → 10 分钟才睡。
                                     * 台架调试时（充电宝供电、没接车）60 秒就睡
                                     * 会让"板子刚还好好的怎么没反应了"反复发生。
                                     * 见过总线的板子仍然按 60 秒，装车后不浪费电瓶。 */
#define WAKE_PERIOD_S      300u     /* 睡眠中每 5 分钟醒一次探总线 */

void app_power_init(void);

/* 调试期临时禁止深睡（模拟模式 / 台架联调）。true = 永不睡 */
void app_power_hold_awake(bool hold);

/* 主循环里每轮调用。bus_alive 为假且持续超时则进入深度睡眠。
 * 注意：本函数可能不返回（进入深睡后从头重启）。 */
void app_power_tick(bool bus_alive);
