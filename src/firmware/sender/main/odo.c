#include "odo.h"

#include <stdbool.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "odo";

#define NVS_NS        "symodo"
#define NVS_KEY_MAX   "max"     /* 历史最大显示值，0.1km */
#define NVS_KEY_OFS   "ofs"     /* 清码补偿量，0.1km */
#define NVS_KEY_RAW   "raw"     /* 上次 ECU 原始值，0.1km，用于跨重启的回落判定 */

/* 判定"清码归零"的回落门槛。
 * 0x31 精度是 1 km 且单调递增，正常行驶不可能回落。留 50 km 余量
 * 是为了容忍 ECU 内部计数器的小幅抖动（有些实现会在点火循环边界
 * 差个几公里），低于这个幅度按噪声处理，不动 offset。 */
#define RESET_DROP_DAM 500u

static uint32_t s_max_dam;     /* 历史上见过的最大值 */
static uint32_t s_offset_dam;  /* 清码补偿 */
static uint32_t s_last_dam;    /* 上次 ECU 原始值（未加 offset） */
static bool     s_have_last;
static bool     s_valid;
static bool     s_dirty;
static bool     s_nvs_ok;

void odo_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open 失败，里程不会持久化");
        return;
    }
    s_nvs_ok = true;

    uint32_t v = 0;
    if (nvs_get_u32(h, NVS_KEY_MAX, &v) == ESP_OK) {
        s_max_dam = v;
        ESP_LOGI(TAG, "从 NVS 恢复里程 %.1f km", (double)(v / 10.0f));
    }
    if (nvs_get_u32(h, NVS_KEY_OFS, &v) == ESP_OK) {
        s_offset_dam = v;
        if (v > 0) {
            ESP_LOGI(TAG, "清码补偿量 %.1f km", (double)(v / 10.0f));
        }
    }
    /* 恢复上次的原始读数。这一步是必要的：如果在上电期间清了码，
     * s_have_last 为 false 会让首次读数跳过回落判定，里程就冻结在
     * 旧值上再也不动。 */
    if (nvs_get_u32(h, NVS_KEY_RAW, &v) == ESP_OK) {
        s_last_dam = v;
        s_have_last = true;
    }
    nvs_close(h);
}

uint32_t odo_feed_km(uint16_t km)
{
    uint32_t dam = (uint32_t)km * 10u;

    /* 判定范围覆盖跨重启的清码：NVS 里存有上次原始值时，init 已把
     * s_have_last 置真，首次读数也会参与比较。只有全新装机（NVS 空）
     * 才跳过——那时 s_last_dam 是 0，任何值都会被误判成回落。 */
    if (s_have_last && dam + RESET_DROP_DAM < s_last_dam) {
        uint32_t drop = s_last_dam - dam;
        s_offset_dam += drop;
        s_dirty = true;
        ESP_LOGW(TAG, "里程回落 %.1f km，判定为清码，累加补偿（累计 %.1f km）",
                 (double)(drop / 10.0f), (double)(s_offset_dam / 10.0f));
    }
    s_last_dam = dam;
    s_have_last = true;

    uint32_t disp = dam + s_offset_dam;
    if (!s_valid) {
        s_valid = true;
        if (s_max_dam == 0) {
            /* 只在全新装机时打，避免深睡唤醒后反复刷同一条 */
            ESP_LOGI(TAG, "ECU 里程 %.1f km，开始跟踪", (double)(disp / 10.0f));
        }
    }
    if (disp > s_max_dam) {
        s_max_dam = disp;
        s_dirty = true;
    }
    return s_max_dam;
}

uint32_t odo_get_dam(void)
{
    return s_max_dam;
}

bool odo_is_valid(void)
{
    return s_valid;
}

void odo_commit(void)
{
    if (!s_nvs_ok || !s_dirty) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    esp_err_t e1 = nvs_set_u32(h, NVS_KEY_MAX, s_max_dam);
    esp_err_t e2 = nvs_set_u32(h, NVS_KEY_OFS, s_offset_dam);
    esp_err_t e3 = ESP_OK;
    if (s_valid) {
        e3 = nvs_set_u32(h, NVS_KEY_RAW, s_last_dam);
    }
    if (e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK && nvs_commit(h) == ESP_OK) {
        s_dirty = false;
    }
    nvs_close(h);
}
