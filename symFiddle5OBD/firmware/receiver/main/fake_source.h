#pragma once

#include <stdint.h>

/**
 * 假数据源：在接收端还没接上 ESP-NOW 之前，用它把界面跑起来。
 *
 * 内部是一条循环的"骑行曲线"：冷车怠速 → 起步 → 巡航 → 拉高转 → 回落，
 * 水温从 41°C 缓慢升到 90°C 左右后维持。
 */

/** 按毫秒时间推进，取当前这一帧应有的读数。 */
void fake_source_get(uint32_t ms, uint16_t *rpm, int8_t *coolant);
