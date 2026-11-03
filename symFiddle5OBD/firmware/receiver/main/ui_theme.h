#pragma once
#include "lvgl.h"

/* 界面主题。白天 = beta-1 基线；黑夜 = 暗金属盘面。 */
typedef enum {
    UI_THEME_DAY = 0,
    UI_THEME_NIGHT = 1,
} ui_theme_t;

typedef struct {
    lv_color_t bg;          /* 设置页整屏底色 */
    lv_color_t card;        /* 卡片底 */
    lv_color_t card_edge;   /* 卡片描边 */
    lv_color_t text;        /* 主文字 */
    lv_color_t sub;         /* 次要文字 */
    lv_color_t accent;      /* 选中态底 */
    lv_color_t accent_txt;  /* 选中态字 */
    lv_color_t good;        /* 已连接 */
    lv_color_t warn;        /* 连着但没数据（CAN 嗅探失败） */
    lv_color_t bad;         /* 未连接 */
    lv_color_t dim;         /* 提示字 */
    lv_color_t gauge_temp;  /* 表盘水温数字（正常态） */
    lv_color_t track;       /* 表盘进度环的空槽底色，必须与背景图里烘焙的一致 */
} ui_palette_t;

void ui_theme_init(void);                       /* 从 NVS 读，没有就用白天 */
ui_theme_t ui_theme_get(void);
void ui_theme_set(ui_theme_t t);                /* 切换 + 存 NVS + 通知订阅者 */
const ui_palette_t *ui_palette(void);

/* 注册主题变化回调。必须在 LVGL 锁内被调用（ui_theme_set 本身就是锁内用的）。 */
void ui_theme_subscribe(void (*cb)(void));
