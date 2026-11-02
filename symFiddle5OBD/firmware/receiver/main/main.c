#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "datasrc.h"
#include "link.h"
#include "perf.h"
#include "power.h"
#include "ui_gauge.h"
#include "ui_settings.h"
#include "ui_theme.h"
#include "version.h"

static const char *TAG = "receiver";

#define PAGE_W      466
#define PAGE_MS     190

#define TEST_AUTOSLIDE 0
#define BOOT_ON_SETTINGS 0  /* 1 = 开机直接进设置页，调试用 */

/* 数据源不再由编译期宏决定：运行时可切、状态存 NVS，设置页上有「演示模式」开关。
 * 见 datasrc.c。 */

static lv_obj_t *s_root;        /* 承载所有页面的容器（就是 screen） */
static lv_obj_t *s_page[2];     /* 0 = 仪表，1 = 设置 */
static int s_cur;               /* 当前停在哪一页 */
static bool s_animating;

static lv_obj_t *s_anim_in;
static lv_obj_t *s_anim_out;
static int32_t s_anim_sign;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* 页面底下的那层也跟主题走。
 * 两个页面本来就铺满屏幕、正常看不到它，但翻页动画里两页同时位移，
 * 万一有像素级的缝隙，黑色底边在白天主题下会闪一下。 */
static void root_apply_theme(void)
{
    if (s_root != NULL) {
        lv_obj_set_style_bg_color(s_root, ui_palette()->bg, 0);
    }
}

/* ---------- 翻页 ----------
 * 两个页面并排放在同一个 screen 里，靠移动 x 做真正的横向滑动。
 * 只有两页，所以「上一页 / 下一页」都落到另一页，但滑入方向跟着手势走：
 *   左滑 → 下一页从右边滑进来
 *   右滑 → 上一页从左边滑进来
 * 页码取模循环，来回滑手感一致。 */
static void anim_cb(void *var, int32_t v)
{
    LV_UNUSED(var);
    /* v: 0 → PAGE_W */
    lv_obj_set_x(s_anim_in, s_anim_sign * (PAGE_W - v));
    lv_obj_set_x(s_anim_out, -s_anim_sign * v);
}

static void anim_done_cb(lv_anim_t *a)
{
    LV_UNUSED(a);
    lv_obj_set_x(s_anim_in, 0);
    /* 出页停到对面去，下次滑之前反正还会被重新摆位 */
    lv_obj_set_x(s_anim_out, -s_anim_sign * PAGE_W);

    s_cur = (s_cur == 0) ? 1 : 0;
    s_animating = false;

    ui_gauge_set_visible(s_cur == 0);
    ESP_LOGI(TAG, "切到第 %d 页（%s）", s_cur, s_cur == 0 ? "仪表" : "设置");
}

static void slide(lv_dir_t dir)
{
    if (s_animating) {
        return;
    }
    int next = (s_cur + ((dir == LV_DIR_LEFT) ? 1 : -1) + 2) % 2;

    s_anim_in = s_page[next];
    s_anim_out = s_page[s_cur];
    s_anim_sign = (dir == LV_DIR_LEFT) ? 1 : -1;   /* 左滑：从右边进 */

    /* 进入页先摆到屏幕外的对应侧（它本来就在屏外，看不见） */
    lv_obj_set_x(s_anim_in, s_anim_sign * PAGE_W);
    lv_obj_set_x(s_anim_out, 0);

    if (next == 1) {
        ui_settings_refresh();     /* 进设置页前先刷一次电量/连接 */
    }
    ui_gauge_set_visible(false);   /* 动画期间不画指针，免得跟翻页打架 */

    s_animating = true;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, NULL);
    lv_anim_set_values(&a, 0, PAGE_W);
    lv_anim_set_time(&a, PAGE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_cb);
    lv_anim_set_completed_cb(&a, anim_done_cb);
    lv_anim_start(&a);
}

static void gesture_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
        slide(dir);
    }
}

#if TEST_AUTOSLIDE
static void autoslide_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    slide(LV_DIR_LEFT);
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "飞度5 接收端启动（ESP-NOW 数据源）");
    /* 版本串带编译时刻，串口和设置页都能看到，
     * 用来确认板上跑的到底是哪一次构建 */
    ESP_LOGI(TAG, "固件版本 %s（编译于 %s）", version_string(), version_build_time());

    /* 主题存 NVS，先把它准备好 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 初始化失败，主题不会被记住");
    }

    /* BSP 负责：CO5300 QSPI 屏初始化、CST9217 触摸、LVGL 任务 */
    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "显示初始化失败");
        return;
    }
    bsp_display_backlight_on();
    ESP_LOGI(TAG, "显示就绪 %dx%d", (int)lv_display_get_horizontal_resolution(disp),
             (int)lv_display_get_vertical_resolution(disp));

    perf_attach(disp);

    /* AXP2101 电量（I2C，探不到就降级成「未知」，不影响界面） */
    power_init();

    /* 数据源：ESP-NOW 链路 + 本地演示模式，统一由 datasrc 管理。
     * 链路初始化失败也不影响界面，只是连接状态一直显示"未连接"。 */
    datasrc_init();

    ui_theme_init();

    /* LVGL 不是线程安全的，界面操作全部在锁内做。
     * 注意：bsp_display_lock 的参数是 uint32_t 透传给 esp_lv_adapter_lock(int32_t)，
     * 传 0 表示「不等待、立即返回失败」——不是「永久等待」。
     * 永久等待要传 -1，这里直接调 adapter 的接口避免类型坑。 */
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        ESP_LOGE(TAG, "拿不到 LVGL 锁");
        return;
    }

    s_root = lv_screen_active();
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_scrollable(s_root, false);
    lv_obj_set_style_bg_color(s_root, ui_palette()->bg, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);

    s_page[0] = ui_gauge_create(s_root);
    s_page[1] = ui_settings_create(s_root);
    lv_obj_set_pos(s_page[0], 0, 0);
    lv_obj_set_pos(s_page[1], PAGE_W, 0);
    s_cur = 0;

    lv_obj_add_event_cb(s_root, gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_page[0], gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_page[1], gesture_cb, LV_EVENT_GESTURE, NULL);
    /* 设置页有一个可垂直滚动的内容容器，它会吃掉 press 事件，
     * 横向手势不一定冒泡到页面，所以在它身上再挂一份，保证设置页也能左右翻。 */
    lv_obj_t *set_body = ui_settings_scrollable();
    if (set_body != NULL) {
        lv_obj_add_event_cb(set_body, gesture_cb, LV_EVENT_GESTURE, NULL);
    }

    ui_theme_subscribe(ui_gauge_apply_theme);
    ui_theme_subscribe(ui_settings_apply_theme);
    ui_theme_subscribe(root_apply_theme);

    ui_gauge_set_visible(true);

#if BOOT_ON_SETTINGS
    /* 临时：开机直接停在设置页，方便看设置页自身的重绘行为 */
    slide(LV_DIR_LEFT);
#endif

#if TEST_AUTOSLIDE
    /* 临时：每 3 秒自动翻一次页，用来量翻页动画的真实帧率。测完删掉。 */
    lv_timer_create(autoslide_cb, 3000, NULL);
#endif

    esp_lv_adapter_unlock();

    uint32_t t0 = now_ms();
    uint32_t last_refresh = 0;

    while (1) {
        /* 跟屏幕节奏：等上一帧刷完再推下一帧。
         * 不这么做的话，主循环推得比刷新快，一个刷新周期里会攒下好几个脏区，
         * 每个都要独立渲染一遍，白烧一倍以上的 CPU。 */
        perf_wait_frame(50);

        uint32_t ms = now_ms() - t0;

        /* 数据源：演示模式走本地假数据，否则走 ESP-NOW。
         * 断连时 datasrc 返回 rpm=0 / clt=INT8_MIN（界面显示 "--"），
         * 绝不拿上一包的旧值顶替：骑手要看的是"当前转速"，不是"上次的转速"。 */
        uint16_t rpm = 0;
        int8_t   clt = INT8_MIN;
        datasrc_read(ms, &rpm, &clt);

        /* 每 500ms 回一个心跳给发送端，让它知道圆屏还在线（内部自己节流）。
         * 发送端那边据此点亮/熄灭板载 LED。 */
        link_service();

        /* 链路统计每 5 秒一行：串口上能直接分辨
         *   "收 0 坏 0"     → 完全没收到（发送端没开机 / 不同信道 / 版本不符）
         *   "收 xx 坏 xx"   → 收到了但被拒收（协议版本不一致）
         *   "丢" 持续增长   → 距离/干扰问题
         */
        static uint32_t last_link_log;
        if (ms - last_link_log >= 5000u) {
            last_link_log = ms;
            datasrc_status_t st;
            datasrc_status(&st);
            if (st.demo) {
                ESP_LOGI(TAG, "演示模式（本地假数据），不看链路");
            } else if (st.ever) {
                static const char *can_txt[] = {"未知", "正常", "无数据"};
                ESP_LOGI(TAG, "链路 %s：收 %lu 丢 %lu 坏 %lu rssi=%ddBm | CAN %s",
                         st.link_up ? "在线" : "超时",
                         (unsigned long)st.rx_count, (unsigned long)st.lost,
                         (unsigned long)st.bad, (int)st.rssi, can_txt[st.can]);
            } else {
                ESP_LOGI(TAG, "链路：从未收到（发送端是否开机？信道是否 %d？）", LINK_CHANNEL);
            }
        }

        uint32_t t_lock0 = now_ms();
        if (esp_lv_adapter_lock(1000) == ESP_OK) {
            uint32_t t_upd0 = now_ms();
            ui_gauge_update(rpm, clt);
            /* 电量/连接状态固定 1 秒刷一次，不依赖当前停在哪一页：
             * 只靠翻页时刷一次的话，插拔电源后界面不会跟着变。 */
            if (!s_animating && ms - last_refresh >= 1000) {
                last_refresh = ms;
                ui_settings_refresh();
            }
            uint32_t upd = now_ms() - t_upd0;
            esp_lv_adapter_unlock();
            perf_tick(now_ms() - t_lock0, upd);
        } else {
            ESP_LOGW(TAG, "抢锁超时 1000ms");
            perf_tick(1000, 0);
        }

        perf_report();
    }
}
