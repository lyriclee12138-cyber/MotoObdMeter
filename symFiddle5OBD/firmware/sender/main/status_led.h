#pragma once

#include <stdbool.h>

/*
 * 板载状态灯。
 *
 * 三种状态（用颜色/亮度区分，比看串口方便得多——骑行时根本看不到串口）：
 *   呼吸      接收端不在线（发了但没人接）
 *   灭        接收端在线（心跳正常）
 *   常亮      模拟数据模式（正在发假数据，不是真实 OBD）
 *
 * 进入模拟模式时会先三连闪作为"收到"的反馈，退出时闪一下。
 *
 * 引脚：默认 GPIO2（绝大多数 ESP32-S3 N16R8 开发板的板载 LED）。
 * 如果你的板子灯不亮，多半是引脚不同 —— 改下面这个宏重新编译即可。
 * 也有些板子是低电平点亮，那就把 LED_ACTIVE_LOW 改成 1。
 */

#define LED_GPIO         2
#define LED_ACTIVE_LOW   0

typedef enum {
    LED_STATE_NO_LINK = 0,   /* 呼吸 */
    LED_STATE_LINKED,        /* 灭 */
    LED_STATE_SIM,           /* 常亮 */
} led_state_t;

/** 初始化（上电会自检闪 3 下，看到闪说明引脚配对）。失败不致命。 */
void status_led_init(void);

/** 状态变化时调用。内部检测变化并触发对应动画。 */
void status_led_set(led_state_t st);
