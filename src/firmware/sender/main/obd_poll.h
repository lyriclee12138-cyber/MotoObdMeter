#pragma once
#include <stdbool.h>
#include "telemetry.h"

/*
 * OBD-II 轮询调度器。
 *
 * 单线程、阻塞式：每次 tick 最多发一个 PID 请求并等响应。
 * 之所以不做并发，是因为 500k 总线上单 ECU 的响应很快（几 ms），
 * 而并发会让请求与响应错配——这是新手最常踩的坑。
 */

void obd_poll_init(void);

/* 调一次最多完成一个 PID 的请求/响应周期，并更新 t。
 * 内部会阻塞到收到响应或超时（约 120ms）。 */
void obd_poll_tick(telemetry_t *t);

/* 最近 5 秒内是否收到过任何 ECU 帧。用于判断车是否熄火 */
bool obd_poll_bus_alive(void);
