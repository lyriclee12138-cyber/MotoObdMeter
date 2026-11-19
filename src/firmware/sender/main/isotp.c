#include "isotp.h"
#include <string.h>

#define PCI_SF 0x0u
#define PCI_FF 0x1u
#define PCI_CF 0x2u
#define PCI_FC 0x3u

void isotp_reset(isotp_rx_t *s)
{
    if (s == 0) {
        return;
    }
    memset(s, 0, sizeof(*s));
}

static int copy_in(isotp_rx_t *s, const uint8_t *src, uint16_t n)
{
    if (s->got + n > s->total) {
        n = (uint16_t)(s->total - s->got);   /* 末尾可能有多余填充字节 */
    }
    if (s->got + n > ISOTP_MAX_PAYLOAD) {
        s->overflow = 1;
        s->active = 0;
        return -1;
    }
    memcpy(&s->buf[s->got], src, n);
    s->got = (uint16_t)(s->got + n);
    return 0;
}

int isotp_feed(isotp_rx_t *s, uint16_t id, const uint8_t *d, uint8_t dlc)
{
    if (s == 0 || d == 0 || dlc == 0) {
        return 0;
    }

    uint8_t pci = (uint8_t)(d[0] >> 4);

    switch (pci) {
    case PCI_SF: {
        uint8_t len = (uint8_t)(d[0] & 0x0Fu);
        if (len == 0 || len > 7 || (uint8_t)(len + 1) > dlc) {
            return -1;
        }
        s->active = 0;
        s->overflow = 0;
        s->id = id;
        s->total = len;
        s->got = len;
        memcpy(s->buf, &d[1], len);
        return 1;
    }

    case PCI_FF: {
        if (dlc < 8) {
            return -1;
        }
        uint16_t total = (uint16_t)(((uint16_t)(d[0] & 0x0Fu) << 8) | d[1]);
        if (total <= 7u) {
            return -1;                       /* 畸形：应该用单帧 */
        }
        s->id = id;
        s->total = total;
        s->got = 0;
        s->next_sn = 1;
        s->overflow = 0;
        s->active = 1;
        if (total > ISOTP_MAX_PAYLOAD) {
            s->overflow = 1;
            s->active = 0;
            return -1;
        }
        /* 首帧带 6 个数据字节 */
        if (copy_in(s, &d[2], 6) != 0) {
            return -1;
        }
        return 0;
    }

    case PCI_CF: {
        if (!s->active) {
            return 0;                        /* 没有在装配，忽略 */
        }
        if (id != s->id) {
            return 0;
        }
        uint8_t sn = (uint8_t)(d[0] & 0x0Fu);
        if (sn != s->next_sn) {
            /* 序号错乱说明丢帧，整条作废 */
            s->active = 0;
            return -1;
        }
        s->next_sn = (uint8_t)((s->next_sn + 1) & 0x0Fu);
        if (copy_in(s, &d[1], 7) != 0) {
            return -1;
        }
        if (s->got >= s->total) {
            s->active = 0;
            return 1;
        }
        return 0;
    }

    case PCI_FC:
    default:
        return 0;
    }
}

int isotp_flow_control(uint8_t *out)
{
    if (out == 0) {
        return 0;
    }
    out[0] = 0x30;   /* 0x3 = 流控帧，低 4 位 0 = Continue To Send */
    out[1] = 0x00;   /* 块大小 0 = 后续不再发流控，让 ECU 连续发完 */
    out[2] = 0x00;   /* STmin 0 = 连续帧之间无需额外间隔 */
    return 3;        /* 注意：FC 帧只带 3 个有效字节，其余由驱动补 0 */
}
