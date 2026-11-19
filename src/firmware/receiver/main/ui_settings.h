#pragma once
#include "lvgl.h"

/** 建立设置页（作为 parent 的子对象）。必须在 LVGL 锁内调用。 */
lv_obj_t *ui_settings_create(lv_obj_t *parent);

lv_obj_t *ui_settings_screen(void);

/** 设置页里那个可垂直滚动的内容容器。
 *  横向拖动要能翻页，所以 main 往它身上也挂一份 GESTURE 回调
 *  （它可点、会吃掉 press，事件不一定冒泡到页面）。 */
lv_obj_t *ui_settings_scrollable(void);

/** 主题变了：重刷配色（卡片/文字/按钮）。 */
void ui_settings_apply_theme(void);

/** 刷新电量与连接状态文字。主循环低频调用（1–2 秒一次）。 */
void ui_settings_refresh(void);
