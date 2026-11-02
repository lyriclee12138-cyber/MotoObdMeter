#include "ui_gauge.h"

#include "esp_log.h"
#include "gauge_assets.h"
#include "lvgl.h"
#include "ui_theme.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

static const char *UI_TAG = "ui";

/* ================= 几何 =================
 * 与 tools/gauge_art_v2.py 严格一致：角度以 12 点为 0°、顺时针为正，
 * 0 rpm → -135°，10000 rpm → +135°。改这里的话素材要跟着重渲染。 */
#define SCR_W        466
#define CX           233.0f
#define CY           233.0f

#define RPM_MAX      10000
#define ANG_START    (-135.0f)
#define ANG_SPAN     (270.0f)

/* lv_draw_arc 的 radius 是**外**半径，所以中心线 = 200 - 11/2 = 194.5 */
#define ARC_R_OUT    200
#define ARC_W        11
#define ARC_R_MID    (ARC_R_OUT - ARC_W / 2.0f)

/* 文字位置（屏幕绝对 y）。这些数字是素材渲染时就定好的，别随手改。 */
#define Y_RPM        208
#define Y_UNIT       272
#define Y_TEMP       392

/* 平滑系数：每帧向目标靠近这么多。1.0 = 不平滑。
 * 0.45 大约 4~5 帧收敛，能抹掉假数据的抖动，又不会明显滞后。 */
#define RPM_SMOOTH   0.45f

LV_FONT_DECLARE(font_num_112);
LV_FONT_DECLARE(font_num_40);
LV_FONT_DECLARE(font_cjk_16);

static lv_obj_t *s_scr;
static lv_obj_t *s_bg;        /* 静态层：底弧 + 刻度 + 刻度数字 */
static lv_obj_t *s_arc;       /* 透明层，在 DRAW_MAIN 里画进度弧 */
static lv_obj_t *s_rpm;       /* 中心大数字 */
static lv_obj_t *s_unit;      /* "×1000 RPM" */
static lv_obj_t *s_temp;      /* 水温 */

static uint16_t s_last_rpm = 0xFFFF;
static int8_t   s_last_clt = INT8_MAX;
static float    s_shown;      /* 平滑后的转速，画弧用这个 */
static bool     s_first = true;
static bool     s_visible = true;

/* ---------- 颜色 ---------- */
/* 弧的颜色由**角度位置**决定（pos = 0..1 沿弧的位置），不是由当前转速决定。
 *
 * 这一点很关键：弧是增量重绘的，每帧只重画变化的那一小段。
 * 如果颜色跟着"当前转速"走，升转过程中每段用的都是当时的颜色，
 * 弧上就会留下历史颜色的残影（低转画的绿 + 中转画的黄 + 高转画的橙堆在一起）；
 * 降转时剩余部分又不重画，颜色就定格在旧值上 —— 看着像"没刷新"。
 *
 * 改成按位置取色后，同一位置永远是同一个颜色，重绘哪段都不影响别处；
 * 转速跌落时脏区重绘会自动露出背景里的底弧，不需要额外的"擦除"逻辑。 */
static lv_color_t arc_color_at(float pos)
{
    static const uint8_t LO[3]  = {0x2E, 0xE5, 0x9D};   /* 绿 */
    static const uint8_t MID[3] = {0xF5, 0xC5, 0x42};   /* 黄 */
    static const uint8_t HI[3]  = {0xF0, 0x50, 0x3C};   /* 红 */

    if (pos < 0.0f) {
        pos = 0.0f;
    }
    if (pos > 1.0f) {
        pos = 1.0f;
    }

    const uint8_t *a;
    const uint8_t *b;
    float t;
    if (pos <= 0.5f) {
        a = LO;
        b = MID;
        t = pos * 2.0f;
    } else {
        a = MID;
        b = HI;
        t = (pos - 0.5f) * 2.0f;
    }
    uint8_t r  = (uint8_t)(a[0] + (b[0] - a[0]) * t + 0.5f);
    uint8_t g  = (uint8_t)(a[1] + (b[1] - a[1]) * t + 0.5f);
    uint8_t bl = (uint8_t)(a[2] + (b[2] - a[2]) * t + 0.5f);
    return lv_color_make(r, g, bl);
}

static lv_color_t temp_color(int8_t c)
{
    if (c >= 105) {
        return lv_color_hex(0xF0503C);      /* 过热 */
    }
    if (c < 55) {
        return lv_color_hex(0x3A8AFF);      /* 冷车 */
    }
    return ui_palette()->text;
}

/* ---------- 进度弧（自绘） ---------- */
/* 弧按位置分段绘制：每 1000 rpm 一段（正好和刻度数字 0-10 对齐），
 * 段内单色，整条弧看起来就是随位置渐变的。
 * 段边界用同一个整数角度，所以相邻段严丝合缝，不会露缝。 */
#define ARC_SEGS     10

static void arc_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = lv_event_get_target(e);

    float frac = s_shown / (float)RPM_MAX;
    if (frac <= 0.003f) {
        return;                              /* 熄火时不画，留一圈底弧就行 */
    }
    if (frac > 1.0f) {
        frac = 1.0f;
    }

    /* 圆心要用对象的**绝对**坐标：翻页动画里整个页面会横向位移，
     * 写死 (233,233) 的话弧会钉在屏幕上不跟着页面走。 */
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);

    /* 角度换算：我们的 θ 是「12 点为 0、顺时针」，LVGL 的 a 是「3 点为 0、顺时针」。
     * 两者指同一个点：x = CX + r·sinθ = CX + r·cos a，y = CY - r·cosθ = CY + r·sin a
     * → a = θ - 90。
     * 起始 θ = -135°（左下 7:30）→ a = -225° ≡ **135°**。 */
    const int32_t a_start = 135;

    int segs = (int)(frac * ARC_SEGS + 0.999f);
    if (segs < 1) {
        segs = 1;
    }

    for (int i = 0; i < segs; i++) {
        float p0 = (float)i / (float)ARC_SEGS;
        float p1 = (float)(i + 1) / (float)ARC_SEGS;
        if (p1 > frac) {
            p1 = frac;
        }
        if (p1 - p0 < 0.001f) {
            continue;
        }

        lv_draw_arc_dsc_t dsc;
        lv_draw_arc_dsc_init(&dsc);
        dsc.center.x = coords.x1 + (int32_t)CX;
        dsc.center.y = coords.y1 + (int32_t)CY;
        dsc.radius = ARC_R_OUT;
        dsc.width = ARC_W;
        dsc.color = arc_color_at((p0 + p1) * 0.5f);   /* 用段中点的位置取色 */
        dsc.opa = LV_OPA_COVER;
        /* 只在整条弧的两个端头做圆头，中间段用方头，否则每个接缝都会鼓起一个圆点 */
        dsc.rounded = (i == 0) || (p1 >= frac - 0.0001f);
        dsc.start_angle = a_start + (int32_t)(ANG_SPAN * p0);
        dsc.end_angle   = a_start + (int32_t)(ANG_SPAN * p1);

        lv_draw_arc(layer, &dsc);
    }
}

/* 只作废「弧从 f0 走到 f1」扫过的那一带。
 * 采样弧上的点取包围盒，再按弧宽的一半膨胀（含圆头端点）。
 * 转速缓变时这里只有几十像素宽，是全屏 217k 像素的千分之一。 */
static void invalidate_sweep(float f0, float f1)
{
    if (f1 < f0) {
        float t = f0;
        f0 = f1;
        f1 = t;
    }
    if (f1 - f0 < 0.0004f) {
        return;
    }

    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    const int N = 24;
    for (int i = 0; i <= N; i++) {
        float f = f0 + (f1 - f0) * (float)i / (float)N;
        float deg = ANG_START + ANG_SPAN * f;
        float rad = deg * 0.017453292f;
        float x = CX + ARC_R_MID * sinf(rad);
        float y = CY - ARC_R_MID * cosf(rad);
        if (x < minx) { minx = x; }
        if (x > maxx) { maxx = x; }
        if (y < miny) { miny = y; }
        if (y > maxy) { maxy = y; }
    }

    float pad = ARC_W * 0.5f + 2.0f;
    lv_area_t a;
    a.x1 = (int32_t)floorf(minx - pad);
    a.y1 = (int32_t)floorf(miny - pad);
    a.x2 = (int32_t)ceilf(maxx + pad);
    a.y2 = (int32_t)ceilf(maxy + pad);

    /* 上面算的是相对页面的坐标，lv_obj_invalidate_area 要的是屏幕绝对坐标 */
    lv_area_t coords;
    lv_obj_get_coords(s_scr, &coords);
    a.x1 += coords.x1;
    a.x2 += coords.x1;
    a.y1 += coords.y1;
    a.y2 += coords.y1;

    if (a.x1 < 0) { a.x1 = 0; }
    if (a.y1 < 0) { a.y1 = 0; }
    if (a.x2 > SCR_W - 1) { a.x2 = SCR_W - 1; }
    if (a.y2 > SCR_W - 1) { a.y2 = SCR_W - 1; }

    if (a.x1 <= a.x2 && a.y1 <= a.y2) {
        lv_obj_invalidate_area(s_scr, &a);
    }
}

/* ---------- 主题 ---------- */
void ui_gauge_apply_theme(void)
{
    const ui_palette_t *p = ui_palette();
    if (s_scr == NULL) {
        return;
    }

    lv_obj_set_style_bg_color(s_scr, p->bg, 0);
    lv_image_set_src(s_bg, (ui_theme_get() == UI_THEME_NIGHT) ? &bg_night_img : &bg_day_img);

    lv_obj_set_style_text_color(s_rpm, p->text, 0);
    lv_obj_set_style_text_color(s_unit, p->dim, 0);

    if (s_last_clt == INT8_MIN || s_last_clt == INT8_MAX) {
        lv_obj_set_style_text_color(s_temp, p->dim, 0);
    } else {
        lv_obj_set_style_text_color(s_temp, temp_color(s_last_clt), 0);
    }

    lv_obj_invalidate(s_scr);
}

void ui_gauge_set_visible(bool visible)
{
    s_visible = visible;
}

/* ---------- 创建 ---------- */
lv_obj_t *ui_gauge_create(lv_obj_t *parent)
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

    /* 1. 静态层：底弧、刻度、刻度数字全在这一张图里 */
    s_bg = lv_image_create(s_scr);
    lv_image_set_src(s_bg, &bg_night_img);
    lv_obj_align(s_bg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_clickable(s_bg, false);

    /* 2. 进度弧：一张透明的全屏层，内容在 DRAW_MAIN 里现画 */
    s_arc = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_arc);
    lv_obj_set_size(s_arc, SCR_W, SCR_W);
    lv_obj_set_pos(s_arc, 0, 0);
    lv_obj_set_style_bg_opa(s_arc, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(s_arc, false);
    lv_obj_add_event_cb(s_arc, arc_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    /* 3. 中心转速：字体只含 0-9 和点，line_height=83，字形中心就是 label 中心。
     * 宽度固定、文字居中——否则 "9.9"→"10.0" 时 label 会重新居中，数字左右跳。 */
    s_rpm = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_rpm, &font_num_112, 0);
    lv_obj_set_style_text_color(s_rpm, p->text, 0);
    lv_obj_set_width(s_rpm, 280);
    lv_obj_set_style_text_align(s_rpm, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_rpm, "0.0");
    lv_obj_align(s_rpm, LV_ALIGN_CENTER, 0, Y_RPM - (int)CY);

    /* 4. 单位 */
    s_unit = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_unit, &font_cjk_16, 0);
    lv_obj_set_style_text_color(s_unit, p->dim, 0);
    lv_obj_set_width(s_unit, 280);
    lv_obj_set_style_text_align(s_unit, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_unit, "×1000 RPM");
    lv_obj_align(s_unit, LV_ALIGN_CENTER, 0, Y_UNIT - (int)CY);

    /* 5. 水温 */
    s_temp = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_temp, &font_num_40, 0);
    lv_obj_set_style_text_color(s_temp, p->dim, 0);
    lv_obj_set_width(s_temp, 200);
    lv_obj_set_style_text_align(s_temp, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_temp, "--");
    lv_obj_align(s_temp, LV_ALIGN_CENTER, 0, Y_TEMP - (int)CY);

    s_shown = 0.0f;
    s_first = true;
    s_last_rpm = 0xFFFF;
    s_last_clt = INT8_MAX;

    ui_gauge_apply_theme();
    ESP_LOGI(UI_TAG, "仪表页 v2 建立（极简弧线版）");
    return s_scr;
}

lv_obj_t *ui_gauge_screen(void)
{
    return s_scr;
}

/* ---------- 每帧更新 ---------- */
void ui_gauge_update(uint16_t rpm, int8_t coolant)
{
    if (s_scr == NULL) {
        return;
    }

    /* 1. 弧：向目标平滑靠近，每帧只重绘扫过的一小段 */
    float target = (float)rpm;
    float prev = s_shown;
    if (s_first) {
        s_shown = target;
        s_first = false;
    } else {
        s_shown += (target - s_shown) * RPM_SMOOTH;
        if (fabsf(target - s_shown) < 6.0f) {
            s_shown = target;
        }
    }

    if (s_visible && fabsf(s_shown - prev) > 1.0f) {
        invalidate_sweep(prev / (float)RPM_MAX, s_shown / (float)RPM_MAX);
    }

    /* 2. 中心数字：只在显示值真的变化时才 set_text。
     * 注意不能写 "%.1f"——本工程 CONFIG_LV_USE_FLOAT 是关的，浮点格式化打不出来。 */
    if (rpm != s_last_rpm) {
        s_last_rpm = rpm;
        lv_label_set_text_fmt(s_rpm, "%u.%u",
                              (unsigned)(rpm / 1000u),
                              (unsigned)((rpm % 1000u) / 100u));
    }

    /* 3. 水温 */
    if (coolant != s_last_clt) {
        s_last_clt = coolant;
        if (coolant == INT8_MIN) {
            lv_label_set_text(s_temp, "--");
            lv_obj_set_style_text_color(s_temp, ui_palette()->dim, 0);
        } else {
            lv_label_set_text_fmt(s_temp, "%d°C", coolant);
            lv_obj_set_style_text_color(s_temp, temp_color(coolant), 0);
        }
    }
}
