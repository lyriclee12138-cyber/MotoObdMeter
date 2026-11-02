#pragma once

/* 构建版本信息。
 *
 * 和接收端同一个套路：数据来自自动生成的 version_env.h（由
 * tools/win/pack_firmware.sh 每次出包时重写），不要手改。
 *
 * 发送端没有屏幕，版本号只能从串口日志看 —— 所以启动第一行一定要打出来，
 * 否则"这块板子跑的是哪一版"就只能靠猜。 */

const char *version_string(void);      /* 形如 v2026.11.02-153012 */
const char *version_build_time(void);  /* "2026-11-02 15:30" */
