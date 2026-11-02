#include "decode.h"
#include <stddef.h>

/* 每个 PID 的公式都来自 SAE J1979。这里只做算术，不做范围裁剪。 */

uint8_t obd_pid_bytes(uint8_t pid)
{
    switch (pid) {
    case OBD_PID_LOAD:       return 1;
    case OBD_PID_COOLANT:    return 1;
    case OBD_PID_MAP:        return 1;
    case OBD_PID_RPM:        return 2;
    case OBD_PID_SPEED:      return 1;
    case OBD_PID_TIMING:     return 1;
    case OBD_PID_IAT:        return 1;
    case OBD_PID_MAF:        return 2;
    case OBD_PID_TPS:        return 1;
    case OBD_PID_RUN_SEC:    return 2;
    case OBD_PID_FUEL:       return 1;
    case OBD_PID_DIST_CLEAR: return 2;
    case OBD_PID_BARO:       return 1;
    case OBD_PID_VOLT:       return 2;
    case OBD_PID_OILTEMP:    return 1;
    case OBD_PID_ODOMETER:   return 4;
    default:                 return 0;
    }
}

int obd_decode(uint8_t pid, const uint8_t *d, uint8_t n, float *out)
{
    if (d == NULL || out == NULL) {
        return -1;
    }

    uint8_t need = obd_pid_bytes(pid);
    if (need == 0 || n < need) {
        return -1;
    }

    /* 双字节量才需要 AB。这个短路是必须的：只需要 1 字节的 PID 里
     * 如果无条件读 d[1]，当调用方只给了 1 个有效字节时就是越界读。 */
    uint16_t ab = 0;
    if (need >= 2) {
        ab = (uint16_t)(((uint16_t)d[0] << 8) | d[1]);
    }

    switch (pid) {
    case OBD_PID_LOAD:
    case OBD_PID_TPS:
    case OBD_PID_FUEL:
        /* A * 100 / 255 */
        *out = (float)d[0] * 100.0f / 255.0f;
        return 0;

    case OBD_PID_COOLANT:
    case OBD_PID_IAT:
    case OBD_PID_OILTEMP:
        /* A - 40 */
        *out = (float)d[0] - 40.0f;
        return 0;

    case OBD_PID_MAP:
    case OBD_PID_BARO:
        /* A，单位 kPa */
        *out = (float)d[0];
        return 0;

    case OBD_PID_RPM:
        /* ((A * 256) + B) / 4 */
        *out = (float)ab / 4.0f;
        return 0;

    case OBD_PID_SPEED:
        *out = (float)d[0];
        return 0;

    case OBD_PID_TIMING:
        /* A / 2 - 64，范围 -64 ~ +63.5°，负值表示点火滞后 */
        *out = (float)d[0] / 2.0f - 64.0f;
        return 0;

    case OBD_PID_MAF:
        *out = (float)ab / 100.0f;
        return 0;

    case OBD_PID_RUN_SEC:
        /* ECU 本次上电以来的秒数。注意它**不是累计运行时长**，
         * 每次上电归零，所以只能用做交叉校验。 */
        *out = (float)ab;
        return 0;

    case OBD_PID_DIST_CLEAR:
        /* (A * 256) + B，单位 km，上限 65535 km 后回绕。
         * 值来自 ECU 内部里程计数器，不受本车车速 PID 恒 0 影响。 */
        *out = (float)ab;
        return 0;

    case OBD_PID_VOLT:
        *out = (float)ab / 1000.0f;
        return 0;

    case OBD_PID_ODOMETER: {
        /* 非标准。多数实现是 4 字节，单位 0.1km。
         * 也有厂商用 2 字节，此时 n == 2 会走不到这里（need 是 4），
         * 所以调用方若发现长度只有 2，应单独处理。 */
        uint32_t raw = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
                       ((uint32_t)d[2] << 8)  |  (uint32_t)d[3];
        *out = (float)raw / 10.0f;
        return 0;
    }

    default:
        return -1;
    }
}
