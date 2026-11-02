#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

/** 建立仪表页（作为 parent 的子对象）。必须在 LVGL 锁内调用。 */
lv_obj_t *ui_gauge_create(lv_obj_t *parent);

/** 仪表页的页面对象（可能为 NULL，未创建时） */
lv_obj_t *ui_gauge_screen(void);

/** 页面是否在前台。不在前台时不重绘指针（省 CPU，也避免跟翻页动画打架）。 */
void ui_gauge_set_visible(bool visible);

/**
 * 刷新一帧。
 * rpm       0–10000
 * coolant   °C，传入 INT8_MIN 表示无效
 */
void ui_gauge_update(uint16_t rpm, int8_t coolant);

/** 主题变了：换背景图、重设水温字色，并整屏重绘。 */
void ui_gauge_apply_theme(void);
