#pragma once
#include <stdint.h>

/*
 * 接收端 → 发送端的单向心跳。
 *
 * 为什么要这个：ESP-NOW 是单向广播，发送端调 esp_now_send() 返回 OK 只代表
 * "进了发送队列"，**它无法知道接收端到底在不在**。骑行时如果圆屏断了电，
 * 发送端依然在傻发，LED 也看不出来。
 *
 * 所以让接收端回一个极短的包：接收端收到 telemetry 后记住源地址，
 * 之后每 500ms 发一次心跳；发送端 1.5 秒收不到就认为掉线。
 *
 * 4 字节，比 telemetry 的 23 字节轻得多，10Hz 下带宽可以忽略。
 * 用独立的 magic，避免和 telemetry 混淆（同信道可能有别的 ESP-NOW 设备）。
 */

#define HB_MAGIC    0xA5
#define HB_KIND_RX  0x01        /* 接收端发的心跳 */

typedef struct __attribute__((packed)) {
    uint8_t  magic;             /* 固定 HB_MAGIC */
    uint8_t  kind;              /* HB_KIND_RX */
    uint16_t seq;               /* 递增，便于将来做丢包统计 */
} heartbeat_t;                  /* sizeof == 4 */

_Static_assert(sizeof(heartbeat_t) == 4, "heartbeat_t 必须保持 4 字节");

/* 超过这么久没收到心跳就认为掉线（3 个心跳周期） */
#define HB_TIMEOUT_MS 1500
/* 接收端发心跳的间隔 */
#define HB_PERIOD_MS  500
