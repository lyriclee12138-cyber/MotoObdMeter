#include "fake_source.h"

#include <stddef.h>

/*
 * 一段 20 秒的骑行脚本，循环播放。用关键帧 + 线性插值，避免引入随机数。
 * 选这几个点是为了把界面的关键状态都覆盖到：绿色低转、黄色中段、橙色红区、
 * 以及水温的冷车蓝 / 正常黑 / 过热红。
 */
typedef struct {
    uint16_t t_ms;
    uint16_t rpm;
    int8_t   clt;
} key_t;

static const key_t k_keys[] = {
    {     0, 1450, 41 },   /* 冷车怠速 */
    {  3000, 1500, 44 },
    {  6500, 5200, 58 },   /* 起步 → 巡航 */
    { 11000, 5600, 78 },
    { 14000, 9000, 84 },   /* 拉高转 */
    { 16500, 9200, 86 },
    { 18500, 3200, 83 },   /* 收油回落 */
    { 20000, 1450, 80 },   /* 回到怠速，进入下一循环 */
};
#define N_KEYS (sizeof(k_keys) / sizeof(k_keys[0]))
#define LOOP_MS 20000u

void fake_source_get(uint32_t ms, uint16_t *rpm, int8_t *coolant)
{
    uint32_t t = ms % LOOP_MS;

    size_t i = 0;
    while (i + 2 < N_KEYS && t > k_keys[i + 1].t_ms) {
        i++;
    }

    const key_t *a = &k_keys[i];
    const key_t *b = &k_keys[i + 1];
    uint32_t span = (b->t_ms > a->t_ms) ? (b->t_ms - a->t_ms) : 1u;
    uint32_t k = (t > a->t_ms) ? (t - a->t_ms) : 0u;
    if (k > span) {
        k = span;
    }

    int32_t dt = (int32_t)b->rpm - (int32_t)a->rpm;
    int32_t dc = (int32_t)b->clt - (int32_t)a->clt;

    *rpm = (uint16_t)((int32_t)a->rpm + dt * (int32_t)k / (int32_t)span);
    *coolant = (int8_t)((int32_t)a->clt + dc * (int32_t)k / (int32_t)span);
}
