#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "telemetry.h"

/*
 * ESP-NOW 下行链路（接收端这一侧的"收"）。
 *
 * 发送端 10Hz 广播 23 字节 telemetry_t，这里只做三件事：
 *   1. 校验 magic/version，丢掉不属于本工程的杂包（同信道里可能有别的 ESP-NOW 设备）
 *   2. 存一份最新快照，主循环无锁竞争地取用
 *   3. 统计丢包（按 seq 跳变），设置页和串口日志用来判断链路质量
 *
 * 不做 ACK、不做重传：仪表数据过期就显示 "--"，有 100ms 后的下一包补上。
 * 为丢一包去重传，反而会把延迟做大。
 */

#define LINK_CHANNEL  1        /* 必须和发送端 uplink.h 的 UPLINK_CHANNEL 一致 */
#define LINK_STALE_MS 500      /* 超过这么久没收到就认为断连（10Hz 下约等于丢 5 包） */

typedef struct {
    telemetry_t t;             /* 最新一帧原始数据 */
    uint32_t    rx_ms;         /* 最近收包时刻（上电起算） */
    uint32_t    count;         /* 累计有效包数 */
    uint32_t    lost;          /* 按 seq 推算的累计丢包数 */
    uint32_t    bad;           /* magic/version 不符或长度不对的包数 */
    int8_t      rssi;          /* 最近一包的信号强度 dBm */
    bool        ever;          /* 上电后是否收到过至少一包 */
} link_state_t;

/* 初始化 Wi-Fi（STA，不连热点）+ ESP-NOW 接收。失败不致命：
 * 界面照常跑，只是连接状态一直显示"未连接"。 */
esp_err_t link_init(void);

/* 取一份快照。返回 true = 数据新鲜，可以直接显示；
 * false = 从未收到或已超时，调用方应显示 "--"。 */
bool link_get(link_state_t *out);

/** 主循环低频调用（每帧一次即可，内部按 500ms 节流）。
 *  收到过发送端的包之后，定期回一个心跳，让发送端知道圆屏还在线。 */
void link_service(void);
