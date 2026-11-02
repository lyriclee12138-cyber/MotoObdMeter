#pragma once
#include <stdint.h>

/*
 * 发送端与接收端共用的空中数据帧定义。
 * 两个工程（sender / receiver）都通过 IDF 组件 "shared" 引用这个头文件。
 *
 * 设计约束：
 *  - 固定长度二进制，不用 JSON。23 字节 vs 100+ 字节，且解析零开销。
 *  - packed，保证两端布局一致。
 *  - 必须带 flags：某项读不到时接收端显示 "--"，绝不能显示 0。
 *    骑行时一个假的 0 rpm 比不显示危险得多。
 *
 * 演进记录：
 *  v1 含 speed 字段。2026-10-31 实测确认本车 ECU 不提供车速
 *  （PID 0x0D 无响应；转前轮仪表有反应但 ECU 侧无数据，说明轮速信号
 *  直连仪表、不进 ECU），因此移除 speed，换为 run_min。
 *  接收端必须同步到 v2，否则会因 version 不符拒收。
 *
 *  v2 内部又改过一次语义（2026-10-31 晚），但**布局不变、仍为 23 字节**，
 *  所以版本号没动：odo_dam 的来源从"非标准 0xA6 探测"改为标准 PID 0x31
 *  （清码后行程），并由发送端做清码补偿。接收端无需改动。
 */

#define TELEMETRY_MAGIC    0x5A01
#define TELEMETRY_VERSION  2
#define TELEMETRY_BCAST    0xFF   /* 接收端 ESP-NOW 回调只认这一组 */

/* flags 位定义 —— 每一位代表对应字段本次是否有效 */
#define TF_RPM      (1u << 0)
#define TF_COOLANT  (1u << 1)
#define TF_TPS      (1u << 2)
#define TF_IAT      (1u << 3)
#define TF_FUEL     (1u << 4)
#define TF_VOLT     (1u << 5)
#define TF_ODO      (1u << 6)   /* 仅当 ECU 提供里程（PID 0x31 或 0xA6）时置位 */
#define TF_RUNTIME  (1u << 7)

/* 油量不可用时的哨兵值（0–100 之外即视为无效） */
#define FUEL_INVALID 0xFF

typedef struct __attribute__((packed)) {
    uint16_t magic;      /* 固定 TELEMETRY_MAGIC，接收端据此丢弃杂包 */
    uint8_t  version;    /* 协议版本，两端不一致时接收端应拒收 */
    uint16_t rpm;        /* 发动机转速，0–12000 */
    int8_t   coolant;    /* 水温 °C */
    int8_t   intake;     /* 进气温度 °C */
    uint8_t  tps;        /* 节气门开度 % */
    uint8_t  fuel;       /* 油量 %，不支持时为 FUEL_INVALID */
    uint16_t volt_dv;    /* 系统电压，单位 0.1V（138 = 13.8V） */
    uint16_t run_min;    /* 发动机累计运行分钟数，饱和于 65535（≈45 天） */
    uint32_t odo_dam;    /* 里程，单位 0.1km；来源为 ECU PID 0x31（清码后行程），
                          * 经发送端做清码补偿后单调不减。TF_ODO 未置位时无意义 */
    uint16_t flags;      /* TF_* 位或 */
    uint32_t seq;        /* 递增序号，接收端用差值判断丢包 */
} telemetry_t;           /* sizeof == 23 */

_Static_assert(sizeof(telemetry_t) == 23, "telemetry_t 布局变了，两端必须同步更新");
