#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * OBD-II 模式 01（当前数据）的 PID 解码。
 * 纯 C，不依赖 ESP-IDF —— 这样可以在 PC 上直接跑单元测试。
 */

/* 本工程关心的 PID。全部经 2026-10-31 实车 ELM327 + Car Scanner 实测确认。
 *
 * 注意 OBD_PID_SPEED：本车实测 PID 有效但**恒返回 0**（Car Scanner 显示
 * 0 km/h，转前轮时原车仪表动、诊断口读数不动）。说明 ECU 认识这个 PID，
 * 但轮速信号直连仪表、没有接入 ECU。obd_poll 不轮询它——读到的 0 不能
 * 显示，显示 0 km/h 会让骑手误判为停车。
 *
 * 0xA6 非标准，部分厂商把总里程放这里。本车改用标准 PID 0x31（清码后行程）
 * 作为里程源，0xA6 降为后备。 */
typedef enum {
    OBD_PID_LOAD       = 0x04,  /* 计算负荷 % */
    OBD_PID_COOLANT    = 0x05,  /* 冷却液温度 °C */
    OBD_PID_MAP        = 0x0B,  /* 进气歧管绝对压力 kPa */
    OBD_PID_RPM        = 0x0C,  /* 发动机转速 rpm */
    OBD_PID_SPEED      = 0x0D,  /* 车速 km/h（本车恒 0，不用） */
    OBD_PID_TIMING     = 0x0E,  /* 点火提前角 ° */
    OBD_PID_IAT        = 0x0F,  /* 进气温度 °C */
    OBD_PID_MAF        = 0x10,  /* 空气流量 g/s */
    OBD_PID_TPS        = 0x11,  /* 节气门开度 % */
    OBD_PID_RUN_SEC    = 0x1F,  /* ECU 本次上电运行秒数 s */
    OBD_PID_FUEL       = 0x2F,  /* 油箱液位 % */
    OBD_PID_DIST_CLEAR = 0x31,  /* 清码后行驶距离 km（里程主源） */
    OBD_PID_BARO       = 0x33,  /* 大气压 kPa */
    OBD_PID_VOLT       = 0x42,  /* 控制模块电压 V */
    OBD_PID_OILTEMP    = 0x5C,  /* 机油温度 °C */
    OBD_PID_ODOMETER   = 0xA6,  /* 总里程 km（非标准，后备） */
} obd_pid_t;

/*
 * 解码一个 PID 响应。
 *   pid : PID 编号
 *   d   : 数据字节指针，指向 PID 之后的第一字节（即 A）
 *   n   : d 中可用字节数
 *   out : 解码结果
 * 返回 0 = 成功，-1 = 该 PID 不支持或字节数不足。
 */
int obd_decode(uint8_t pid, const uint8_t *d, uint8_t n, float *out);

/* 该 PID 需要几个数据字节，未知 PID 返回 0 */
uint8_t obd_pid_bytes(uint8_t pid);

#ifdef __cplusplus
}
#endif
