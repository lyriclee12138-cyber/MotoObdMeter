#include "ui_settings.h"

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "lvgl.h"
#include <string.h>
#include "nvs.h"
#include "datasrc.h"
#include "link.h"
#include "power.h"
#include "ui_theme.h"
#include "version.h"

static const char *TAG = "settings";

/* 和 ui_theme.c 用同一个命名空间，亮度单独一个 key */
#define NVS_NS       "symui"
#define NVS_KEY_BRI  "bri"

LV_FONT_DECLARE(font_cjk_16);
LV_FONT_DECLARE(font_cjk_22);
LV_FONT_DECLARE(font_cjk_32);

/* 屏幕是圆形（466×466，圆心 233,233），四角会被表框切掉，
 * 所以所有内容都收在半径 ~225 以内：卡片宽 320、x=73，行的 y 中心距圆心不超过 150。 */
#define SCR_W      466

/* 临时诊断：电量副标题显示原始状态位，排查完改回 0 */
#define DEBUG_RAW_POWER 0   /* 1 = 电量副标题显示原始状态位，排查用 */

/* 屏幕是圆形（466×466，圆心 233,233），四角会被表框切掉。
 * 卡片宽 300、x=83 → 左右各留 83，圆边不会切字。
 *
 * 内容总高 CONTENT_H 故意做得比屏幕高，这样设置页能上下滑动：
 * 版本卡片有一半在首屏之外，要往下滑才看得全 —— 版本号平时不占视线，
 * 需要确认「板上跑的到底是哪一版」时滑到底即可。 */
#define CARD_X     83
#define CARD_W     300

#define CARD1_Y    84
#define CARD1_H    88      /* 电量（两行） */
#define CARD2_Y    186
#define CARD2_H    96      /* 发送端 + CAN（两行） */
#define CARD3_Y    294
#define CARD3_H    60      /* 主题 */
#define CARD4_Y    366
#define CARD4_H    60      /* 亮度 */
#define CARD5_Y    438
#define CARD5_H    60      /* 演示模式 */
#define CARD6_Y    510
#define CARD6_H    86      /* 版本 */

#define HINT_Y     608
/* 内容实际排到的高度。滚动量 = CONTENT_H - 466 ≈ 174px。
 * 注意：s_body 自己的尺寸仍然是屏高 466，靠「子对象超出它」来产生滚动；
 * 如果把 s_body 直接设成 640 高，它内部就没有溢出了，滚不动。 */
#define CONTENT_H  640

/* 亮度滑条：自己用基础对象画（本工程没开 LV_USE_SLIDER）。
 * 布局是「上面一行标题 + 下面一整条横贯卡片的轨道」。
 * 可点区域横向铺满卡片（早期只有 120px 宽、24px 高的一小块，手指很难戳中），
 * 纵向只覆盖轨道那一带——如果连标题文字也覆盖，点「亮度」两个字会把亮度跳到 0%。 */
#define BRI_MIN     10      /* 再低屏幕基本看不见，且 0 会直接黑屏 */
#define BRI_PAD_X   20      /* 轨道距卡片左右边 */
#define BRI_TRACK_W (CARD_W - 2 * BRI_PAD_X)   /* 260 */
#define BRI_HIT_Y   30      /* 可点区域在卡片内的 y（22px 字体行高 24，标题占 6~30） */
#define BRI_HIT_H   30      /* 可点区域高度：整整 30px，比原来的 24px 更好按 */
#define BRI_BAR_Y   6       /* 轨道在可点区域内的 y */
#define BRI_BAR_H   12
#define BRI_KNOB    22

#define N_CARDS     6

static lv_obj_t *s_scr;
static lv_obj_t *s_body;          /* 可垂直滚动的内容容器（高 CONTENT_H，比屏幕高） */
static lv_obj_t *s_cards[N_CARDS];
static lv_obj_t *s_lbl[16];       /* 需要随主题改色的静态文字 */
static int s_lbl_n;

static lv_obj_t *s_bat_val;       /* "100%" */
static lv_obj_t *s_bat_sub;       /* "USB 供电 · 已充满" */
static lv_obj_t *s_link_txt;      /* "已连接" / "未连接" */
static lv_obj_t *s_link_dot;
static lv_obj_t *s_can_txt;       /* "正常" / "无数据" / "未连接" */
static lv_obj_t *s_can_dot;
static lv_obj_t *s_demo_btn;      /* 演示模式开关按钮 */
static lv_obj_t *s_demo_txt;      /* "开" / "关" */
static lv_obj_t *s_ver_txt;       /* "v2026.11.02-0953_local" */
static lv_obj_t *s_scroll_dbg;    /* 临时：卡片右下角显示可滚动距离 */

static lv_obj_t *s_btn_day;
static lv_obj_t *s_btn_night;
static lv_obj_t *s_btn_day_txt;
static lv_obj_t *s_btn_night_txt;

/* 亮度 */
static lv_obj_t *s_bri_track;     /* 可点区域（透明） */
static lv_obj_t *s_bri_bar;       /* 底槽 */
static lv_obj_t *s_bri_fill;      /* 已选部分 */
static lv_obj_t *s_bri_knob;
static lv_obj_t *s_bri_txt;       /* "80%" */
static int s_bri = 80;

/* ---------- 小工具 ---------- */
static lv_obj_t *mk_card(lv_obj_t *parent, int y, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, CARD_X, y);
    lv_obj_set_size(c, CARD_W, h);
    lv_obj_set_style_radius(c, 22, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_scrollable(c, false);
    lv_obj_set_clickable(c, false);
    return c;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *txt, const lv_font_t *fnt, lv_color_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    /* 顺序很重要：必须先设字体再设文字。
     * 反过来的话，LVGL 会先用默认字体（Montserrat，没有中文字形、advance=0）
     * 去算内容宽度，算出来接近 0，标签就被压成 0 宽，中文整段画不出来。 */
    lv_obj_set_style_text_font(l, fnt, 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_label_set_text(l, txt);
    lv_obj_set_clickable(l, false);
    if (s_lbl_n < (int)(sizeof(s_lbl) / sizeof(s_lbl[0]))) {
        s_lbl[s_lbl_n++] = l;
    }
    return l;
}

static void btn_evt(lv_event_t *e)
{
    ui_theme_t t = (ui_theme_t)(lv_intptr_t)lv_event_get_user_data(e);
    ui_theme_set(t);
}

/* 演示模式：点一下切换，状态存 NVS。
 * 开了之后不管有没有发送端，仪表都跑本地假数据 —— 只有圆屏一块板子时也能看效果。 */
static void demo_evt(lv_event_t *e)
{
    LV_UNUSED(e);
    datasrc_set_demo(!datasrc_demo());
    ui_settings_refresh();
}

static lv_obj_t *mk_btn(lv_obj_t *parent, int x, const char *txt, ui_theme_t t, lv_obj_t **txt_out)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    /* 垂直居中要按**父卡片的实际高度**算，不能写死某个卡片的宏：
     * 发送端卡片加高到 96 之后，写死 (CARD2_H-42)/2 会让主题按钮在 60 高的卡片里溢出 9px。 */
    lv_obj_set_pos(b, x, (lv_obj_get_height(parent) - 42) / 2);
    lv_obj_set_size(b, 76, 42);
    lv_obj_set_style_radius(b, 21, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(b, false);
    lv_obj_set_clickable(b, true);
    lv_obj_add_event_cb(b, btn_evt, LV_EVENT_CLICKED, (void *)(lv_intptr_t)t);

    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &font_cjk_16, 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    lv_obj_set_clickable(l, false);
    if (txt_out) {
        *txt_out = l;
    }
    return b;
}

/* ---------- 亮度滑条 ----------
 * 本工程没开 LV_USE_SLIDER，用基础对象自己拼：
 *   透明容器(可点) → 底槽 → 已选填充 → 圆钮
 * 拖动时直接改背光（LCD 命令 0x51），松手才写 NVS。 */
static void bri_save(int pct)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, NVS_KEY_BRI, pct);
        nvs_commit(h);
        nvs_close(h);
    }
}

static int bri_load(void)
{
    int v = 80;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t got = 0;
        if (nvs_get_i32(h, NVS_KEY_BRI, &got) == ESP_OK && got >= BRI_MIN && got <= 100) {
            v = (int)got;
        }
        nvs_close(h);
    }
    return v;
}

/* 只更新界面与背光，不落盘 */
static void bri_apply(int pct)
{
    if (pct < BRI_MIN) {
        pct = BRI_MIN;
    }
    if (pct > 100) {
        pct = 100;
    }
    s_bri = pct;

    if (s_bri_track) {
        int32_t w = (int32_t)BRI_TRACK_W * pct / 100;
        lv_obj_set_width(s_bri_fill, w);
        lv_obj_set_x(s_bri_knob, BRI_PAD_X + w - BRI_KNOB / 2);
        lv_label_set_text_fmt(s_bri_txt, "%d%%", pct);
    }
    bsp_display_brightness_set(pct);
}

static void bri_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_area_t a;
    lv_obj_get_coords(s_bri_track, &a);
    /* track 现在是整张卡片，要减掉左右留白才是滑块的真实行程 */
    int rel = (int)(p.x - a.x1) - BRI_PAD_X;
    if (rel < 0) {
        rel = 0;
    }
    if (rel > BRI_TRACK_W) {
        rel = BRI_TRACK_W;
    }
    bri_apply(rel * 100 / BRI_TRACK_W);

    if (code == LV_EVENT_RELEASED) {
        bri_save(s_bri);
        ESP_LOGI(TAG, "亮度 → %d%%", s_bri);
    }
}

/* ---------- 主题着色 ---------- */
void ui_settings_apply_theme(void)
{
    const ui_palette_t *p = ui_palette();
    if (s_scr == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(s_scr, p->bg, 0);

    for (int i = 0; i < N_CARDS; i++) {
        lv_obj_set_style_bg_color(s_cards[i], p->card, 0);
        lv_obj_set_style_border_color(s_cards[i], p->card_edge, 0);
    }
    /* 亮度滑条跟着主题换色 */
    if (s_bri_bar) {
        lv_obj_set_style_bg_color(s_bri_bar, p->card_edge, 0);
        lv_obj_set_style_bg_color(s_bri_fill, p->accent, 0);
        lv_obj_set_style_bg_color(s_bri_knob, p->accent, 0);
    }
    for (int i = 0; i < s_lbl_n; i++) {
        lv_obj_set_style_text_color(s_lbl[i], p->text, 0);
    }
    /* 次要文字单独再压一档 */
    if (s_bat_sub) {
        lv_obj_set_style_text_color(s_bat_sub, p->sub, 0);
    }

    bool day = (ui_theme_get() == UI_THEME_DAY);
    lv_obj_set_style_bg_color(s_btn_day, day ? p->accent : p->card_edge, 0);
    lv_obj_set_style_text_color(s_btn_day_txt, day ? p->accent_txt : p->sub, 0);
    lv_obj_set_style_bg_color(s_btn_night, day ? p->card_edge : p->accent, 0);
    lv_obj_set_style_text_color(s_btn_night_txt, day ? p->sub : p->accent_txt, 0);

    lv_obj_invalidate(s_scr);

    /* 连接/CAN 状态和演示按钮的颜色是按主题算的，切主题后要立刻跟上，
     * 不然要等下一次每秒刷新才变。 */
    ui_settings_refresh();
}

/* ---------- 内容刷新 ---------- */
void ui_settings_refresh(void)
{
    if (s_scr == NULL) {
        return;
    }
    power_info_t pi;
    power_get(&pi);
    const ui_palette_t *p = ui_palette();

#if DEBUG_RAW_POWER
    /* 临时诊断：原始状态位 + 自增计数。
     *  - 计数在涨  → 屏幕重绘正常，问题在寄存器读数（vbus 一直是 1）
     *  - 计数不动  → 屏幕不重绘，去查重绘链路
     *  - s0/s1 的十六进制值能直接看出插拔电源时寄存器到底变没变 */
    static uint32_t dbg_n;
    dbg_n++;
    lv_label_set_text_fmt(s_bat_sub, "s0=%02X d%d v%d #%lu",
                          (unsigned)pi.raw_st0, (int)pi.raw_dir, (int)pi.vbus,
                          (unsigned long)dbg_n);
#else
    if (pi.present && pi.bat_ok) {
        lv_label_set_text_fmt(s_bat_val, "%d%%", pi.percent);

        if (pi.charging) {
            lv_label_set_text(s_bat_sub, "充电中");
        } else if (pi.vbus && pi.percent >= 99) {
            lv_label_set_text(s_bat_sub, "USB 供电 · 已充满");
        } else if (pi.vbus) {
            lv_label_set_text(s_bat_sub, "USB 供电");
        } else {
            /* 注意：CONFIG_LV_USE_FLOAT 是关的，lv_label_set_text_fmt 里的 %.2f 打不出来，
             * 所以自己拆成整数部分和两位小数 */
            lv_label_set_text_fmt(s_bat_sub, "电池 · %u.%02uV",
                                  (unsigned)(pi.mv / 1000u), (unsigned)((pi.mv % 1000u) / 10u));
        }
    } else if (pi.present) {
        lv_label_set_text(s_bat_val, "--");
        lv_label_set_text(s_bat_sub, pi.vbus ? "USB 供电" : "电量未知");
    } else {
        lv_label_set_text(s_bat_val, "--");
        lv_label_set_text(s_bat_sub, "未检测到电源管理芯片");
    }
#endif  /* DEBUG_RAW_POWER */

    /* 发送端连接状态 + CAN 嗅探状态。
     * CAN 的判断不需要协议里额外加字段：收到包但 flags 里没有 TF_RPM，
     * 就说明发送端在跑、但 OBD 那边没读到 —— 正是「CAN 到底通没通」的答案。 */
    datasrc_status_t st;
    datasrc_status(&st);

    if (st.demo) {
        lv_label_set_text(s_link_txt, "演示中");
        lv_obj_set_style_text_color(s_link_txt, p->sub, 0);
        lv_obj_set_style_bg_color(s_link_dot, p->bad, 0);
    } else {
        const bool linked = st.link_up;
        lv_label_set_text(s_link_txt, linked ? "已连接" : "未连接");
        lv_obj_set_style_text_color(s_link_txt, linked ? p->good : p->sub, 0);
        lv_obj_set_style_bg_color(s_link_dot, linked ? p->good : p->bad, 0);
    }

    if (st.demo) {
        lv_label_set_text(s_can_txt, "旁路");
        lv_obj_set_style_text_color(s_can_txt, p->sub, 0);
        lv_obj_set_style_bg_color(s_can_dot, p->bad, 0);
    } else if (st.can == CAN_SNIFF_OK) {
        lv_label_set_text(s_can_txt, "正常");
        lv_obj_set_style_text_color(s_can_txt, p->good, 0);
        lv_obj_set_style_bg_color(s_can_dot, p->good, 0);
    } else if (st.can == CAN_SNIFF_EMPTY) {
        /* 连着但没数据：这是最需要被看见的状态，用警示色 */
        lv_label_set_text(s_can_txt, "无数据");
        lv_obj_set_style_text_color(s_can_txt, p->warn, 0);
        lv_obj_set_style_bg_color(s_can_dot, p->warn, 0);
    } else {
        lv_label_set_text(s_can_txt, "未知");
        lv_obj_set_style_text_color(s_can_txt, p->sub, 0);
        lv_obj_set_style_bg_color(s_can_dot, p->bad, 0);
    }

    /* 文案有变化才打日志，方便核对状态机（插拔电源时能直接看到） */
    static char last[64];
    char now_txt[64];
    lv_snprintf(now_txt, sizeof(now_txt), "%s | %s",
                lv_label_get_text(s_bat_val), lv_label_get_text(s_bat_sub));
    if (strcmp(last, now_txt) != 0) {
        strncpy(last, now_txt, sizeof(last) - 1);
        last[sizeof(last) - 1] = '\0';
        ESP_LOGI(TAG, "电量卡片 → %s（副标题宽 %d 高 %d）", now_txt,
                 (int)lv_obj_get_width(s_bat_sub), (int)lv_obj_get_height(s_bat_sub));
    }

    /* 演示模式按钮：开着用强调色，关着用卡片边色 */
    if (s_demo_btn != NULL) {
        const bool demo = datasrc_demo();
        lv_label_set_text(s_demo_txt, demo ? "开" : "关");
        lv_obj_set_style_bg_color(s_demo_btn, demo ? p->accent : p->card_edge, 0);
        lv_obj_set_style_text_color(s_demo_txt, demo ? p->accent_txt : p->sub, 0);
    }
}

/* ---------- 创建 ---------- */
lv_obj_t *ui_settings_create(lv_obj_t *parent)
{
    const ui_palette_t *p = ui_palette();

    s_scr = lv_obj_create(parent);
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_size(s_scr, SCR_W, SCR_W);
    lv_obj_set_pos(s_scr, 0, 0);
    lv_obj_set_style_bg_color(s_scr, p->bg, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_scr, false);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);

    /* --- 滚动容器 ---
     * 只允许垂直滚动（scroll_dir = VER），这一点很关键：
     * 横向拖动不会被它当成滚动吃下去，而是照常产生 GESTURE 冒泡到页面，翻页功能不受影响。
     * 滚动条关掉——圆形屏幕边上出现一条竖条很难看。
     *
     * 尺寸必须是「一个屏幕大」，不能设成 CONTENT_H：
     * LVGL 的滚动是「子对象超出容器」触发的，容器自己撑大就没有溢出了，
     * 表现就是滑不动（第一版就栽在这）。 */
    s_body = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_pos(s_body, 0, 0);
    lv_obj_set_size(s_body, SCR_W, SCR_W);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_body, 0, 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_set_scroll_dir(s_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_OFF);
    /* 必须可点，否则收不到 press 事件、也就滚不动 */
    lv_obj_set_clickable(s_body, true);
    lv_obj_add_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);

    /* 标题 */
    lv_obj_t *title = mk_label(s_body, "设置", &font_cjk_32, p->text);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    /* --- 卡片 1：电量 --- */
    s_cards[0] = mk_card(s_body, CARD1_Y, CARD1_H);
    lv_obj_t *l1 = mk_label(s_cards[0], "电量", &font_cjk_22, p->text);
    lv_obj_set_pos(l1, 20, 18);

    s_bat_sub = lv_label_create(s_cards[0]);
    lv_obj_set_style_text_font(s_bat_sub, &font_cjk_16, 0);
    lv_obj_set_style_text_color(s_bat_sub, p->sub, 0);
    lv_label_set_text(s_bat_sub, "--");
    lv_obj_set_pos(s_bat_sub, 20, 52);

    s_bat_val = lv_label_create(s_cards[0]);
    lv_obj_set_style_text_font(s_bat_val, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(s_bat_val, p->text, 0);
    lv_label_set_text(s_bat_val, "--");
    lv_obj_align(s_bat_val, LV_ALIGN_RIGHT_MID, -18, 0);
    if (s_lbl_n < (int)(sizeof(s_lbl) / sizeof(s_lbl[0]))) {
        s_lbl[s_lbl_n++] = s_bat_val;
    }

    /* --- 卡片 2：发送端 + CAN 嗅探（两行） ---
     * 这里回答两个不同的问题：
     *   发送端 —— ESP-NOW 收得到包吗（500ms 无包即断连）
     *   CAN    —— 收到了包，但发送端那边 OBD 到底读到数据没有
     * 第二个才是"CAN 通没通"的判据：收得到包、flags 里却没有 TF_RPM，
     * 说明发送端在跑但总线没应答。 */
    s_cards[1] = mk_card(s_body, CARD2_Y, CARD2_H);

    lv_obj_t *l2a = mk_label(s_cards[1], "发送端", &font_cjk_16, p->text);
    lv_obj_set_pos(l2a, 20, 22);

    s_link_dot = lv_obj_create(s_cards[1]);
    lv_obj_remove_style_all(s_link_dot);
    lv_obj_set_size(s_link_dot, 10, 10);
    lv_obj_set_style_radius(s_link_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_link_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_link_dot, p->bad, 0);
    lv_obj_set_clickable(s_link_dot, false);
    lv_obj_set_pos(s_link_dot, 116, 26);

    s_link_txt = mk_label(s_cards[1], "未连接", &font_cjk_16, p->sub);
    lv_obj_set_pos(s_link_txt, 136, 22);

    /* 第二行：CAN */
    lv_obj_t *l2b = mk_label(s_cards[1], "CAN", &font_cjk_16, p->text);
    lv_obj_set_pos(l2b, 20, 62);

    s_can_dot = lv_obj_create(s_cards[1]);
    lv_obj_remove_style_all(s_can_dot);
    lv_obj_set_size(s_can_dot, 10, 10);
    lv_obj_set_style_radius(s_can_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_can_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_can_dot, p->bad, 0);
    lv_obj_set_clickable(s_can_dot, false);
    lv_obj_set_pos(s_can_dot, 116, 66);

    s_can_txt = mk_label(s_cards[1], "未知", &font_cjk_16, p->sub);
    lv_obj_set_pos(s_can_txt, 136, 62);

    /* --- 卡片 3：主题 --- */
    s_cards[2] = mk_card(s_body, CARD3_Y, CARD3_H);
    lv_obj_t *l3 = mk_label(s_cards[2], "主题", &font_cjk_22, p->text);
    lv_obj_align(l3, LV_ALIGN_LEFT_MID, 22, 0);

    s_btn_day = mk_btn(s_cards[2], CARD_W - 176, "白天", UI_THEME_DAY, &s_btn_day_txt);
    s_btn_night = mk_btn(s_cards[2], CARD_W - 92, "黑夜", UI_THEME_NIGHT, &s_btn_night_txt);

    /* --- 卡片 4：亮度 ---
     * 两行：上面「亮度 ... 80%」，下面一整条横贯卡片的轨道。
     * s_bri_track 铺满整张卡片，所以在卡片任意位置按下/拖动都能调亮度，
     * 不再要求手指精确戳中那条细轨。 */
    s_cards[3] = mk_card(s_body, CARD4_Y, CARD4_H);
    lv_obj_t *l4 = mk_label(s_cards[3], "亮度", &font_cjk_22, p->text);
    lv_obj_align(l4, LV_ALIGN_TOP_LEFT, 20, 6);
    s_bri_txt = mk_label(s_cards[3], "80%", &font_cjk_16, p->sub);
    lv_obj_align(s_bri_txt, LV_ALIGN_TOP_RIGHT, -20, 9);

    /* 可点容器：横向铺满卡片、纵向覆盖轨道那一带。
     * y 从 BRI_HIT_Y 开始，避开上面的标题文字，免得点「亮度」把亮度跳到 0%。 */
    s_bri_track = lv_obj_create(s_cards[3]);
    lv_obj_remove_style_all(s_bri_track);
    lv_obj_set_pos(s_bri_track, 0, BRI_HIT_Y);
    lv_obj_set_size(s_bri_track, CARD_W, BRI_HIT_H);
    lv_obj_set_style_bg_opa(s_bri_track, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollable(s_bri_track, false);
    lv_obj_set_clickable(s_bri_track, true);
    lv_obj_add_event_cb(s_bri_track, bri_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_bri_track, bri_touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_bri_track, bri_touch_cb, LV_EVENT_RELEASED, NULL);

    /* 底槽：横贯卡片 */
    s_bri_bar = lv_obj_create(s_bri_track);
    lv_obj_remove_style_all(s_bri_bar);
    lv_obj_set_pos(s_bri_bar, BRI_PAD_X, BRI_BAR_Y);
    lv_obj_set_size(s_bri_bar, BRI_TRACK_W, BRI_BAR_H);
    lv_obj_set_style_radius(s_bri_bar, BRI_BAR_H / 2, 0);
    lv_obj_set_style_bg_opa(s_bri_bar, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_bri_bar, false);

    /* 已选填充 */
    s_bri_fill = lv_obj_create(s_bri_track);
    lv_obj_remove_style_all(s_bri_fill);
    lv_obj_set_pos(s_bri_fill, BRI_PAD_X, BRI_BAR_Y);
    lv_obj_set_size(s_bri_fill, 0, BRI_BAR_H);
    lv_obj_set_style_radius(s_bri_fill, BRI_BAR_H / 2, 0);
    lv_obj_set_style_bg_opa(s_bri_fill, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_bri_fill, false);

    /* 圆钮 */
    s_bri_knob = lv_obj_create(s_bri_track);
    lv_obj_remove_style_all(s_bri_knob);
    lv_obj_set_size(s_bri_knob, BRI_KNOB, BRI_KNOB);
    lv_obj_set_pos(s_bri_knob, BRI_PAD_X - BRI_KNOB / 2,
                   BRI_BAR_Y + (BRI_BAR_H - BRI_KNOB) / 2);
    lv_obj_set_style_radius(s_bri_knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_bri_knob, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_bri_knob, false);

    /* 读回上次的亮度并应用（同时初始化界面上的位置） */
    s_bri = bri_load();

    /* --- 卡片 5：演示模式 ---
     * 切换后立刻生效，不用重启：datasrc_read() 每帧都会看这个开关。
     * 开着的时候仪表跑本地假数据，跟发送端有没有开机无关。 */
    s_cards[4] = mk_card(s_body, CARD5_Y, CARD5_H);
    lv_obj_t *l5 = mk_label(s_cards[4], "演示模式", &font_cjk_16, p->text);
    lv_obj_set_pos(l5, 20, 22);

    s_demo_btn = lv_obj_create(s_cards[4]);
    lv_obj_remove_style_all(s_demo_btn);
    lv_obj_set_size(s_demo_btn, 76, 42);
    lv_obj_align(s_demo_btn, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_set_style_radius(s_demo_btn, 21, 0);
    lv_obj_set_style_bg_opa(s_demo_btn, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_demo_btn, false);
    lv_obj_set_clickable(s_demo_btn, true);
    lv_obj_add_event_cb(s_demo_btn, demo_evt, LV_EVENT_CLICKED, NULL);

    s_demo_txt = lv_label_create(s_demo_btn);
    lv_obj_set_style_text_font(s_demo_txt, &font_cjk_16, 0);
    lv_label_set_text(s_demo_txt, "关");
    lv_obj_center(s_demo_txt);
    lv_obj_set_clickable(s_demo_txt, false);

    /* --- 卡片 6：版本 ---
     * 放最下面，首屏只露出卡片上沿，往下滑才看得全。
     * 版本串带编译时刻，用来确认「板上跑的到底是哪一次构建」：
     * 烧完如果版本号没变，说明固件根本没烧进去（手机端工具不一定会硬复位，
     * 屏不自动重启是正常的，但版本号一定会变）。 */
    s_cards[5] = mk_card(s_body, CARD6_Y, CARD6_H);
    lv_obj_t *l6 = mk_label(s_cards[5], "版本", &font_cjk_22, p->text);
    lv_obj_set_pos(l6, 20, 12);

    s_ver_txt = mk_label(s_cards[5], version_string(), &font_cjk_16, p->sub);
    lv_obj_set_pos(s_ver_txt, 20, 48);

    /* 临时诊断：把可滚动距离画在卡片右下角。
     * 非 0 = 内容确实溢出、理论上能滑；0px = 内容没溢出（那才是真的滑不动）。
     * 板上没串口，只能把值画到屏上，确认滚动正常后可以删掉这一段。 */
    s_scroll_dbg = lv_label_create(s_cards[5]);
    lv_obj_set_style_text_font(s_scroll_dbg, &font_cjk_16, 0);
    lv_obj_set_style_text_color(s_scroll_dbg, p->dim, 0);
    lv_label_set_text(s_scroll_dbg, "--");
    lv_obj_align(s_scroll_dbg, LV_ALIGN_BOTTOM_RIGHT, -14, -10);
    lv_obj_set_clickable(s_scroll_dbg, false);

    /* 底部提示 */
    lv_obj_t *hint = lv_label_create(s_body);
    lv_obj_set_style_text_font(hint, &font_cjk_16, 0);
    lv_obj_set_style_text_color(hint, p->dim, 0);
    lv_label_set_text(hint, "上下滑动看版本 · 左右滑动切换");
    lv_obj_set_pos(hint, 0, HINT_Y);
    lv_obj_set_width(hint, SCR_W);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_clickable(hint, false);

    ui_settings_apply_theme();
    bri_apply(s_bri);
    ui_settings_refresh();

    /* 布局跑完之后才能拿到真实的可滚动距离。
     * 打到屏上（卡片右下角）和日志两处 —— 板上没串口时只能看屏。 */
    lv_obj_update_layout(s_body);
    int scroll_px = (int)lv_obj_get_scroll_bottom(s_body);
    lv_label_set_text_fmt(s_scroll_dbg, "%dpx", scroll_px);
    ESP_LOGI(TAG, "设置页已建立（亮度 %d%%），内容可滚动 %d px", s_bri, scroll_px);
    return s_scr;
}

lv_obj_t *ui_settings_screen(void)
{
    return s_scr;
}

lv_obj_t *ui_settings_scrollable(void)
{
    return s_body;
}
