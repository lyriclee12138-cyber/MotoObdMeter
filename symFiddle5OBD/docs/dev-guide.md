# 开发文档

> 项目：SYM Fiddle 5 踏板车用的圆屏仪表（CAN/OBD → ESP-NOW → 圆屏）
> 当前版本：**v0.1-alpha**（收发两端上机验证通过，待实车测试）
> 这份文档是唯一入口。变更历史看 `CHANGELOG.md`，供电方案看 `docs/power-plan.md`（手机版在 `docs/wiring/index.html` 的「供电方案」页签）。

---

## 1. 它是什么

两块 ESP32-S3：

```
车上 OBD 口 ──CAN(500k)──> 发送端(开发板, 座桶内) ──ESP-NOW 2.4G──> 接收端(圆屏, 车把)
                              │                                        │
                              └── 每 500ms 回心跳 ────────────────────┘  （判在线/不在线）
```

- **发送端**：轮询 OBD 拿转速/水温，解码后 20Hz 广播；收心跳判断圆屏在不在。
- **接收端**：466×466 圆屏，显示转速弧 + 大数字 + 水温，另有设置页。

### 硬件

| 部件 | 型号 | 备注 |
|---|---|---|
| 发送端 | ESP32-S3 N16R8 开发板（双 USB-C，16MB Flash / 8MB PSRAM） | **PSRAM 已关**，见坑 #2 |
| 接收端 | 微雪 ESP32-S3-Touch-AMOLED-1.75C（466×466，CO5300 QSPI，32MB Flash） | |
| CAN 收发器 | SN65HVD230 模块（**必须 3.3V 版**，5V 会烧） | |
| OBD 接头 | 白色 16 针公头带尾线 | 针号需万用表核对，线色不标准 |
| 供电 | 现在：充电宝；装车：9–36V→5V 模块 | 见 `power-plan.md` |

---

## 2. 目录结构

```
symFiddle5OBD/
├── CHANGELOG.md               版本记录（用户能看见的变化 + 结构性坑）
├── docs/
│   ├── power-plan.md          装车供电方案
│   ├── power/index.html       供电方案源文件（已合并进 wiring/index.html）
│   ├── sender-wiring.md       接线表（模块↔开发板↔OBD）
│   ├── UI规格-beta.html       界面规格锁定表
│   └── gauge-ui-beta.html     界面基线（可交互，LOCKED）
├── firmware/
│   ├── common_components/shared/   telemetry.h（23 字节契约）、heartbeat.h
│   ├── sender/                发送端（ESP-IDF）
│   └── receiver/              接收端（ESP-IDF + LVGL）
├── tools/
│   ├── gauge_art_v2.py        渲染仪表背景图（彩环烘焙在这里）
│   ├── png2c.py               位图 → LVGL RGB565 C 数组
│   ├── gen_fonts.sh           中文字体子集（改了文案必须重跑）
│   └── win/
│       ├── idf.bat            ★ 构建入口，必须用它
│       ├── pack_firmware.sh   ★ 出包（编译 + 合并 + 校验 + 同步共享盘）
│       ├── bench.sh           编译+烧录+测帧率
│       └── log.py / bootlog.py 抓串口
└── build/                     渲染产物（不进 git）
```

---

## 3. 环境

| 项 | 位置 / 值 |
|---|---|
| ESP-IDF | `C:/esp/v5.5.5/esp-idf`，工具在 `C:/Espressif/tools` |
| 构建入口 | `tools/win/idf.bat`（**必须**：它清掉 `MSYSTEM`，否则 idf.py 静默退出 0） |
| Python | 系统 python3（脚本用 PIL + numpy） |
| 字体工具 | `npx lv_font_conv`（`tools/gen_fonts.sh` 里已写好参数） |

> 换机器要重建时：装 ESP-IDF v5.5.x（xtensa-esp-elf + esp-rom-elfs 就够，不用全家桶），
> 然后 `python -m pip install pillow numpy`。**不要**用 MSYS 的 cmake/ninja，用 IDF 自带的。

---

## 4. 构建 / 出包 / 烧录

### 4.1 一条命令出包

```bash
cd symFiddle5OBD
./tools/win/pack_firmware.sh            # 两端都出
./tools/win/pack_firmware.sh receiver    # 只出圆屏（更快）
./tools/win/pack_firmware.sh sender      # 只出发送端
```

脚本会：写版本头 → 编译 → `merge-bin` 合成 full → **校验 full 里的应用区等于 app** → 复制到 `firmware_out/` → 同步到共享盘。

### 4.2 版本号机制（重要）

`pack_firmware.sh` 每次出包**重写** `main/version_env.h`，里面是编译时刻：

```
v2026.11.03-144354     ← 精确到秒，避免同分钟撞车
```

内容每次不同 → `version.c` 必然重编 → 固件里的版本串必然更新。
**这是判断"烧进去的是不是新固件"的唯一可靠依据**（手机烧录工具不发硬复位，屏不重启是正常的）。

- 接收端：设置页滑到底，「版本」卡片
- 发送端：串口首行

### 4.3 烧录参数

| | Flash | 模式 | 频率 | full 地址 | app 地址 |
|---|---|---|---|---|---|
| 接收端 | **32MB** | DIO | 80MHz | 0x0 | 0x10000 |
| 发送端 | **16MB** | DIO | 80MHz | 0x0 | 0x10000 |

**两端 Flash 容量不同，参数不能混用。**

---

## 5. 架构要点

### 5.1 数据流

```
obd_poll.c  ── 发 0x0C(转速)/0x05(水温) 请求
    │
isotp.c     ── ISO-TP 单帧/多帧拆包
    │
decode.c    ── 原始字节 → rpm = (A*256+B)/4，clt = A-40
    │
uplink.c    ── 组 telemetry_t（magic + ver + seq + flags + rpm + clt），20Hz 广播
    │
ESP-NOW ────────────────> link.c（接收端）—— 校验 magic/ver，500ms 内算在线
    │
datasrc.c  ── 演示模式开？本地假数据 ： 真实数据
    │
ui_gauge.c ── 平滑 + 增量重绘
```

### 5.2 接收端任务模型

| 任务 | 干什么 | 优先级 |
|---|---|---|
| `app_main` 主循环 | 读数据 → 拿 LVGL 锁 → 更新 UI → 等一帧 | 1 |
| LVGL 任务（`esp_lv_adapter` 建） | `lv_timer_handler`：渲染 + flush | 6 |

LVGL 不是线程安全的，**所有 UI 操作必须在 `esp_lv_adapter_lock()` 里**。
`perf.c` 的二进制信号量用来"等一帧"，避免主循环推得比屏幕刷新快、同一片像素反复画。

### 5.3 显示层怎么分工（改外观前必读）

| 元素 | 谁画 |
|---|---|
| 底纹、刻度、刻度数字、**整条绿→黄→红彩环** | `tools/gauge_art_v2.py` 离线烘焙成 466×466 RGB565 |
| 未填充部分 | 运行时用轨道底色画一条弧"擦掉" |
| 针尖圆头 | 运行时一个圆（`lv_draw_rect` + `LV_RADIUS_CIRCLE`） |
| 转速数字、水温 | LVGL label（专用字体 `font_num_112` / `font_num_40`） |
| 设置页全部 | 纯 LVGL 控件 |

**为什么颜色要烘焙**：弧是脏区增量重绘的。颜色若在运行时算，任何一次重绘时机错位
都会把颜色留在错误位置，而且**永远不会自愈**。烘焙后颜色是像素自带的，最坏只是"弧长/短一点"。

---

## 6. 开发约定

1. **大改动必须提交 git**，message 写清"什么现象 / 为什么这么改"。
2. **改素材要同步三处几何**（这是目前最容易出错的地方）：

   | 位置 | 变量 |
   |---|---|
   | `tools/gauge_art_v2.py` | `R_TRACK/TRACK_W`、`R_ARC/ARC_W` |
   | `main/ui_gauge.c` | `ARC_R_OUT/ARC_W`、`ERASE_R_OUT/ERASE_W` |
   | `main/ui_theme.c` | `track` 颜色（必须等于渲染脚本的 `'track'`） |

   当前值：彩环 194/11（183..194），轨道与擦除弧 195/13（182..195）。

3. **改了中文文案 → 必须重跑 `./tools/gen_fonts.sh`**，否则新字是空白。
   字体是子集，只包含脚本里列出的字。

4. **改素材流程**：
   ```bash
   python tools/gauge_art_v2.py     # 渲染到 build/v2/
   python tools/png2c.py            # 转 C 数组到 build/assets/
   cp build/assets/gauge_assets.* firmware/receiver/main/assets/
   ./tools/win/pack_firmware.sh receiver
   ```

5. **协议改动**（`shared/telemetry.h`）必须两端同步改，否则接收端 version 校验拒收，
   现象是"收包数一直是 0"，从现象看不出是协议问题。

---

## 7. 已经确认的车辆事实（不要再重查）

- 诊断口：**16 针 OBD-II**，位置在座桶后、电池上方
- 协议：**ISO 15765-4 CAN，11 bit，500 kbaud**，ECU 名 `ECM-EngineControl`，自报 EOBD (Europe)
- **车速 PID 0x0D 有应答但恒返 0** —— 轮速直连仪表，不进 ECU，已放弃
- **里程用 PID 0x31**（清码后行驶距离），清码会归零 → `odo.c` 做 offset 补偿
- **0x2F 油量、0x42 模块电压不支持**（App 里那个"14V"是 ELM327 自测的）
- 可用 PID：0x04 / 05 / 06 / 07 / 0B / 0C / 0E / 0F / 10 / 11 / 14 / 1F / 21 / 31 / 33

---

## 8. 坑清单（都踩过，别再踩）

| # | 现象 | 根因 | 结论 |
|---|---|---|---|
| 1 | 发送端只出 bootloader 日志，卡在 `cpu_start: Multicore app` | 按 DEBUG 优化编译，卡在 CPU1 启动 | 改 `-O2`（PERF） |
| 2 | 修完 #1 立刻 panic 循环 | 崩在 `esp_psram_impl_enable` | 发送端不需要 PSRAM，`CONFIG_SPIRAM=n` |
| 3 | 黑夜模式翻页时四角发白 | `png2c.py` 把透明像素合成到**白色** | 按主题底色合成 |
| 4 | 转速弧颜色错乱（两次） | 运行时算色 + 增量重绘 = 错误颜色永久残留 | 彩环烘焙进背景图 |
| 5 | 弧中间断一截 | 脏区起点按"上一帧值"算，慢爬升时漂移，漂过那段从没画过 | 记 `s_painted`（屏幕上实际画到哪） |
| 6 | 0 转时留一个绿点 | 烘焙的起点圆头比擦除弧探出 1.7° | 两端都不烘焙圆头 |
| 7 | 未填充段边缘透色 | 擦除弧与彩环同尺寸，半像素错位擦不净 | 擦除弧宽 1px |
| 8 | `full.bin` 里装的是旧代码 | `merge-bin` 失败但脚本没检查 | 出包脚本校验 `full[0x10000:] == app` |
| 9 | 设置页滚不动 | 滚动容器被设成 540 高（比屏幕高），内部没溢出 | 容器保持 466×466，让卡片溢出它 |
| 10 | 主题按钮跑到卡片外 | `lv_obj_get_height()` 在布局前返回 0 | 用 `lv_obj_align` 相对定位 |
| 11 | LVGL 弧角度整体偏 180° | LVGL 是"0°在3点顺时针"，本设计是"0°在12点" | `a = θ - 90` |
| 12 | 编译偶发假死（日志十几分钟不动、gcc CPU 时间 0） | 两个 build 抢同一目录 / 残留进程 | 杀 ninja+gcc 重跑；不要并发两个 build |

---

## 9. 当前状态 / 下一步

**已完成并上机验证**：圆屏界面（日夜双主题）、设置页全部功能、ESP-NOW + 心跳、
发送端状态灯、BOOT 切模拟数据、发送端深睡（总线静默 60s）、CAN TWAI 500k。

**未验证**：实车 CAN 数据（转速/水温解码准不准）、车载 12V 供电。

**下一步顺序**：
1. 装车前先量 4 个数（见 `power-plan.md` 第 6 节）
2. 装车供电（推荐 OBD 16 脚常电）
3. 实车验证解码
4. 可选优化：收发器 Rs 待机（省 10mA）、圆屏加休眠

---

## 10. 速查命令

```bash
# 出包（最常用）
./tools/win/pack_firmware.sh receiver

# 只编译
./tools/win/idf.bat -B firmware/receiver/build -C firmware/receiver build

# 抓串口（发送端 CH340）
python tools/win/log.py                 # 看参数

# 重渲染仪表面板 + 转 C 数组
python tools/gauge_art_v2.py && python tools/png2c.py

# 重生成中文字体子集
./tools/gen_fonts.sh

# git
git log --oneline | head -20
git tag -n1
```
