#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * 里程管理。
 *
 * 数据源是标准 PID 0x31（Distance traveled since codes cleared），单位 km。
 * 这不是"总里程" PID（标准里根本没有），但本车实测值 11582 km，与表显
 * 里程同一量级，对保养提醒完全够用。
 *
 * 用它而不用非标准 0xA6 的理由：0x31 是 SAE J1979 定义的 PID，任何符合
 * EOBD 的 ECU 都必须支持；0xA6 是厂商私有的，能不能读到全看运气。
 *
 * 两个已知缺陷，本模块就是为它们存在的：
 *
 *  1. 清故障码会让 0x31 归零。直接透传的话，某天清了码里程就从 11582
 *     跳成 1，比不显示更糟。这里用 offset 补偿：检测到显著回落时把差额
 *     记进 offset，显示值继续单调上升。
 *
 *  2. 65535 km 后回绕。真到那个里程这车也该报废了，不做处理。
 */

void     odo_init(void);

/* 喂入 ECU 报告的公里数，返回应显示的里程（单位 0.1km）。
 * 内部做清码检测与 offset 补偿，返回值保证不回退。 */
uint32_t odo_feed_km(uint16_t km);

/* 当前里程（0.1km）。未从 ECU 读到过任何值时返回 0 */
uint32_t odo_get_dam(void);

/* 是否已经从 ECU 得到过有效里程。false 时界面应显示 "--" 而不是 0 */
bool     odo_is_valid(void);

/* 强制落盘。进深睡前调用 */
void     odo_commit(void);
