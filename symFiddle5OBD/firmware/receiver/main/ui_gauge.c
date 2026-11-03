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

/* lv_draw_arc 的 radius 是**外**半径。
 * 194/11 → 带 183..194，与背景图里烘焙的轨道和彩环完全重合。
 * （早先是 200，比烘焙的轨道外扩 6px，彩色弧看着浮在轨道外面。） */
#define ARC_R_OUT    194
#define ARC_W        11
#define ARC_R_MID    (ARC_R_OUT - ARC_W / 2.0f)     /* 188.5 */

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
static bool     s_need_full = true;   /* 首帧要把整条彩环擦一遍（见 ui_gauge_update） */
/* 屏幕上**实际画到**的位置（0..1）。注意它和 s_shown 不是一回事：
 * 只有真正重绘过的位置才算数，否则脏区起点会漂移，见 ui_gauge_update 的注释。 */
static float    s_painted;
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

/* ---------- 进度弧（自绘） ----------
 *
 * **颜色不再由运行时计算，而是烘焙进背景图。**
 *
 * 背景图（gauge_art_v2.py 渲染）里已经有一条完整的绿→黄→红彩环。
 * 每帧只做两件事：
 *   1. 用轨道底色画一条「当前转速 → 满量程」的弧，把还没到的部分擦掉；
 *   2. 在针尖处画一个圆，做出圆头。
 *
 * 为什么这么改（踩过两次，别再退回分段着色）：
 * 弧是靠脏区增量重绘的，每帧只重画转速变化扫过的那一小段。早先的写法是
 * 「每 1000 转画一段、段色由该段位置算出」，只要某一帧的重绘范围和绘制内容
 * 错开半个身位，错误颜色就会永久留在屏上——表现就是低转速区残留橘黄、
 * 弧中间断一节，而且怎么改颜色函数都没用。
 * 现在每个角度的颜色是像素自带的，重绘时机再错也只会表现为「弧长了一点」
 * 或「短了一点」，不可能把红画到低速区。
 */
static void arc_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = lv_event_get_target(e);

    float frac = s_shown / (float)RPM_MAX;
    if (frac < 0.0f) {
        frac = 0.0f;
    }
    if (frac > 1.0f) {
        frac = 1.0f;
    }

    /* 圆心要用对象的**绝对**坐标：翻页动画里整个页面会横向位移。
     * 角度换算 a = θ - 90（θ 是「12 点为 0、顺时针」，a 是 LVGL 的「3 点为 0」），
     * 起点 θ=-135°（左下 7:30）→ a=135°。 */
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    const int32_t cx = coords.x1 + (int32_t)CX;
    const int32_t cy = coords.y1 + (int32_t)CY;

    /* 1. 擦掉未填充的部分 */
    if (frac < 0.999f) {
        lv_draw_arc_dsc_t ad;
        lv_draw_arc_dsc_init(&ad);
        ad.center.x = cx;
        ad.center.y = cy;
        ad.radius = ARC_R_OUT;
        ad.width = ARC_W;
        ad.color = ui_palette()->track;
        ad.opa = LV_OPA_COVER;
        ad.rounded = 0;                                  /* 方口，圆头交给第 2 步 */
        ad.start_angle = 135 + (int32_t)(ANG_SPAN * frac);
        ad.end_angle = 405;       /* 135+270；>360 由底层取模后按跨过 0° 处理 */
        lv_draw_arc(layer, &ad);
    }

    /* 2. 针尖圆头：直径等于轨道宽度，压在擦除弧的方口上 */
    if (frac > 0.001f) {
        const float rad = (ANG_START + ANG_SPAN * frac) * 0.017453292f;
        const int32_t px = cx + (int32_t)lroundf(ARC_R_MID * sinf(rad));
        const int32_t py = cy - (int32_t)lroundf(ARC_R_MID * cosf(rad));
        const int32_t r = ARC_W / 2;

        lv_draw_rect_dsc_t rd;
        lv_draw_rect_dsc_init(&rd);
        rd.bg_color = arc_color_at(frac);                /* 与烘焙值同一套插值 */
        rd.bg_opa = LV_OPA_COVER;
        rd.radius = LV_RADIUS_CIRCLE;
        lv_area_t a = { px - r, py - r, px + r, py + r };
        lv_draw_rect(layer, &rd, &a);
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
    /* 这里只挡"完全没变"（0.00002 ≈ 0.2 转）。
     * 早先写的是 0.0004（4 转），慢速爬升时每帧只走 1.5 转，于是整段都被跳过、
     * 屏幕上的弧停住不动；更糟的是 s_shown 继续前进，等下一次快速拉升时
     * 重绘起点已经漂到前面，中间那一条永远没人画 —— 表现为弧上有一段断口。 */
    if (f1 - f0 < 0.00002f) {
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
    if (visible) {
        /* 隐藏期间 s_shown 一直在变，s_painted 已经过期，回来先整圈重画一次 */
        s_need_full = true;
    }
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
    s_painted = 0.0f;
    s_need_full = true;
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
    if (s_first) {
        s_shown = target;
        s_first = false;
        /* 背景图里烘焙的是**整条**彩环（100% 全亮）。首帧必须完整重绘一次，
         * 否则 0 转时屏幕上会留着一整圈彩色，看起来像满油门。
         * 注意不能靠 invalidate_sweep(0,0)——跨度太小时它是直接返回的。 */
        s_need_full = true;
    } else {
        s_shown += (target - s_shown) * RPM_SMOOTH;
        if (fabsf(target - s_shown) < 6.0f) {
            s_shown = target;
        }
    }

    if (s_visible) {
        const float frac = s_shown / (float)RPM_MAX;
        if (s_need_full) {
            s_need_full = false;
            invalidate_sweep(0.0f, 1.0f);
            s_painted = frac;
        } else if (fabsf(frac - s_painted) > 0.00005f) {   /* ≈0.5 转 */
            /* **必须从 s_painted 而不是 s_shown 的上一帧值开始失效**：
             * 两者在"某帧变化太小、被跳过"之后会分开。若按上一帧值算，
             * 起点会一路漂移，漂过的这一段屏幕上从没画过（露出轨道），
             * 下一段快速变化也不会补它 —— 弧上的断口就是这么来的。 */
            invalidate_sweep(s_painted, frac);
            s_painted = frac;
        }
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
