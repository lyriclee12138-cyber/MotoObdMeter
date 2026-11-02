# 交接文档 · symFiddle5OBD

> 写给接手的 agent。读完这一份，你应该能直接接着干，不用重新摸索。
> 最后更新：2026-11-02 傍晚，状态：**圆屏端 v2 极简界面已跑通；发送端固件已出包、等上板联调**。
> **本项目由两个对话并行开发，动代码前先看 0.7 节的边界约定。**

---

## 0. 一句话说清

夏杏三阳飞度5 踏板摩托车的 OBD 数显系统：**开发板读 CAN 总线 → ESP-NOW → 圆屏显示指针转速表**。
现在**圆屏端已经能在真机上显示**（假数据驱动），**发送端代码写完但没上过板**（硬件还没到货）。


---

## 0.5 今日进度（2026-11-01 晚，换机当天）

**已完成并上真机验证：**

| 项 | 结果 |
|---|---|
| 帧率 | 4 fps → **42–59 fps**（抗锯齿开），详见第 6 节 |
| 黑夜模式 | 两套背景位图（`bg.png` / `bg_dark.png`），NVS 记住选择 |
| 设置页 | 与仪表页同屏两个页面对象，**横向滑动循环翻页**（左滑下一页从右进、右滑上一页从左进）|
| 电量 | 自己写的 AXP2101 最小驱动，显示 `100%` + 状态文案，1 秒刷新 |
| 屏幕亮度 | 设置页第四张卡片，自绘滑条，拖动实时调背光，松手存 NVS |

**已知限制（不是 bug，是硬件/框架约束）：**

- **翻页动画只有 4–5 帧**：整屏重绘 ~50 ms 是 LVGL 软件渲染在这颗 S3 上的地板价。
  搬 PSRAM、加大绘制缓冲、搬内部 RAM 全都试过，无效。要丝滑得绕过 LVGL 直接 DMA 刷屏。
- 发送端连接状态恒为「未连接」：ESP-NOW 还没接，没有伪造。（**2026-11-02 已接上，见 0.6**）

**下次继续（按优先级）：**
1. **P1 发送端上板**（硬件到货后）：焊接 SN65HVD230 → 只收不发看总线 → 发 `02 01 0C` 试 0x7DF/0x7E0
2. **P2 ESP-NOW 联通**：接收端替换 `fake_source_get()`，把 23 字节 `telemetry_t` 喂给 `ui_gauge_update()`
3. 可选：翻页动画丝滑版（预合成过渡帧 + `esp_lcd_panel_draw_bitmap` 直刷）

> 板子当前是拔掉的。本地最后一次改动（power.c 日志级别）**没烧进去**，
> 下次插上接 COM4 跑一次 `tools/win/bench.sh` 即可同步。

---

## 0.6 2026-11-02 下午：发送端开工 + 接收端接上 ESP-NOW

**代码全部写完并编译出包，还没上板**（用户硬件刚到手）。

| 项 | 结果 |
|---|---|
| 接线方案 | `docs/sender-wiring.md`（接线表、白色接头针号核实方法、电气红线、分阶段联调、故障对照）|
| 发送端固件 | 首次编译通过，0.74 MB；新增**上电嗅探 3 秒**、**BOOT 键模拟数据**、版本串、免深睡保护 |
| 发送端分区 | 新增 `partitions.csv`，factory 4MB（默认只有 1MB，装不下后续功能）|
| 接收端 | 新增 `link.c/h`：ESP-NOW 接收 + 协议校验 + 丢包统计；`main.c` 不再用假数据；设置页「发送端」显示真实连接状态 |
| 出包 | `pack_firmware.sh` 支持 `receiver|sender|both`，收发共用同一版本串，`firmware_out/` 出 4 个 bin + 烧录说明 |

**关键设计（别改错）：**

- ESP-NOW 信道两端都写死 **1**（`uplink.h` 的 `UPLINK_CHANNEL` / `link.h` 的 `LINK_CHANNEL`）。
  不同信道时的表现是"完全收不到"，很容易被误判成硬件坏。
- 接收端 **500ms 无包即判断开**：rpm 归 0、水温传 `INT8_MIN`（界面 `--`）。
  不允许拿上一包的旧值顶替 —— 骑手看的必须是当前转速。
- 发送端 **BOOT 键模拟数据是"按住生效、松手退出"**，不是复位时按（那时会进下载模式，程序根本不跑）。
- 发送端"从未见过总线"时深睡超时 **10 分钟**（台架调试），见过总线后仍 60 秒（装车省电瓶）。

**下一步**：按 `docs/sender-wiring.md` 第 5 节 A→B→C→D 四阶段联调，先桌面对联（不要车），
再上车只收不发，最后全链路。跑通后才做 12V 供电上车。

---

## 0.7 并行开发的边界（2026-11-02 设，**2026-11-03 已撤销**）

> **已回到单对话开发。** 用户发现发送端和接收端需要联调，分两个对话分别交代反而更麻烦，
> 另一个对话已停用。所有改动都在当前对话进行。
>
> 下面这段约定保留仅作历史记录，**不再生效**。唯一还有价值的经验是：
> 1. `shared/telemetry.h` 是两端契约，改字段必须同时改两端
> 2. `pack_firmware.sh` 默认 `both`，会同时编两端
> 3. COM 口全局只有一个，烧录要串行
>
> （原文如下）

### 原文：并行开发的边界

2026-11-02 起这个项目由两个对话并行推进。没有边界就会互相覆盖，已经踩过一次：
出包脚本被一边扩展成 both 模式，另一边的流程跟着变却不知道原因。

### 先决条件：git

仓库根有 git（2026-11-02 建基线），`build/`、`managed_components/`、产物和存档都已忽略。
**改动前后都跑一次 `git status`**，看到不认识的文件被改，先问再动。

### 文件归属

| 谁负责 | 路径 |
|---|---|
| 接收端（圆屏）侧 | `firmware/receiver/`、`firmware/common_components/shared/`、`tools/`、根级脚本与文档 |
| 发送端（开发板）侧 | `firmware/sender/main/`、`firmware/sender/CMakeLists.txt` |

### 硬规则

1. **协议改动只能由接收端侧发起。** `shared/telemetry.h` 是两端契约，加字段必须同步改两端。
   单边改会让接收端 version 校验拒收，现象是"收包数一直是 0"——从现象根本看不出是协议不一致，
   排查极费时间。
2. **不要动 `tools/`。** `pack_firmware.sh` 默认 `both`，会同时编译收发两端；
   一边在跑构建时另一边不要执行，会争同一个 build 目录。
3. **`sender/main/version_env.h` 是生成文件**，每次出包都被重写（内含编译时刻），不要手改。
4. **COM 口全局只有一个。** 烧录前确认另一边没在烧，否则双方都失败。
5. 提交时在 message 里写清动了哪一块，另一边 `git log` 能看见。

### 撞车了怎么办

- 同一文件两边都改了：以最后一次提交为准，另一方重新应用自己的改动
- 不确定某文件归谁：**默认不改，先问**

---

## 1. 硬件

| 部件 | 型号 | 状态 |
|---|---|---|
| 发送端 | ESP32-S3 N16R8 开发板（双 USB-C） | **已到货**，与 SN65HVD230、白色 16 针 OBD 公头一起 |
| 接收端 | 微雪 ESP32-S3-Touch-AMOLED-1.75C（466×466，CO5300 QSPI） | **已跑通** |
| CAN 收发器 | SN65HVD230 模块（**必须 3.3V 版**） | **已到货** |
| OBD 接头 | 白色 16 针公头带尾线 | **已到货**，针号需实测核对（颜色不标准）|
| 降压 | 9–36V → 5V 3A，静态电流 <1mA | 未采购（第一阶段用充电宝，不需要）|

已有 ELM327 蓝牙头（只用于侦察，不做成品——ESP32-S3 没有经典蓝牙）。

---

## 2. 已经确认的事实（**不要再重新查**）

- 诊断口是 **16 针 OBD-II**，位置在坐桶后、电池上方
- 协议 **ISO 15765-4 CAN，11 bit，500 kbaud**，ECU 名 `ECM-EngineControl`，自报 EOBD (Europe)
- **车速 0x0D 有应答但恒返 0** —— 轮速信号直连仪表，不进 ECU。已放弃，不做旁路
- **里程用 PID 0x31**（清码后行驶距离，实测 11582 km）。清码会归零，`odo.c` 做 offset 补偿
- **0x2F 油量、0x42 模块电压不支持**。App 上那个「OBD 模块电压 14V」是 ELM327 自测的
- 完整可用 PID：0x04 / 05 / 06 / 07 / 0B / 0C / 0E / 0F / 10 / 11 / 14 / 1F / 21 / 31 / 33

---

## 3. 目录结构

```
docs/
  可行性分析报告.html        链路、选型、BOM、风险、实车 PID 清单
  CAN方案需求清单.html       CAN 路线软硬件需求
  UI规格-beta.html           ★ 界面规格锁定表（八节，逐项参数）
  gauge-ui-beta.html         ★ 界面基线，可交互，LOCKED
  check-ui.py                界面一致性校验脚本
firmware/
  common_components/shared/  telemetry.h（v2，23 字节，收发端共享）
  sender/                    发送端（ESP-IDF 5.5）
  receiver/                  接收端（圆屏）★ 已跑通
  tests/                     主机端单测，50 项，`make` 直接跑
tools/
  gauge_art.py               按规格渲染背景/指针/轴心的位图
  preview.py                 合成四状态预览图，上板前目视验收
  png2c.py                   位图 → LVGL 图像 C 数组
  flash.sh                   一键烧录
build/                       渲染产物（bg/needle/hub .png + 四张 preview）
```

---

## 4. 换电脑后怎么把环境搭起来

> **2026-11-01 更新：已经搬到 Windows 机（LEE-HOME），环境装好了，本节 4.1–4.3 是历史记录，不用再做。**
>
> | 项 | 位置 |
> |---|---|
> | ESP-IDF v5.5.5（EIM 装的） | `C:/esp/v5.5.5/esp-idf`，工具在 `C:/Espressif/tools` |
> | 构建入口 | `tools/win/idf.bat`（**必须用它**：它负责清 `MSYSTEM`，否则 idf.py 静默退出 0） |
> | 一键编译+烧录+测帧率 | `tools/win/bench.sh` |
> | 读串口 | `tools/win/log.py` / `tools/win/bootlog.py`（板子固定 COM4） |
>
> 实测：首次全量 **8 分钟**、改一个 .c 增量 **23 秒**（旧机 i5-5200U 是 2 小时 / 1 小时）。
> 下面 4.1–4.3 的 Linux 踩坑（`~/esp`、`CODEBUDDY_BROKERED_FS_HOOK_ENABLED=0`、极狐镜像）**在本机都不适用**，保留仅供追溯。

**注意：路径 `~/esp` 是上一台机器的约定，新机器可以换，但改了要同步 `firmware/receiver/README.md`。**

### 4.1 装 ESP-IDF 5.5

国内网络下 GitHub 直连基本不可用，用**极狐镜像**（官方同步，速度差 20 倍）：

```bash
mkdir -p ~/esp && cd ~/esp
git clone --depth 1 -b release/v5.5 https://jihulab.com/esp-mirror/espressif/esp-idf.git
cd esp-idf
git submodule update --init --recursive --depth 1 --jobs 4
```

**如果 clone 报「检出成功但只有 3% 文件」**（沙箱或磁盘写入被拦导致），重建索引：

```bash
rm -f .git/index.lock
git read-tree HEAD
git checkout-index -a -f      # 约 6 分钟，写全 18612 个文件
```

### 4.2 装工具链（只装必需的）

```bash
export IDF_GITHUB_ASSETS="dl.espressif.cn/github_assets"   # 国内 CDN，2KB/s → 140KB/s
python3 tools/idf_tools.py install xtensa-esp-elf esp-rom-elfs
python3 tools/idf_tools.py install-python-env --no-constraints
```

> 原机器上 `install-python-env` 被环境的批量删除保护拦了，改成直接在 IDF 自建的 venv 里 pip 装依赖：
> `~/.espressif/python_env/idf5.5_py3.13_env/bin/pip install -r tools/requirements/requirements.core.txt cmake ninja`

**不需要装** gdb / openocd / riscv32-esp-elf / esp32ulp-elf（省 400 MB，烧录用 esptool 即可）。
**cmake 用 3.31 或更早**，4.x 与 IDF 5.5 有兼容问题。

### 4.3 环境变量（照抄，逐条都有原因）

```bash
export IDF_PATH="$HOME/esp/esp-idf"
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.13_env"
export IDF_GITHUB_ASSETS="dl.espressif.cn/github_assets"
export IDF_PYTHON_CHECK_CONSTRAINTS=0        # 官方 constraints 文件在 dl.espressif.com 上 502
export ESP_ROM_ELF_DIR="$HOME/.espressif/tools/esp-rom-elfs/20241011"
```

如果 WorkBuddy 环境里有文件访问钩子，**还必须**：

```bash
export CODEBUDDY_SAFE_DELETE_ENABLED=0
export CODEBUDDY_BROKERED_FS_HOOK_ENABLED=0
```

最后这条是上一台机器折腾最久的问题：那个钩子把「文件不存在」变成了 EPERM，gcc 的 `#include`
搜索链一旦收到 EPERM 就认为「文件存在但打不开」，直接放弃后续路径，于是满屏
`features.h: Operation not permitted`。**新机器如果没这个钩子就不用设**，遇到这个报错再考虑。

### 4.4 首次编译

```bash
cd firmware/receiver
idf.py -B build build
```

会从 components.espressif.com 自动下载 21 个依赖（约 273 MB），`dependencies.lock` 保证版本一致。
**打包里没有包含这个目录**，因为可以还原。

---

## 5. 接收端（重点，当前工作重心）

### 5.1 界面是位图，不是 LVGL 控件

beta-1 那套金属质感（环带、下沉槽、四组投影、径向渐变）用 LVGL 画不出来。所以反过来做：
**用 Pillow 按规格表参数渲染成真屏幕尺寸的图，再转 C 数组**，LVGL 只负责贴图和旋转。

| 图层 | 格式 | 尺寸 | 说明 |
|---|---|---|---|
| 背景 | RGB565 | 466×466 | 外圈、红区槽、色块、刻度、0–10 数字、水温标签，全部烘焙 |
| 指针 | **A8** | 360×360 | 只带 alpha，颜色靠 `image_recolor` 染 |
| 轴心 | ARGB8888 | 152×152 | 含投影，**最后创建**（贯穿感来源） |

> **A8 不是 L8**。L8 是灰度，0 表示**黑**不是透明，会画成一块旋转的黑方块——这是上板第一天踩的坑。

**改界面 = 改 `tools/gauge_art.py` 再重渲染**，不要动 C 代码：

```bash
python tools/gauge_art.py     # 渲染 bg / needle / hub
python tools/preview.py       # 合成四状态预览，先目视验收
python tools/png2c.py         # 转 C 数组
cp build/assets/gauge_assets.[ch] firmware/receiver/main/assets/
idf.py -B build build
```

改任何视觉参数前先读 `docs/UI规格-beta.html`，跑 `python3 docs/check-ui.py` 确认没偏离基线。

### 5.2 关键数值

```c
#define ANG_START  240      // 指针起点，8 点方向（0° = 12 点，顺时针）
#define ANG_SPAN   240      // 扫角，正下方留 120° 给水温
#define TEMP_CX    239      // 温度数字中心（屏坐标）
#define TEMP_CY    349
```

旋转：`deg10 = (240 + 240*rpm/10000) * 10`，LVGL 用 0.1° 为单位。

### 5.3 锁的坑（BSP 特有）

```c
// ✗ 错：0 表示「不等待、立即超时」，不是「永久等待」
if (bsp_display_lock(0) == ESP_OK) { ... }

// ✓ 对：直接调适配层，-1 才是无限等待
if (esp_lv_adapter_lock(-1) == ESP_OK) { ...; esp_lv_adapter_unlock(); }
```

`bsp_display_lock(uint32_t)` 是 `esp_lv_adapter_lock(int32_t)` 的透传，而 uint32_t 传不进 -1。
写错的话界面完全不创建，日志里是 `Failed to acquire LVGL lock`。

### 5.4 读串口日志（不做这步会以为板子死了）

ESP32-S3 的 USB-JTAG **必须 DTR=False、RTS=False** 才有输出：

```python
import serial
s = serial.Serial(); s.port = '/dev/ttyACM0'; s.baudrate = 115200
s.dtr = False; s.rts = False; s.open()
```

---

## 6. 帧率：已解决（4 fps → 42–59 fps，2026-11-01 换机后第一件事）

旧机器上的结论「瓶颈是 `lv_image_set_rotation` 对 360×360 的逐像素重采样」**只对了一半**。
在新机器上把渲染拆开打点（`firmware/receiver/main/perf.c`，挂 `LV_EVENT_RENDER_*`）之后，
真正的原因浮出水面：**不是旋转算法慢，是每帧重绘的面积太大**。

消融实测：

| 实验 | 渲染耗时 | 结论 |
|---|---|---|
| 每帧强制全屏重绘（无指针） | 58 ms | 217k 像素 ≈ 267 ns/像素，这是本平台的上限基准 |
| 指针静态、只改 recolor（不旋转） | 70 ms | 已经不慢了 —— 说明锅不在旋转 |
| 指针旋转（原基线） | 240 ms | 元凶在这里 |
| 只重绘扫过区域 + 关抗锯齿 | 20 ms | 脏区从 176k 像素降到 10–20k |

**根因**：`lv_image_set_rotation()` 和 `lv_obj_set_style_image_recolor()` **每帧调用都会把整个 360×360 图层标脏**，
LVGL 于是每帧重画 13 万像素。而 240° 里指针每帧其实只转过 1° 左右，真正变了的像素不到一万。

三条修复（缺一不可），细节见 `firmware/receiver/README.md` 第七节：

1. 指针改**自绘**（`LV_EVENT_DRAW_MAIN` + `lv_draw_image()`），角度/颜色存静态量 —— 改颜色不再触发整块作废
2. **脏矩形**：只作废「旧角度→新角度」扫过的那一带，并沿半径切成 3 段小矩形
   （斜着的指针包围盒接近整圆盘；切 6 段反而更慢，**每多一个脏区就多一趟渲染、每趟约 4 ms 固定开销**）
3. 主循环用 `perf_wait_frame()` 跟屏幕节奏，**一屏一更新**，不让脏区在一个刷新周期里叠加

另外两项白捡：

- `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` **160 → 240**（之前一直是 160，没人注意）
- `CONFIG_LV_DEF_REFR_PERIOD` **33 → 16 ms**（脏矩形把单帧压到 15 ms 以下后，33 ms 成了天花板）

最终：**42–59 fps（抗锯齿开），渲染 16–24 ms，刷屏 1 ms**。

**别再往这几个方向折腾**（都试过，无效）：
把指针图拷进内部 RAM / PSRAM（0 改善，瓶颈是面积不是带宽）、
加大 LVGL 绘制缓冲高度（0 改善）、`DRAW_UNIT_CNT=2` 双核绘制（面积问题不是算力问题）。

---

## 7. 后续工作方向（按优先级）

### P0 · ~~性能~~ ✅ 已完成（见第 6 节），42–59 fps

### P1 · 发送端上板联调（代码已就绪，等实测）

固件、接线方案、包都好了，按 `docs/sender-wiring.md` 第 5 节走：
桌面对联 → 只收不发看总线 → 发 `02 01 0C` → 全链路。

**供电红线**：16 脚是常电，开发板常驻会把电瓶耗干。必须走 ACC 开关电，或固件里做深度睡眠。
调试时**不要同时插 USB 和 12V**。改线前断电池负极。

### P2 · ESP-NOW 联通 ✅ 代码已完成（2026-11-02）

接收端 `link.c` 实现了 `esp_now_init` + 接收回调 + magic/version 校验 + seq 丢包统计，
`main.c` 已改成真实数据源（`USE_FAKE_WHEN_NO_LINK` 默认 0）。等实测验证。

### P3 · 显示细节（可选）

- 温度数字位置可能要微调 1–2 px（`TEMP_CX/CY`）
- AMOLED 纯白背景功耗高，考虑夜间深色模式（`docs/gauge-ui-beta.html` 里有 dark 变量组）
- 烧屏：静态元素（刻度、环带）亮度已经压低，长期观察后再决定是否要定时微移

---

## 8. 踩过的坑（按时间顺序，都是真实发生过的）

| # | 现象 | 原因 | 解法 |
|---|---|---|---|
| 1 | clone 只写了 3% 文件 | 检出被中断，git 仍报成功 | `read-tree` + `checkout-index -a -f` |
| 2 | 工具链 2 KB/s | dl.espressif.com 慢 | `IDF_GITHUB_ASSETS=dl.espressif.cn/github_assets` |
| 3 | `features.h: Operation not permitted` | 文件访问钩子把 ENOENT 变成 EPERM | `CODEBUDDY_BROKERED_FS_HOOK_ENABLED=0` |
| 4 | LVGL examples 编译失败 | 关了 slider 但 examples 引用它 | `CONFIG_LV_BUILD_EXAMPLES=n`（**改 sdkconfig，不是 defaults**，sdkconfig 优先） |
| 5 | 指针变黑方块 | L8 的 0 是黑色 | 改 A8 |
| 6 | 界面没创建 | `lock(0)` = 不等待 | `esp_lv_adapter_lock(-1)` |
| 7 | 5 秒一帧变 64 秒 | 两层图各自旋转，419 ms/帧 | 高光并入针身，单图层 |
| 8 | 串口读不到任何输出 | USB-JTAG 需要 DTR/RTS=False | 显式设置两者 |
| 9 | 轴心变蓝 | `lv_color32_t` 字节序是 B,G,R,A | png2c 里改正 |
| 10 | 盘面阴影糊成实心灰圆 | 阴影合成多写了一次不透明叠加 | 删掉那句 |

---

## 9. 速查命令

```bash
source ~/esp/env.sh                      # 或新机器上你自己写的等价脚本

cd firmware/receiver
idf.py -B build build                     # 编译
idf.py -B build -p /dev/ttyACM0 flash     # 烧录
./tools/flash.sh                          # 自动找串口

cd firmware/tests && make                 # 发送端纯逻辑单测，50 项
python3 docs/check-ui.py docs/gauge-ui.html   # 界面是否偏离 beta-1
```

---

## 10. 交接时的一句话

界面在真机上是对的，图都对、指针颜色随转速变、水温显示正常，**唯一没解决的是 5 fps**。
这个问题在上一台电脑上试错成本太高（改一行要等 1 小时重编），所以换机器再继续。
先解决帧率，再接 ESP-NOW，最后等发送端硬件。
