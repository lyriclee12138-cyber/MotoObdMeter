#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * TWAI（CAN 2.0）底层封装。
 * 500 kbps / 11 bit —— 已由 ELM327 实测确认：ISO 15765-4 CAN。
 *
 * 这一层只做：装驱动、收发帧、bus-off 自愈。
 * 协议语义（ISO-TP / PID）不在这里。
 */

#define CAN_TX_GPIO 4
#define CAN_RX_GPIO 5

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[8];
} can_frame_t;

esp_err_t can_link_init(void);

/* 发送一帧标准帧。dlc 超过 8 会被截断 */
esp_err_t can_link_send(uint32_t id, const uint8_t *data, uint8_t dlc);

/* 收一帧。返回 1 = 收到，0 = 超时，-1 = 出错 */
int can_link_recv(can_frame_t *out, uint32_t timeout_ms);

/* 主循环里定期调用：处理 bus-off / 恢复 / 重启 */
void can_link_service(void);

/* 最近一次成功收到帧的时间戳（ms，上电起算）。从未收到返回 0 */
uint32_t can_link_last_rx_ms(void);

/* 上电自检：被动监听 ms 毫秒，把收到的帧打到日志，返回收到的帧数。
 *
 * 这是接好线后第一步该跑的东西 —— 不依赖任何 OBD 请求，只看物理层通不通。
 * 收到帧 = 接线、共地、波特率、极性全对；一帧没有 = 问题在物理层，
 * 别去怀疑 ISO-TP 或者 PID 表。
 *
 * 最多打印 24 帧（再多就刷屏了），末尾汇总「多少帧 / 多少个不同 ID」。 */
int can_link_sniff(uint32_t ms);
