#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ISO 15765-2（ISO-TP）接收端最小实现。
 * 纯 C，不依赖 ESP-IDF，可在 PC 上单测。
 *
 * 只做接收方向的分包重组，以及生成流控帧。
 * 单 ECU 场景（只有 0x7E8 应答），所以只维护一份装配缓冲。
 *
 * 帧类型由首字节高 4 位决定：
 *   0x0  SF 单帧      len 在低 4 位
 *   0x1  FF 首帧      总长 12 bit（低 4 位 + 下一字节）
 *   0x2  CF 连续帧    序号在低 4 位，从 1 开始循环
 *   0x3  FC 流控帧    （本工程不处理收到的 FC）
 */

#define ISOTP_MAX_PAYLOAD 64

typedef struct {
    uint16_t id;                      /* 正在装配的报文来源 ID */
    uint16_t total;                   /* 报文总长（FF 里带过来的） */
    uint16_t got;                     /* 已收字节 */
    uint8_t  next_sn;                 /* 期待的下一个连续帧序号 */
    uint8_t  active;                  /* 1 = 正在装配多帧 */
    uint8_t  overflow;                /* 1 = 超出缓冲，本次报文作废 */
    uint8_t  buf[ISOTP_MAX_PAYLOAD];
} isotp_rx_t;

void isotp_reset(isotp_rx_t *s);

/*
 * 送入一帧 CAN 数据。
 * 返回  1 = 完整报文已就绪，数据在 s->buf，长度 s->total
 *       0 = 还没收完（或只是中间帧）
 *      -1 = 格式错误 / 溢出（该报文已作废）
 */
int isotp_feed(isotp_rx_t *s, uint16_t id, const uint8_t *d, uint8_t dlc);

/*
 * 收到首帧后应回一个流控帧。返回写入 out 的字节数（3），
 * 不需要发流控时返回 0（本实现总是返回 3，块大小 0、间隔 0）。
 */
int isotp_flow_control(uint8_t *out);

#ifdef __cplusplus
}
#endif
