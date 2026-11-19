#include "obd_poll.h"

#include <string.h>
#include "can_link.h"
#include "decode.h"
#include "isotp.h"
#include "odo.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "obd";

/* ISO 15765-4 的固定地址。
 * 0x7DF 是功能寻址（所有 ECU 都听），但不少 ECU 直接忽略它，
 * 只答物理地址 0x7E0 —— 所以这里做成失败后自动切换，不写死。 */
#define OBD_REQ_FUNC   0x7DF
#define OBD_REQ_PHYS   0x7E0
#define OBD_RESP_BASE  0x7E8
#define OBD_RESP_END   0x7EF

#define REQ_TIMEOUT_MS   120   /* 请求到首帧的超时 */
#define MAX_FAIL_BEFORE_PHYS 3 /* 功能寻址连失几次后切物理寻址 */
#define MAX_FAIL_DISABLE     6 /* 某 PID 连失几次后彻底停轮询 */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

typedef struct {
    uint8_t  pid;
    uint16_t period_ms;
    uint32_t last_ms;
    uint8_t  fails;
    uint8_t  disabled;
} poll_task_t;

/* 排布原则：越影响骑行判断的越靠前、周期越短。
 *
 * 0x0D 车速不在表里：本车 PID 有效但恒返回 0（轮速信号直连仪表、
 * 不进 ECU）。读到 0 不能显示——显示 0 km/h 会让骑手误判为停车。
 *
 * 里程项用 pid = 0 占位，实际 PID 由 s_odo_pid 决定：先试标准 0x31，
 * 被否定响应后降级到非标准 0xA6，两者都不支持就关掉这一项。 */
static poll_task_t s_tasks[] = {
    { OBD_PID_RPM,      40,  0, 0, 0 },   /* 25 Hz 上限，实际受总线限制 */
    { OBD_PID_COOLANT,  1000, 0, 0, 0 },  /* 1 Hz，温度变化很慢 */
    { OBD_PID_TPS,      200, 0, 0, 0 },
    { OBD_PID_VOLT,     1000, 0, 0, 0 },
    { OBD_PID_IAT,      2000, 0, 0, 0 },
    { OBD_PID_FUEL,     2000, 0, 0, 0 },
    { 0,                5000, 0, 0, 0 },  /* 里程（动态 PID） */
};
static const size_t s_task_count = sizeof(s_tasks) / sizeof(s_tasks[0]);

/* 里程数据源，按可靠性从高到低尝试 */
static uint8_t  s_odo_pid = OBD_PID_DIST_CLEAR;

static bool     s_use_phys;
static uint8_t  s_func_fails;
static isotp_rx_t s_isotp;

void obd_poll_init(void)
{
    isotp_reset(&s_isotp);
    s_use_phys = false;
    s_func_fails = 0;
    s_odo_pid = OBD_PID_DIST_CLEAR;
    for (size_t i = 0; i < s_task_count; i++) {
        s_tasks[i].last_ms = 0;
        s_tasks[i].fails = 0;
        s_tasks[i].disabled = 0;
    }
    ESP_LOGI(TAG, "poller ready, %u tasks", (unsigned)s_task_count);
}

/* 选出最"该发"的那一项：超期比例最大的。
 * 这样慢项不会永远被快项饿死。 */
static poll_task_t *pick_next(uint32_t now)
{
    poll_task_t *best = NULL;
    uint64_t best_score = 0;

    for (size_t i = 0; i < s_task_count; i++) {
        poll_task_t *t = &s_tasks[i];
        if (t->disabled) {
            continue;
        }
        uint32_t elapsed = now - t->last_ms;
        if (elapsed < t->period_ms) {
            continue;
        }
        /* 用 64 位算，避免长时间运行后 elapsed×1000 溢出把排序搞乱 */
        uint64_t score = ((uint64_t)elapsed * 1000u) / t->period_ms;
        if (score > best_score) {
            best_score = score;
            best = t;
        }
    }
    return best;
}

/* 等一帧属于我们的响应。返回 1 = 拿到有效数据，0 = 超时/无关帧，-1 = 否定响应 */
static int wait_response(uint8_t pid, uint8_t *out, uint8_t *out_len)
{
    uint32_t start = now_ms();

    while ((now_ms() - start) < REQ_TIMEOUT_MS) {
        can_frame_t f;
        int r = can_link_recv(&f, 30);
        if (r <= 0) {
            continue;
        }
        if (f.id < OBD_RESP_BASE || f.id > OBD_RESP_END) {
            continue;   /* 不是诊断响应，跳过 */
        }

        int rc = isotp_feed(&s_isotp, (uint16_t)f.id, f.data, f.dlc);

        if (rc == 0) {
            /* 首帧刚收到，回一个流控让 ECU 继续发。
             * 注意目标地址是 0x7E0（ECU 的接收地址），不是发来数据的 0x7E8
             * ——ECU 收帧和自己发帧用的是两个不同 ID，这里搞错会静默失败。 */
            if ((f.data[0] >> 4) == 0x1) {
                uint8_t fc[8] = { 0 };
                isotp_flow_control(fc);
                can_link_send(OBD_REQ_PHYS, fc, 8);
            }
            continue;
        }
        if (rc < 0) {
            ESP_LOGD(TAG, "isotp error on id 0x%lX", (unsigned long)f.id);
            continue;
        }

        /* 完整报文到手 */
        uint8_t *p = s_isotp.buf;
        uint16_t len = s_isotp.total;

        if (len < 2) {
            return 0;
        }
        if (p[0] == 0x7F) {
            /* 否定响应：7F <pid> <nrc>。0x12 = 不支持该 PID */
            return -1;
        }
        if (len < 3 || p[0] != 0x41 || p[1] != pid) {
            return 0;   /* 别的 PID 的迟到响应，丢弃 */
        }

        uint16_t dlen = (uint16_t)(len - 2);
        if (dlen > 8) {
            dlen = 8;
        }
        memcpy(out, &p[2], dlen);
        *out_len = (uint8_t)dlen;
        return 1;
    }
    return 0;
}

static void store_value(telemetry_t *t, uint8_t pid, float v)
{
    switch (pid) {
    case OBD_PID_RPM: {
        int r = (int)(v + 0.5f);
        if (r < 0) r = 0;
        if (r > 20000) r = 20000;
        t->rpm = (uint16_t)r;
        t->flags |= TF_RPM;
        break;
    }
    case OBD_PID_COOLANT:
        t->coolant = (int8_t)v;
        t->flags |= TF_COOLANT;
        break;
    case OBD_PID_IAT:
        t->intake = (int8_t)v;
        t->flags |= TF_IAT;
        break;
    case OBD_PID_TPS: {
        int p = (int)(v + 0.5f);
        if (p < 0) p = 0;
        if (p > 100) p = 100;
        t->tps = (uint8_t)p;
        t->flags |= TF_TPS;
        break;
    }
    case OBD_PID_FUEL: {
        int p = (int)(v + 0.5f);
        if (p < 0) p = 0;
        if (p > 100) p = 100;
        t->fuel = (uint8_t)p;
        t->flags |= TF_FUEL;
        break;
    }
    case OBD_PID_VOLT: {
        int dv = (int)(v * 10.0f + 0.5f);
        if (dv < 0) dv = 0;
        if (dv > 65535) dv = 65535;
        t->volt_dv = (uint16_t)dv;
        t->flags |= TF_VOLT;
        break;
    }
    default:
        break;
    }
}

/* 里程单独处理：它要过一层清码保护，不能直接写进 t。
 * v 的单位是 km（obd_decode 已经把 0xA6 的 0.1km 归一成 km）。 */
static void handle_odo(telemetry_t *t, float v)
{
    if (v <= 0.0f || v > 65535.0f) {
        /* 0 表示 ECU 刚清过码或计数器未初始化，喂进去只会污染 offset */
        return;
    }
    t->odo_dam = odo_feed_km((uint16_t)(v + 0.5f));
    t->flags |= TF_ODO;
}

void obd_poll_tick(telemetry_t *t)
{
    if (t == NULL) {
        return;
    }

    uint32_t now = now_ms();
    poll_task_t *task = pick_next(now);
    if (task == NULL) {
        return;
    }
    task->last_ms = now;

    /* pid 为 0 的任务是占位，实际 PID 取自里程源状态机 */
    const uint8_t pid = task->pid ? task->pid : s_odo_pid;

    /* 请求帧：02 01 <pid> 00 00 00 00 00 */
    uint8_t req[8] = { 0x02, 0x01, pid, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint32_t req_id = s_use_phys ? OBD_REQ_PHYS : OBD_REQ_FUNC;

    esp_err_t err = can_link_send(req_id, req, 8);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tx failed for pid 0x%02X: %s", pid, esp_err_to_name(err));
        return;
    }

    uint8_t data[8];
    uint8_t dlen = 0;
    int r = wait_response(pid, data, &dlen);

    if (r == 1) {
        task->fails = 0;
        float v = 0.0f;
        if (obd_decode(pid, data, dlen, &v) == 0) {
            if (pid == OBD_PID_DIST_CLEAR || pid == OBD_PID_ODOMETER) {
                handle_odo(t, v);
            } else {
                store_value(t, pid, v);
            }
        }
        return;
    }

    if (r < 0) {
        /* ECU 明确说不支持，别再浪费总线时间 */
        ESP_LOGI(TAG, "PID 0x%02X not supported by ECU", pid);

        /* 里程源降级：0x31 是标准 PID，理论上 EOBD 车必须有；
         * 万一没有，退到非标准 0xA6 碰碰运气。两者都不行就放弃里程。 */
        if (pid == OBD_PID_DIST_CLEAR) {
            s_odo_pid = OBD_PID_ODOMETER;
            task->fails = 0;
            task->disabled = 0;
            ESP_LOGI(TAG, "改试非标准 0xA6 作为里程源");
            return;
        }

        task->disabled = 1;
        if (pid == OBD_PID_FUEL) {
            t->fuel = FUEL_INVALID;
        }
        return;
    }

    /* 超时：先怀疑寻址方式，再怀疑这个 PID */
    task->fails++;
    if (!s_use_phys && ++s_func_fails >= MAX_FAIL_BEFORE_PHYS) {
        s_use_phys = true;
        s_func_fails = 0;
        ESP_LOGW(TAG, "功能寻址无应答，切换到物理寻址 0x%03X", OBD_REQ_PHYS);
        for (size_t i = 0; i < s_task_count; i++) {
            s_tasks[i].fails = 0;
        }
    } else if (task->fails >= MAX_FAIL_DISABLE) {
        ESP_LOGW(TAG, "PID 0x%02X 连续 %u 次超时，停止轮询", pid, task->fails);
        task->disabled = 1;
    }
}

bool obd_poll_bus_alive(void)
{
    uint32_t last = can_link_last_rx_ms();
    if (last == 0) {
        return false;
    }
    return (now_ms() - last) < 5000u;
}
