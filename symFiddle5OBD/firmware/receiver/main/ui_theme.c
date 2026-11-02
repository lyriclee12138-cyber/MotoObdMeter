#include "ui_theme.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "theme";

#define NVS_NS   "symui"
#define NVS_KEY  "theme"

static ui_theme_t s_theme = UI_THEME_DAY;
static void (*s_cb[4])(void);
static int s_cb_n;

/* 白天：沿用 beta-1 的观感。黑夜：暗底 + 提亮文字。 */
static const ui_palette_t k_pal[2] = {
    [UI_THEME_DAY] = {
        .bg         = { .red = 0xF2, .green = 0xF4, .blue = 0xF7 },
        .card       = { .red = 0xFF, .green = 0xFF, .blue = 0xFF },
        .card_edge  = { .red = 0xE2, .green = 0xE6, .blue = 0xEC },
        .text       = { .red = 0x2B, .green = 0x2E, .blue = 0x35 },
        .sub        = { .red = 0x6B, .green = 0x72, .blue = 0x80 },
        .accent     = { .red = 0x2B, .green = 0x2E, .blue = 0x35 },
        .accent_txt = { .red = 0xFF, .green = 0xFF, .blue = 0xFF },
        .good       = { .red = 0x22, .green = 0xC5, .blue = 0x5E },
        .warn       = { .red = 0xC9, .green = 0x7A, .blue = 0x0E },
        .bad        = { .red = 0xA8, .green = 0xAE, .blue = 0xB8 },
        .dim        = { .red = 0xB4, .green = 0xBA, .blue = 0xC4 },
        .gauge_temp = { .red = 0x2B, .green = 0x2E, .blue = 0x35 },
    },
    [UI_THEME_NIGHT] = {
        .bg         = { .red = 0x0E, .green = 0x10, .blue = 0x13 },
        .card       = { .red = 0x19, .green = 0x1C, .blue = 0x21 },
        .card_edge  = { .red = 0x2A, .green = 0x2F, .blue = 0x37 },
        .text       = { .red = 0xE7, .green = 0xEA, .blue = 0xEF },
        .sub        = { .red = 0x8C, .green = 0x93, .blue = 0x9C },
        .accent     = { .red = 0xE7, .green = 0xEA, .blue = 0xEF },
        .accent_txt = { .red = 0x14, .green = 0x17, .blue = 0x1B },
        .good       = { .red = 0x3D, .green = 0xD6, .blue = 0x8C },
        .warn       = { .red = 0xE8, .green = 0xA3, .blue = 0x3D },
        .bad        = { .red = 0x6B, .green = 0x72, .blue = 0x7C },
        .dim        = { .red = 0x55, .green = 0x5C, .blue = 0x66 },
        .gauge_temp = { .red = 0xE7, .green = 0xEA, .blue = 0xEF },
    },
};

const ui_palette_t *ui_palette(void)
{
    return &k_pal[s_theme == UI_THEME_NIGHT ? UI_THEME_NIGHT : UI_THEME_DAY];
}

ui_theme_t ui_theme_get(void)
{
    return s_theme;
}

void ui_theme_subscribe(void (*cb)(void))
{
    if (cb && s_cb_n < (int)(sizeof(s_cb) / sizeof(s_cb[0]))) {
        s_cb[s_cb_n++] = cb;
    }
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "打不开 NVS 命名空间，主题不会被记住");
        return;
    }
    nvs_set_u8(h, NVS_KEY, (uint8_t)s_theme);
    nvs_commit(h);
    nvs_close(h);
}

void ui_theme_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, NVS_KEY, &v) == ESP_OK && v <= UI_THEME_NIGHT) {
            s_theme = (ui_theme_t)v;
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "初始主题：%s", s_theme == UI_THEME_NIGHT ? "黑夜" : "白天");
}

void ui_theme_set(ui_theme_t t)
{
    if (t != UI_THEME_DAY && t != UI_THEME_NIGHT) {
        return;
    }
    if (t == s_theme) {
        return;
    }
    s_theme = t;
    save();
    ESP_LOGI(TAG, "切到%s", t == UI_THEME_NIGHT ? "黑夜" : "白天");
    for (int i = 0; i < s_cb_n; i++) {
        s_cb[i]();
    }
}
