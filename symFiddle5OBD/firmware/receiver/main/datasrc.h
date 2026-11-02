#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * 数据源统一入口。
 *
 * 接收端有两个数据来源：
 *   1. ESP-NOW 下行的 telemetry_t（真实）
 *   2. 本地 fake_source（演示模式，不依赖发送端）
 *
 * 以前是 main.c 里一个宏 USE_FAKE_WHEN_NO_LINK 二选一，改不了；
 * 现在做成运行时可切换、状态存 NVS，设置页上有开关。
 *
 * 另外这里顺便把「发送端连着但 CAN 没数据」这种情况识别出来：
 * 收得到包、但 flags 里没有 TF_RPM，说明发送端在跑、OBD 那边没读到。
 * 这正是用户想确认的「CAN 嗅探到底通没通」。
 */

typedef enum {
    CAN_SNIFF_UNKNOWN = 0,   /* 没收到包，无从判断 */
    CAN_SNIFF_OK,            /* 收到包且转速字段有效 —— CAN 通了 */
    CAN_SNIFF_EMPTY,         /* 收到包但没有任何有效字段 —— 连着但 CAN 没数据 */
} can_sniff_t;

typedef struct {
    bool        demo;        /* 演示模式是否开启 */
    bool        link_up;     /* 当前有新鲜数据 */
    bool        ever;        /* 上电后是否收到过至少一包 */
    uint32_t    rx_count;    /* 累计有效包数 */
    uint32_t    lost;        /* 按 seq 推算的丢包数 */
    uint32_t    bad;         /* magic/version 不符的包数 */
    int8_t      rssi;        /* 最近一包信号强度 dBm */
    can_sniff_t can;         /* CAN 嗅探结论 */
} datasrc_status_t;

/** 读 NVS 恢复演示模式设置，并把链路初始化好。 */
void datasrc_init(void);

/** 演示模式开关（会写 NVS）。 */
bool datasrc_demo(void);
void datasrc_set_demo(bool on);

/**
 * 取当前该显示的数据。
 * demo 模式走本地假数据；否则走 ESP-NOW 链路，断开时返回 rpm=0 / clt=INT8_MIN。
 */
void datasrc_read(uint32_t ms, uint16_t *rpm, int8_t *clt);

/** 给设置页看的汇总状态。 */
void datasrc_status(datasrc_status_t *out);
