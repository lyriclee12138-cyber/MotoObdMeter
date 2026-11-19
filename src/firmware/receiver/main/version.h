#pragma once

/* 构建版本信息。
 *
 * 数据来自自动生成的 version_env.h（由 tools/win/pack_firmware.sh 每次出包时重写），
 * 所以不要手改 version_env.h —— 改了下一次出包就被覆盖。
 *
 * 版本串形如： v2026.11.02-1215
 *                       ^^^^^ 编译时刻（到分钟），比它新就一定是最新固件。
 * 不需要额外后缀：时间戳本身就能判断新旧。 */

const char *version_string(void);      /* 完整版本串 */
const char *version_build_time(void);  /* "2026-11-02 12:15" */
