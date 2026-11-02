#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "telemetry.h"

/* ESP-NOW 上行链路。
 * 收发两端必须处在同一个 2.4G 信道，否则收不到——这里固定信道，
 * 不要依赖自动选择，后者在两端不同步时会表现为"完全无数据"。 */

#define UPLINK_CHANNEL 1

esp_err_t uplink_init(void);

/* 填 magic/version/seq 并广播。seq 内部自增 */
esp_err_t uplink_send(telemetry_t *t);

/* 接收端是否在线（收到过心跳且未超时）。
 * 广播是单向的，发送成功 ≠ 对端收到，所以要看这个。 */
bool uplink_peer_alive(void);
