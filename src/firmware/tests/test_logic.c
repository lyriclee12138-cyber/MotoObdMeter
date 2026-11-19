/*
 * 纯逻辑层的主机端单元测试。
 * decode.c 与 isotp.c 不依赖 ESP-IDF，所以可以直接在 PC 上跑：
 *
 *   gcc -std=c11 -Wall -Wextra -I ../sender/main test_logic.c \
 *       ../sender/main/decode.c ../sender/main/isotp.c -o /tmp/t && /tmp/t
 *
 * 这两个文件是"错了也不会报错、只会显示错数字"的那一类代码，
 * 必须在拿到硬件之前就验证掉。
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "decode.h"
#include "isotp.h"

static int g_pass;
static int g_fail;

static void check(int cond, const char *what)
{
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL: %s\n", what);
    }
}

static void check_near(float got, float want, const char *what)
{
    if (fabsf(got - want) < 0.01f) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL: %s  got=%.3f want=%.3f\n", what, (double)got, (double)want);
    }
}

static void test_decode(void)
{
    printf("[decode]\n");
    float v = 0;

    /* 常用的那条例子：41 0C 1A F8 → 1726 rpm */
    uint8_t rpm[] = { 0x1A, 0xF8 };
    check(obd_decode(0x0C, rpm, 2, &v) == 0, "rpm decode ok");
    check_near(v, 1726.0f, "rpm value");

    uint8_t spd[] = { 0x3C };
    check(obd_decode(0x0D, spd, 1, &v) == 0, "speed decode ok");
    check_near(v, 60.0f, "speed 60km/h");

    uint8_t clt[] = { 0x7B };            /* 123 - 40 = 83 */
    check(obd_decode(0x05, clt, 1, &v) == 0, "coolant decode ok");
    check_near(v, 83.0f, "coolant 83C");

    uint8_t iat[] = { 0x00 };            /* 0 - 40 = -40 */
    check(obd_decode(0x0F, iat, 1, &v) == 0, "iat decode ok");
    check_near(v, -40.0f, "iat -40C");

    uint8_t tps[] = { 0x80 };
    check(obd_decode(0x11, tps, 1, &v) == 0, "tps decode ok");
    check_near(v, 50.196f, "tps 128/255");

    uint8_t volt[] = { 0x36, 0xB0 };     /* 14000 mV */
    check(obd_decode(0x42, volt, 2, &v) == 0, "volt decode ok");
    check_near(v, 14.0f, "volt 14V");

    /* 0x001E24F0 = 1975536 → 197553.6 km */
    uint8_t odo[] = { 0x00, 0x1E, 0x24, 0xF0 };
    check(obd_decode(0xA6, odo, 4, &v) == 0, "odometer decode ok");
    check_near(v, 197553.6f, "odometer value");

    /* 里程主源 0x31：0x2D3E = 11582 km，取自 2026-10-31 实车读数 */
    uint8_t dist[] = { 0x2D, 0x3E };
    check(obd_decode(0x31, dist, 2, &v) == 0, "dist_clear decode ok");
    check_near(v, 11582.0f, "dist_clear 11582km");
    check(obd_decode(0x31, dist, 1, &v) == -1, "dist_clear with 1 byte must fail");

    /* 0x1F ECU 本次上电秒数：0x008A = 138 s */
    uint8_t runsec[] = { 0x00, 0x8A };
    check(obd_decode(0x1F, runsec, 2, &v) == 0, "run_sec decode ok");
    check_near(v, 138.0f, "run_sec 138s");

    /* 0x0E 点火提前角：0x94 = 148 → 148/2-64 = 10° */
    uint8_t timing[] = { 0x94 };
    check(obd_decode(0x0E, timing, 1, &v) == 0, "timing decode ok");
    check_near(v, 10.0f, "timing 10deg");
    /* 负提前角（点火滞后）0x00 → -64 */
    uint8_t timing_neg[] = { 0x00 };
    check(obd_decode(0x0E, timing_neg, 1, &v) == 0, "timing neg decode ok");
    check_near(v, -64.0f, "timing -64deg");

    /* 0x0B 进气歧管压力 44 kPa、0x33 大气压 101 kPa */
    uint8_t map[] = { 0x2C };
    check(obd_decode(0x0B, map, 1, &v) == 0, "map decode ok");
    check_near(v, 44.0f, "map 44kPa");

    uint8_t baro[] = { 0x65 };
    check(obd_decode(0x33, baro, 1, &v) == 0, "baro decode ok");
    check_near(v, 101.0f, "baro 101kPa");

    /* 边界：字节数不够必须失败，不能算出半个数 */
    check(obd_decode(0x0C, rpm, 1, &v) == -1, "rpm with 1 byte must fail");
    check(obd_decode(0xA6, odo, 2, &v) == -1, "odometer with 2 bytes must fail");
    check(obd_decode(0x99, spd, 1, &v) == -1, "unknown pid must fail");

    check(obd_pid_bytes(0x0C) == 2, "rpm needs 2 bytes");
    check(obd_pid_bytes(0xA6) == 4, "odometer needs 4 bytes");
}

static void test_isotp_single(void)
{
    printf("[isotp single frame]\n");
    isotp_rx_t s;
    isotp_reset(&s);

    uint8_t f[8] = { 0x03, 0x41, 0x0D, 0x3C, 0x00, 0x00, 0x00, 0x00 };
    check(isotp_feed(&s, 0x7E8, f, 8) == 1, "SF returns complete");
    check(s.total == 3, "SF total len 3");
    check(s.buf[0] == 0x41 && s.buf[1] == 0x0D && s.buf[2] == 0x3C, "SF payload");

    /* 单帧长度超出 DLC 必须拒绝 */
    uint8_t bad[8] = { 0x07, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    check(isotp_feed(&s, 0x7E8, bad, 4) == -1, "SF len > dlc must fail");
}

static void test_isotp_multi(void)
{
    printf("[isotp multi frame]\n");
    isotp_rx_t s;
    isotp_reset(&s);

    /* 总长 20 字节：首帧带 6，然后两个连续帧各 7 */
    uint8_t ff[8] = { 0x10, 0x14, 1, 2, 3, 4, 5, 6 };
    check(isotp_feed(&s, 0x7E8, ff, 8) == 0, "FF returns not-complete");
    check(s.total == 20, "FF total 20");
    check(s.got == 6, "FF carried 6 bytes");

    uint8_t cf1[8] = { 0x21, 7, 8, 9, 10, 11, 12, 13 };
    check(isotp_feed(&s, 0x7E8, cf1, 8) == 0, "CF1 not complete yet");
    check(s.got == 13, "after CF1 got 13");

    uint8_t cf2[8] = { 0x22, 14, 15, 16, 17, 18, 19, 20 };
    check(isotp_feed(&s, 0x7E8, cf2, 8) == 1, "CF2 completes message");
    check(s.total == 20 && s.got == 20, "final length 20");
    check(s.buf[0] == 1 && s.buf[19] == 20, "payload head/tail correct");

    /* 序号跳变：说明丢了连续帧，必须整条作废而不是拼出错数据 */
    isotp_reset(&s);
    check(isotp_feed(&s, 0x7E8, ff, 8) == 0, "FF again");
    uint8_t cf_bad[8] = { 0x23, 7, 8, 9, 10, 11, 12, 13 };
    check(isotp_feed(&s, 0x7E8, cf_bad, 8) == -1, "wrong SN must fail");

    /* 超长报文直接拒收，不能溢出静态缓冲 */
    isotp_reset(&s);
    uint8_t ff_big[8] = { 0x10, 0xC8, 1, 2, 3, 4, 5, 6 };  /* 200 字节 */
    check(isotp_feed(&s, 0x7E8, ff_big, 8) == -1, "oversized FF must fail");

    /* 流控帧 */
    uint8_t fc[8] = { 0 };
    check(isotp_flow_control(fc) == 3, "FC length 3");
    check(fc[0] == 0x30, "FC byte0 0x30 (CTS, BS=0)");
    check(fc[1] == 0x00 && fc[2] == 0x00, "FC BS/STmin zero");
}

int main(void)
{
    test_decode();
    test_isotp_single();
    test_isotp_multi();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
