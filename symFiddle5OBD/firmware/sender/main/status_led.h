#pragma once

#include <stdbool.h>

/*
 * 板载状态灯（WS2812 RGB，GPIO48）。
 *
 * 用颜色代替"看串口"——骑行时根本看不到串口：
 *   蓝色呼吸    接收端不在线（发了但没人接）
 *   灭          接收端在线（心跳正常）
 *   橙色常亮    模拟数据模式（发的是假数据，不是真实 OBD）
 *
 * 进入模拟模式时三连闪、退出闪一下、上电自检闪两下。
 *
 * 注意 GPIO48 上是**可寻址 RGB 灯**（WS2812），不是普通 LED，
 * 只能用 RMT 按单总线时序驱动，LEDC 的 PWM 对它无效。
 * 如果你的板子 GPIO48 接的是普通单色 LED（少见），把 LED_USE_WS2812 改成 0。
 */

/* 1 = 启用状态灯；0 = 完全禁用（排查用，先用它确认基础功能） */
#define LED_ENABLE        1

/* 全局亮度上限（百分比）。
 * WS2812 满亮（255）很刺眼，夜里骑行更明显，50 相当于一半亮度。
 * 这条对呼吸、常亮、三连闪一律生效，改一个数就能整体调暗/调亮。
 * 建议范围 20~80：太低看不清状态，太高刺眼。 */
#define LED_BRIGHT_PCT    50

#define LED_GPIO          48
#define LED_USE_WS2812    1

typedef enum {
    LED_STATE_NO_LINK = 0,   /* 蓝色呼吸 */
    LED_STATE_LINKED,        /* 灭 */
    LED_STATE_SIM,           /* 橙色常亮 */
} led_state_t;

/** 初始化。上电会自检闪两下——看到闪说明引脚和灯型都对。失败不致命。 */
void status_led_init(void);

/** 状态变化时调用。内部检测变化并触发对应动画。 */
void status_led_set(led_state_t st);
