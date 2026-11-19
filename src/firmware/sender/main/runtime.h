#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * 发动机运行计时。
 *
 * 这是"车速不可用"之后的替代指标：本车 ECU 不给车速，就无法积分里程，
 * 而里程原本是保养提醒的依据。运行小时数其实是更准的保养基准——机油
 * 劣化看的是运转时间与热循环，不是走了多远。很多工程机械和商用车的
 * 保养周期就是按小时表定的。
 *
 * 只在总线活着（= 发动机/ECU 上电）时累加，所以它天然等价于"点火后
 * 的累计时长"，熄火停放不会空转。
 */

void     runtime_init(void);

/* bus_alive 为真时累加 dt_ms。dt_ms 超过 5 秒按 5 秒算，
 * 防止某次阻塞导致一次加进几分钟。 */
void     runtime_tick(bool bus_alive, uint32_t dt_ms);

/* 累计分钟数，饱和于 65535 */
uint16_t runtime_get_minutes(void);

/* 强制落盘。进深睡前调用 */
void     runtime_commit(void);
