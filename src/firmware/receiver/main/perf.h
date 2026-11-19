#pragma once
#include <stdint.h>
#include "lvgl.h"

/* 把渲染耗时拆开打点，用来定位帧率瓶颈到底在"逐像素变换"还是"刷屏"。
 * 事件回调跑在 LVGL 任务里，不需要加锁；主循环读计数也不需要。 */
void perf_attach(lv_display_t *disp);

/* 主循环每轮调用一次：lock_wait_ms = 抢锁耗时，update_ms = 锁内耗时 */
void perf_tick(uint32_t lock_wait_ms, uint32_t update_ms);

/* 每 2 秒由主循环调用一次，打印累计均值并清零 */
void perf_report(void);

/* 阻塞等到「上一帧真正刷完」（LV_EVENT_REFR_READY）。
 * 主循环用它可以做到「一屏一更新」，不让脏区在一个刷新周期里叠加，
 * 否则 LVGL 会把几个脏区渲染好几遍，白烧 CPU。
 * 返回 true 表示等到了，false 表示超时。 */
bool perf_wait_frame(uint32_t timeout_ms);
