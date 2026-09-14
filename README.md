# ESP32-S3 Agent Display

**中文** | [English](README.en.md) · [开发环境搭建](docs/开发环境搭建.md) · [Dev Setup (EN)](docs/dev-setup.en.md)

ESP32-S3 N16R8 桌面状态屏：WebSocket 状态下行、离线 RGB565 RLE 表情、PDM 开机自动收听、PC 后端 ASR + LLM，网页日志带时间戳。

| 项 | 规格 |
|----|------|
| 固件框架 | ESP-IDF **5.4.2** + FreeRTOS + LVGL **9.2.2** |
| 通信 | WebSocket（`espressif/esp_websocket_client`） |
| 后端 | FastAPI + uvicorn，默认 `:8000` |

### 实景预览

| 硬件原型 | Web 控制台 |
|:--:|:--:|
| ![硬件原型：ESP32-S3 + ST7789 + 扬声器](docs/images/hardware-prototype.jpg) | ![Web 控制台 Dashboard](docs/images/dashboard.png) |
| ST7789 圆屏显示 Cursor Agent 状态（编码中） | 后端 Dashboard：状态推送、语音、BLE/AP 配网、LLM 配置 |

### 设计红线

1. **音频绑 Core 0**，优先级高于 LVGL；I2S 供数不及时会爆音、卡麦。
2. **任何非 `lvgl` 任务不得直接调用 LVGL API**；一律经 FreeRTOS 队列投递。
3. **表情用离线 RLE + Core 1 SPI 直绘**，设备端不启用 `lv_gif`。
4. **PDM DATA 用 GPIO16**，不要用 GPIO48（板载 WS2812）或 GPIO3（strapping）。
5. **LLM 密钥存本机 `backend/llm.json`（Dashboard 可改）或初始写在 `backend/.env`**，二者均已 gitignore，勿提交仓库。

```mermaid
flowchart LR
  gifSrc[emoji-gif] --> genPy[gen_frame_player.py]
  genPy --> animBin[animations.bin]
  animBin --> flashPart[animations分区 mmap]
  flashPart --> rle[Core1 RLE解码]
  rle --> spi[ST7789 SPI直绘]
  hook[Pi/Cursor Hook] --> backend[FastAPI :8000]
  backend --> ws[ws://pc:8000/ws]
  ws --> netTask[Core0 net_task]
  netTask -->|UI队列| lvglTask[Core1 lvgl_task]
  i2sTask[Core0 audio_task] --> pdm[PDM GPIO16/15]
  i2sTask --> amp[I2S功放 GPIO5/6/7]
```

---

## 目录

- [一、硬件](#一硬件)
  - [1.1 硬件架构](#11-硬件架构)
  - [1.2 引脚定义](#12-引脚定义)
  - [1.3 Flash 分区与内存占用](#13-flash-分区与内存占用)
  - [1.4 硬件与固件配置](#14-硬件与固件配置)
- [二、固件软件架构](#二固件软件架构)
  - [2.1 分层与模块](#21-分层与模块)
  - [2.2 启动流程](#22-启动流程)
  - [2.3 跨任务通信](#23-跨任务通信)
- [三、FreeRTOS 双核与任务分配](#三freertos-双核与任务分配)
  - [3.1 双核职责划分](#31-双核职责划分)
  - [3.2 应用任务一览](#32-应用任务一览)
  - [3.3 优先级与调度策略](#33-优先级与调度策略)
  - [3.4 IDF 系统任务与临时任务](#34-idf-系统任务与临时任务)
  - [3.5 内存分配策略](#35-内存分配策略)
  - [3.6 线程安全红线](#36-线程安全红线)
- [四、核心功能实现](#四核心功能实现)
  - [4.1 显示与 UI](#41-显示与-ui)
  - [4.2 表情动画（离线 RLE）](#42-表情动画离线-rle)
  - [4.3 中文字库](#43-中文字库)
  - [4.4 网络与 WebSocket](#44-网络与-websocket)
  - [4.5 语音交互](#45-语音交互)
  - [4.6 BLE 配网](#46-ble-配网)
  - [4.7 热点配网（AP）](#47-热点配网ap)
- [五、后端服务](#五后端服务)
  - [5.1 架构与模块](#51-架构与模块)
  - [5.2 WebSocket 协议](#52-websocket-协议)
  - [5.3 语音处理流水线](#53-语音处理流水线)
  - [5.4 LLM 配置与对接](#54-llm-配置与对接)
  - [5.5 Agent Hook 集成](#55-agent-hook-集成)
- [六、构建与部署](#六构建与部署)
  - [6.1 依赖清单](#61-依赖清单)
  - [6.2 构建与烧录](#62-构建与烧录)
  - [6.3 仓库结构](#63-仓库结构)
- [附录](#附录)

---

## 一、硬件

### 1.1 硬件架构

| 子系统 | 规格 | 说明 |
|--------|------|------|
| 主控 | ESP32-S3 N16R8 | 双核 Xtensa LX7，**240 MHz** |
| Flash | 16 MB QIO 80 MHz | 自定义 `partitions.csv` |
| PSRAM | 8 MB OPI 80 MHz | `SPIRAM_USE_MALLOC`，大块缓冲优先放 PSRAM |
| 显示 | ST7789 240×240 | **SPI**（非 I2C），RGB565 16 bit |
| 麦克风 | PDM，I2S0 RX | 16 kHz / mono / 16 bit |
| 功放 | I2S1 TX，Philips | 16 kHz / 16 bit，播放侧写 stereo |
| 控制台 | USB Serial/JTAG | GPIO19 / GPIO20 |
| 按键 | BOOT（GPIO0） | 长按 BLE 配网 / 超长按诊断；WiFi 失败时自动热点配网 |

音频格式：16 kHz 单声道 s16le；单段最长 15 s（VAD）/ 缓冲 20 s；播放 ring **512 KB**（PSRAM）。

### 1.2 引脚定义

#### ST7789 屏幕

| 信号 | GPIO |
|------|------|
| GND | GND |
| VCC / BLK | 3V3 |
| CS | 8 |
| DC | 9 |
| RST | 10 |
| SDA (MOSI) | 11 |
| SCL (SCLK) | 12 |

#### I2S 功放（I2S1 TX）

| 信号 | GPIO | 说明 |
|------|------|------|
| DIN | 5 | 数据 |
| LRCLK / WS | 6 | 左右声道时钟 |
| BCLK | 7 | 位时钟 |

功放 VCC 建议 **5V**（3V3 音量偏低），与 ESP、屏幕共地。

#### PDM 麦克风（I2S0 RX）

| 信号 | GPIO | 说明 |
|------|------|------|
| CLK | 15 | 仅在 `audio_task` 启动后开启 |
| DATA | **16** | 不用 GPIO3（strapping）或 GPIO48（板载 WS2812） |

无录音键：开机 VAD 自动收听。GPIO17 / 18 空闲备用。

#### 禁止占用

| GPIO | 原因 |
|------|------|
| 19 / 20 | 原生 USB Serial/JTAG |
| 26–32 | N16R8 OPI Flash / PSRAM |
| 45 / 46 | strapping（VDD_SPI / boot） |
| 48 | 板载 RGB LED |

GPIO3 在复位时会参与 strapping，接麦 DATA 可能干扰 USB 下载。

### 1.3 Flash 分区与内存占用

> 体积对应当前构建产物；分区定义见 `partitions.csv`。构建或烧录资源后若 bin 变化，请更新本节数字。

#### Flash — 16 MB

**占用排名（按文件大小降序）**

| 排名 | 分区 | 资源 | 文件 | 大小 | 占分区 | 占 Flash |
|------|------|------|------|------|--------|----------|
| 1 | animations | 表情 GIF → RLE 帧序列（mmap） | `firmware/data/animations.bin` | 4.41 MB | 73.4% | 27.5% |
| 2 | app0 | 主程序固件 | `build/esp32s3_agent_display.bin` | 1.38 MB | 46.0% | 8.6% |
| 3 | cjk_font | 中文字库 | `firmware/data/font_cjk_16.bin` | 893 KB | 43.6% | 5.4% |
| 4 | bootloader | 引导程序 | `build/bootloader/bootloader.bin` | 21.8 KB | 68.3% | 0.1% |
| 5 | otadata | OTA 槽位 | `build/ota_data_initial.bin` | 8.0 KB | 100% | 0.05% |
| 6 | partition-table | 分区表 | `build/partition_table/partition-table.bin` | 3.0 KB | 75.0% | 0.02% |

**未占用 / 运行时分区**

| 分区 | 偏移 | 容量 | 说明 |
|------|------|------|------|
| nvs | `0x009000` | 20 KB | BLE / `agent_cfg`、WiFi 等 |
| spiffs | `0x310000` | 896 KB | 遗留空 |
| coredump | `0x3F0000` | 64 KB | 崩溃转储 |
| storage | `0xC00000` | 4 MB | 预留 |

烧录：`scripts/flash_animations.py` → `0x600000`；`scripts/flash_font.py` → `0x400000`。

#### PSRAM — 8 MB

| 排名 | 占用项 | 源码 | 大小 | 占 PSRAM |
|------|--------|------|------|----------|
| 1 | CJK 字库运行时 | `main/font_loader.c` | 893 KB | 10.9% |
| 2 | 录音 PCM | `VOICE_MAX_RECORD_BYTES` | 640 KB | 7.6% |
| 3 | 播放 ring | `VOICE_PLAY_RING_BYTES` | 512 KB | 6.3% |
| 4 | 语音历史 | `VOICE_HISTORY_BYTES` | 128 KB | 1.6% |
| 5 | LVGL partial×2 | `PARTIAL_BUF_LINES=40` | 37.5 KB | 0.5% |

已知缓冲合计 ≈ **2.14 MB**（26.8%），空闲堆 ≈ **5.86 MB**。表情 RLE 双缓冲（≈32 KB）在**片内 DRAM**。

#### 片内 DRAM — 约 512 KB 可用池（示意）

| 排名 | 占用项 | 源码 / 配置 | 大小 |
|------|--------|-------------|------|
| 1 | 应用任务栈等 | FreeRTOS 各任务 | ≈80 KB |
| 2 | SPIRAM 预留片内 | `SPIRAM_MALLOC_RESERVE_INTERNAL=32768` | 32 KB |
| 3 | 表情双缓冲 BSS | `main/ui.c` `frame_buffer_a/b` | 32 KB |

NimBLE / WiFi DMA 为动态占用。配网前需 `wifi_deinit` 腾连续块，否则 BT controller 可能 `Malloc failed` 断言重启。

### 1.4 硬件与固件配置

`sdkconfig.defaults` 关键项：

| 配置 | 值 | 说明 |
|------|-----|------|
| `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` | 240 | 必须 240 MHz |
| `CONFIG_SPIRAM_MODE_OCT` | y | OPI PSRAM |
| `CONFIG_SPIRAM_USE_MALLOC` | y | malloc 可走 PSRAM |
| `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` | 32768 | 保留片内给 DMA / BT |
| `CONFIG_FREERTOS_HZ` | 1000 | 1 ms tick |
| `CONFIG_LV_USE_GIF` | n | 禁用运行时 GIF |
| `CONFIG_LV_USE_CLIB_MALLOC` | y | CJK binfont 需大堆 |
| `CONFIG_AGENT_VOICE_HW` | y | 启用 PDM + 功放 |
| `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL` | y | NimBLE 堆走 PSRAM |

menuconfig「Agent Display」：WiFi SSID/密码、静态 IP、`AGENT_WS_URL` 等。出厂默认可读 Kconfig；BLE 写入 NVS 后 **NVS 优先**。

---

## 二、固件软件架构

### 2.1 分层与模块

```text
┌─────────────────────────────────────────────────────────────┐
│  PC 后端 (FastAPI)  ←── WebSocket /ws ──→  net_ws.c         │
├─────────────────────────────────────────────────────────────┤
│  应用层   voice.c (VAD)  │  ui.c (LVGL)  │  ble_prov / ap_prov │
├─────────────────────────────────────────────────────────────┤
│  服务层   audio.c  │  anim_loader.c  │  font_loader.c       │
├─────────────────────────────────────────────────────────────┤
│  驱动层   display.c (SPI/ST7789)  │  I2S PDM/功放  │  GPIO │
├─────────────────────────────────────────────────────────────┤
│  IDF      WiFi / lwIP / NimBLE / NVS / esp_timer           │
└─────────────────────────────────────────────────────────────┘
```

| 模块 | 文件 | 职责 |
|------|------|------|
| 入口 | `main.c` | `app_main` 初始化、创建任务 |
| 显示 | `display.c` | SPI 总线、ST7789、`display_blit_rgb565` 直绘 |
| UI | `ui.c` / `ui_msg.h` | LVGL 控件、消息队列、表情刷新、字幕 |
| 动画 | `anim_loader.c` | mmap `animations` 分区、帧表索引 |
| 字库 | `font_loader.c` | 从 `cjk_font` 分区加载 binfont |
| 网络 | `net_ws.c` | WiFi、WebSocket、语音上传/下行 |
| 音频 | `audio.c` | PDM 采集、AGC、I2S 播放 ring |
| 语音 | `voice.c` | VAD 状态机、边录边传、UI 叠加 |
| 配网 | `ble_prov.c` / `ap_prov.c` / `prov_cfg.c` / `agent_cfg.c` | BLE NUS、Soft AP 网页、共享 NVS |
| 串口 CLI | `serial_cli.c` | `ap` / `ap_stop` / `ble_stop` 调试命令 |
| 按键 | `btn_boot.c` | BOOT 长按检测 |
| 内存 | `mem_utils.c` | `psram_malloc` / `dram_malloc`、诊断报告 |

### 2.2 启动流程

`app_main`（在 IDF **main 任务**上同步执行，Core 0）顺序：

1. `nvs_flash_init`
2. `anim_loader_init` — mmap 表情分区
3. `display_init` — SPI + ST7789 + LVGL display
4. `font_cjk_init` — 字库载入 PSRAM
5. `ui_init` — 创建 UI 队列与控件
6. `agent_cfg_load` — 读 NVS / Kconfig
7. `btn_boot_init` — 创建 `btn_boot` 任务
8. `net_init` — WiFi 栈（尚未建 `net_task` 循环）
9. `serial_cli_init` — 串口命令行（`ap` 等）
10. `voice_init` — 分配 PSRAM 缓冲
11. `mem_report("boot")`
12. 创建常驻任务：`main.c` 建 `audio` / `net` / `lvgl` / `app`；`btn_boot_init` 建 `btn_boot`；`serial_cli_init` 建 `cli`（见 [§3.2](#32-应用任务一览)）

此后 `app_main` 返回，main 任务进入空闲；业务逻辑由各 FreeRTOS 任务承担。

### 2.3 跨任务通信

```mermaid
flowchart TB
  subgraph Core0["Core 0"]
    audio[audio_task]
    net[net_task]
  end
  subgraph Core1["Core 1"]
    app[app_task]
    lvgl[lvgl_task]
    btn[btn_boot]
  end
  net -->|ui_post_event_json| Q[UI 队列 depth=16]
  app -->|ui_post_voice_link / link_state| Q
  btn -->|ui_post_ble_prov| Q
  voice[voice_loop in app] -->|voice_net_poll in net| net
  audio <-->|PCM ring / 播放状态| voice
  Q --> lvgl
  lvgl -->|refresh_face_display SPI直绘| LCD[ST7789]
```

| 通道 | 类型 | 生产者 | 消费者 | 说明 |
|------|------|--------|--------|------|
| UI 队列 | `xQueueCreate(16, ui_msg_t)` | `net_task` / `app_task` / `btn_boot` | `lvgl_task` | 唯一 LVGL 写入口 |
| 语音链路 | 函数调用 + 共享缓冲 | `voice.c` | `net_ws.c` / `audio.c` | 录音 PCM、上传块在 PSRAM |
| `voice_link` 合并 | `portENTER_CRITICAL` | `voice.c` | `ui.c` | 避免队列丢消息卡在 SPEAKING |
| WebSocket | `esp_websocket_client` 回调 | IDF 内部 | `net_task` 轮询 | 不在回调里直接刷 UI |

---

## 三、FreeRTOS 双核与任务分配

ESP32-S3 为 **双核**（`CONFIG_FREERTOS_UNICORE` 未启用）。应用任务一律 `xTaskCreatePinnedToCore(..., core_id)` 显式绑核。

### 3.1 双核职责划分

| 核心 | 定位 | 常驻负载 | 设计理由 |
|------|------|----------|----------|
| **Core 0** | 实时 I/O + 网络 | `audio_task`、`net_task`、WiFi、lwIP、（配网时）NimBLE Host | I2S DMA 与 WiFi 协议栈对抖动敏感；音频与网络同核减少跨核同步 |
| **Core 1** | 显示 + 应用逻辑 | `lvgl_task`、`app_task`、`btn_boot`、`cli` | LVGL `lv_timer_handler`、RLE 解码、SPI `draw_bitmap` 集中在一核，避免与 I2S 争用 |

```text
Core 0                          Core 1
────────                        ────────
audio_task  (pri 6)             lvgl_task (pri 4)
  ├ I2S0 PDM RX                   ├ xQueueReceive → LVGL API
  └ I2S1 功放 TX                  ├ refresh_face_display()
net_task    (pri 4)               └ RLE 解码 + SPI 直绘 90×90
  ├ WiFi 事件
  ├ esp_websocket_client
  └ voice_net_poll()
WiFi / lwIP (IDF)               app_task  (pri 3)
NimBLE Host (按需)                ├ voice_loop() VAD
                                  ├ ui_tick_animation()
                                  ├ ble_prov_loop() / ap_prov_loop()
                                  └ link_state 周期投递
                                btn_boot  (pri 6)
                                  └ GPIO0 长按轮询
```

### 3.2 应用任务一览

创建位置：`main/main.c` `app_main` 末尾；`btn_boot` 在 `btn_boot_init`。

| 任务名 | 核心 | 优先级 | 栈 (B) | 周期 / 阻塞 | 职责 |
|--------|------|--------|--------|-------------|------|
| `audio` | **0** | **6** | 8192 | `audio_task_loop` + `vTaskDelay(5ms)` | I2S0 PDM 读入、AGC、录音缓冲写入；I2S1 播放出队；**播放期停麦** |
| `net` | **0** | **4** | 8192 | `net_loop` + `vTaskDelay(20ms)` | WiFi 连接/重连、`esp_websocket_client`、JSON 事件解析、`voice_net_poll` 流式上传 |
| `lvgl` | **1** | **4** | 8192 | `ui_loop_once` + `vTaskDelay(5ms)` | 排空 UI 队列、调用 LVGL、`refresh_face_display`、表情帧 SPI 直绘 |
| `app` | **1** | **3** | 8192 | `vTaskDelay(5ms)` | `voice_loop` VAD 状态机；动画 tick；`ble_prov_loop` / `ap_prov_loop`；1 s 时钟、250 ms 链路状态投递 UI |
| `btn_boot` | **1** | **6** | 4096 | `vTaskDelay(20ms)` 轮询 | BOOT 按住 ≥3 s → BLE；≥5 s → 诊断覆盖层 |
| `cli` | **1** | **2** | 4096 | `getchar` 阻塞 | 串口命令：`ap` / `ap_stop` / `ble_stop` / `help` |

**临时任务**（非常驻）：

| 任务名 | 核心 | 优先级 | 栈 | 触发 | 说明 |
|--------|------|--------|-----|------|------|
| `ble_start` | 未绑核 | 5 | 8192 | `btn_boot` 长按 | 调用 `ble_prov_start()` 后自删除 |
| `NimBLE Host` | IDF 默认 | — | 8192 | 首次 BLE 配网 | `nimble_port_freertos_init`；`CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` |

### 3.3 优先级与调度策略

FreeRTOS 优先级：**数值越大越优先**（ESP-IDF 默认可用范围通常 0–24，应用使用 3–6）。

```text
优先级 (高 → 低)

  6  audio_task ─────────  Core 0  I2S 实时供数，必须高于 LVGL
  6  btn_boot   ─────────  Core 1  按键响应（与 audio 不同核，不冲突）
  5  ble_start  (临时)
  4  net_task   ─────────  Core 0  WebSocket / WiFi
  4  lvgl_task  ─────────  Core 1  显示刷新
  3  app_task   ─────────  Core 1  VAD 决策（可容忍数 ms 抖动）
  2  cli        ─────────  Core 1  串口调试（最低，不影响实时路径）
```

**为何音频 = 6、LVGL = 4？**  
`audio_task` 每 5 ms 从 I2S 取数；若优先级低于 `lvgl_task`，SPI 刷屏或 RLE 解码会饿死音频，导致 DMA 欠载、爆音。`app_task` 最低：VAD 以帧为单位（数十 ms），通过队列异步更新 UI 即可。

**同优先级：** `net_task` 与 `lvgl_task` 均为 4，分属不同核心，互不抢占。

**tick：** `CONFIG_FREERTOS_HZ=1000`，`vTaskDelay(1)` = 1 ms。`lvgl_task` / `app_task` 5 ms 循环 ≈ 200 Hz；`net_task` 20 ms ≈ 50 Hz 足够驱动 WS 与上传。

### 3.4 IDF 系统任务与临时任务

除上表外，IDF 自动创建（未绑核或内部绑核）：

| 任务 / 组件 | 栈配置 | 说明 |
|-------------|--------|------|
| `main` | `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` | 跑 `app_main` 后闲置 |
| `tiT` (lwIP tcpip) | `CONFIG_LWIP_TCPIP_TASK_STACK_SIZE=4096` | TCP/IP 协议栈 |
| WiFi 任务 | IDF 内置 | 与 `net_task` 协作 |
| `esp_timer` | IDF 内置 | LVGL tick（5 ms）、按键时间戳 |
| Idle (×2) | — | 每核一个 |
| NimBLE Host | 8192 | 仅配网窗口内 |

BLE 配网特殊流程：`ensure_nimble()` 前调用 `net_pause_for_ble()`（`wifi_stop` + `wifi_deinit`）释放片内连续块；失败则 `net_resume_after_ble()`。NimBLE 内存走外部：`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y`。

### 3.5 内存分配策略

| API | 能力位 | 用途 |
|-----|--------|------|
| `psram_malloc()` | `MALLOC_CAP_SPIRAM` | 录音 PCM、播放 ring、语音历史、CJK 字库、LVGL partial buffer |
| `dram_malloc()` | `MALLOC_CAP_DMA \| INTERNAL` | 需 DMA 的驱动缓冲 |
| `malloc()` | 先 internal ≤4 KB，否则 PSRAM | `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` |

大块原则：**>4 KB 默认 PSRAM**；片内保留 32 KB 给 WiFi/BT DMA。表情 `frame_buffer_a/b`（90×90×2×2）故意放 **BSS 片内**，减轻 PSRAM 带宽争用。

启动后 `mem_report()` 输出 DRAM/PSRAM free 与 largest block；PC 发 `{"type":"debug"}` 或 **按住 BOOT ≥5 s** 可看屏幕诊断。

### 3.6 线程安全红线

**禁止：**

1. 非 `lvgl` 任务调用任何 LVGL API（`lv_label_set_text`、`lv_obj_create` 等）。
2. 在 ISR 中调用 LVGL。
3. 多任务无锁同时读写 PCM / 文本缓冲。
4. 把 I2S 读写放进 `lvgl_task`，或与 UI 共用一个低优先级循环。
5. 把 CJK 字库套到 `LV_SYMBOL` 图标上（图标继续用 Montserrat）。

**正确做法：**

- `app_task` / `net_task` 只 `ui_post_*` 投递队列；`lvgl_task` 独占 LVGL。
- 动画**计时**在 `app_task`（`ui_tick_animation`），**解码与刷屏**只在 `lvgl_task`（`refresh_face_display`）。
- `ui_post_voice_link()` 在临界区内合并 `voice_hold` / `overlay`，防止连续帧覆盖 `voice_hold=false`。
- 长文本不进队列体：语音历史用 PSRAM 指针；`ui_msg_t.json` 上限 **1024 B**。
- `source=VOICE` 的 WS 事件**只更新底部字幕**；中部 GIF/状态行由 `refresh_face_display()` 统一决定。Agent `status` 在 `voice_hold` 期间暂存 `agent_status`，结束后恢复。

---

## 四、核心功能实现

### 4.1 显示与 UI

- **LVGL 9.2**：`PARTIAL_BUF_LINES=40`，双 partial buffer（PSRAM），刷新周期 `CONFIG_LV_DEF_REFR_PERIOD=15` ms。
- **布局**：顶部来源行、中部 90×90 表情区、状态中文、底部语音字幕（最多 2 行）。
- **刷新优先级**（`refresh_face_display`）：

| 优先级 | 条件 | 显示 |
|--------|------|------|
| 1 | WS 未连接 | `OFFLINE` |
| 2 | `voice_hold=true` | `voice_overlay`（EAR / THINKING / SPEAKING） |
| 3 | 其它 | `agent_status`（Hook 推送） |

- **录音音量条**：仅 `voice_hold` + 监听 + `overlay=EAR` 时显示 80×4 绿色条。

### 4.2 表情动画（离线 RLE）

PC 将 GIF 展开、按背景 `#10151B` 预混合，编成 90×90 RGB565 RLE。设备 **禁止** `lv_gif`。

```text
mmap(animations) → gif_id 帧表 → Core1 RLE 解到双缓冲 → display_blit_rgb565
```

二进制（小端）：`magic "AGIF"` + version + anim_count + width/height；每套 `frame_count/table_off`；每帧 `duration_ms/rle_size/rle_off`；RLE `(count, hi, lo)` 重复。

当前：`ANIM_BIN_SIZE=3557266`，`ANIM_FACE_SIZE=90`，`ANIM_FRAME_COUNT=434`。

| gif_id | status | 中文 | 源文件 |
|--------|--------|------|--------|
| 0 | IDLE | 空闲 | `IDLE.gif` |
| 1 | THINKING | 思考中 | `THINKING.gif` |
| 2 | CODING | 编码中 | `CODING.gif` |
| 3 | READING | 读取中 | `READING.gif` |
| 4 | TESTING | 测试中 | `TESTING.gif` |
| 5 | WAITING | 等待中 | `WAITING.gif` |
| 6 | DONE | 已完成 | `DONE.gif` |
| 7 | ERROR | 错误 | `ERROR.gif` |
| 8 | OFFLINE | 离线 | `OFFLINE.gif` |
| 9 | STALE | 已过期 | `STALE.gif` |
| 10 | UNKNOWN | 未知 | `UNKNOWN.gif` |
| 11 | TOOL | 使用工具 | `TOOL.gif` |
| 12 | EAR | 收听中 | `EAR.gif` |
| 13 | SPEAKING | 播报中 | `SPEAKING.gif` |

重新生成：

```bash
python scripts/gen_frame_player.py
python scripts/flash_animations.py -p COMx
```

只烧 app、不烧 `animations.bin` → 有字无表情。

### 4.3 中文字库

独立分区 `cjk_font`（`0x400000`，2 MB），16px / 4bpp，`simhei.ttf` 源。字模组成：

| 来源 | 文件 / 范围 | 说明 |
|------|-------------|------|
| 通用字表 | `third_party/fonts/tyz_7000_chars.txt` | 《现代汉语通用字表》7000 字 |
| 额外标点 | `third_party/fonts/extra_symbols.txt` | 弯引号 `“”‘’`、书名号、破折号等 LLM 常用符号（不在 7000 字内） |
| ASCII | `0x20-0x7F` | 含直引号 `"`、数字、字母等 |

增补标点请改 `extra_symbols.txt` 后重新生成；**不建议**为标点问题扩到 GBK 全字库（2 MB 分区放不下 16px/4bpp 体量）。

```bash
python scripts/gen_cjk_font.py
python scripts/flash_font.py -p COMx
```

`font_cjk_init()` 拷贝约 900 KB 到 PSRAM；须 `CONFIG_LV_USE_CLIB_MALLOC`。未烧字库则中文退回 Montserrat。

### 4.4 网络与 WebSocket

- 组件：`espressif/esp_websocket_client`
- 连接后发 `{"type":"hello","role":"device","rssi":…}`；服务端 `ack` 含 `server_time`
- 断线约 **3 s** 重连（`WS_RECONNECT_MS`）
- `source`：`PI` / `CURSOR` / `VOICE` / `BOT` / `UNKNOWN`；设备来源行**不显示 `VOICE`**

通用帧类型见 [§5.2](#52-websocket-协议)。

### 4.5 语音交互

```mermaid
sequenceDiagram
    participant Mic as PDM 麦克风
    participant ESP as ESP32 voice/audio
    participant WS as WebSocket /ws
    participant BE as 后端 pipeline
    participant Amp as I2S 功放

    Mic->>ESP: 连续采样 16kHz PCM
    ESP->>ESP: VAD 触发录音
    ESP->>WS: text audio_upload (stream=true)
    WS->>BE: 创建 session + VoskStreamRecognizer
    BE->>WS: session + volume_percent
    loop 边录边传
        ESP->>WS: binary PCM（4096 B/块）
        WS->>BE: AcceptWaveform
    end
    ESP->>WS: text audio_end
    BE->>BE: FinalResult → LLM
    BE->>WS: status THINKING + 文本
    alt VOICE_TTS=1
        BE->>WS: audio_chunk + binary
        WS->>ESP: 停麦播放
        ESP->>Amp: I2S 输出
    else VOICE_TTS=0
        BE->>WS: status IDLE + 回复文本
    end
    ESP->>ESP: 恢复 VAD
```

#### 设备端参数

| 项 | 值 |
|----|-----|
| 流式上传块 | 4096 B（≈128 ms） |
| 录音增益 | PDM AGC 3–10×，初始 5× |
| 播放增益 | `PLAY_GAIN_100 × volume_percent / 100` |
| 高通 + 低通 | `LP_ALPHA=0.7`；播放淡出 `FADE_SAMPLES=160` |

#### VAD 状态机（`main/voice.c`）

```text
LISTEN ──(越阈且 WS 已连接)──► RECORDING
RECORDING ──(静音/最长时长)──► UPLOADING
UPLOADING ──(流式结束或 bulk)──► WAIT_REPLY
WAIT_REPLY ──(audio_chunk)──► PLAYING
WAIT_REPLY ──(IDLE|ERROR / 超时)──► LISTEN
PLAYING ──(播完)──► LISTEN（冷却 800 ms）
```

| 常量 | 值 | 含义 |
|------|-----|------|
| `VAD_RMS_START_FLOOR` | 0.012 | 起始 RMS 下限 |
| `VAD_PEAK_START_FLOOR` | 0.060 | 起始 Peak 下限 |
| `VAD_NOISE_MULT` | 3.5 | 阈值 = max(floor, 噪声×倍数) |
| `VAD_SILENCE_MS` | 800 | 静音结束录音 |
| `VAD_MAX_MS` | 15000 | 单段最长 |
| `WAIT_REPLY_MS` | 55000 | 等待后端超时 |
| `MIN_CLIP_BYTES` | 6400 | 过短丢弃 |

#### 任务分工（语音相关）

| 任务 | 核心 | 职责 |
|------|------|------|
| `audio_task` | 0 | PDM 读入、录音缓冲、I2S 播放 |
| `net_task` | 0 | `voice_net_poll` 开流/推块/`audio_end` |
| `app_task` | 1 | `voice_loop` VAD、`post_voice_ui` |
| `lvgl_task` | 1 | 字幕、`refresh_face_display` |

#### 播放期行为

- 收到首个 `audio_chunk`：停 PDM，进 `PLAYING`；**无 barge-in**
- `resume_listen()` 须**先**切 `s_phase=LISTEN` 再 `post_voice_ui()`，否则 UI 误留 `SPEAKING`
- 末片无 `end`：播放环空后 **300 ms** 兜底恢复

语音 WebSocket 帧格式、后端流水线、LLM 密钥配置见 [§5.2](#52-websocket-协议)、[§5.3](#53-语音处理流水线)、[§5.4](#54-llm-配置与对接)。

#### 语音相关源码

| 路径 | 职责 |
|------|------|
| `main/voice.c` | VAD、边录边传、`voice_on_ws_lost` |
| `main/audio.c` | PDM、AGC、播放 ring |
| `main/net_ws.c` | `audio_upload` / binary / `audio_chunk` |
| `main/ui.c` | 字幕、PSRAM 历史、`refresh_face_display` |
| `backend/ws_manager.py` | WS hub、流式会话、音频泵 |
| `backend/voice_pipeline.py` | ASR → LLM → TTS |

### 4.6 BLE 配网

设备支持两种配网入口：**BLE（§4.6）** 与 **热点网页（§4.7）**。二者写入同一套 `agent_cfg` NVS（WiFi + 后端 URL + 可选静态 IP），字段语义一致；**BLE 与 AP 互斥**，不可同时进行。

ESP32-S3 **仅 BLE**（NimBLE + Nordic UART Service）。Windows 系统蓝牙设置**不会**弹出配对框——用本仓库 Dashboard 或 SerialTest（LE）。

#### 触发方式

| 操作 | 结果 |
|------|------|
| 长按 BOOT ≥ **3 s** | 蓝色蓝牙图标 → NimBLE 广播 `AgentDisplay` |
| 按住 BOOT ≥ **5 s** | 运行时诊断覆盖层（约 12 s） |
| 按住 BOOT → 短按 RST | 下载模式（strapping） |

配网窗口最长约 **5 分钟**；`APPLY` 成功后约 0.5 s 结束广播并重连 WiFi/WS。

#### NUS UUID

| 角色 | UUID |
|------|------|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| RX | `6E400002-…` |
| TX | `6E400003-…` |

#### 文本协议（UTF-8，`\n` 结尾）

```text
WIFI:<ssid>,<password>
HOST:<ip>:<port>
IP:<x.x.x.x>          # 可选静态 IP
MASK:<x.x.x.x>
GW:<x.x.x.x>
GET
APPLY
```

#### 网页配网

1. `pip install -r backend/requirements.txt`（含 `bleak`）
2. 打开 Dashboard → **BLE 配网**
3. 设备长按 BOOT → 扫描 → 填写 WiFi / 后端 →「写入并 APPLY」

| API | 说明 |
|-----|------|
| `GET /api/ble/status` | bleak 可用性、日志 |
| `POST /api/ble/scan` | 扫描设备 |
| `POST /api/ble/provision` | 写入并 APPLY |

实现：`backend/ble_prov.py`、`backend/dashboard.html`。

### 4.7 热点配网（AP）

启动后若 **30 秒内** WiFi 与后端 WebSocket 均未就绪，设备自动开启 **开放热点** 并内置配网网页；也可通过串口手动触发。配置保存后与 BLE `APPLY` 相同：写入 NVS、关闭热点、以 STA 模式重连。

与 BLE 不同，热点配网**无需** `wifi_deinit`，内存占用更小，适合「连不上 WiFi」时的兜底场景。

#### 触发方式

| 方式 | 说明 |
|------|------|
| **自动** | 上电后 30 s 内未连上 WiFi+WS → 自动开热点 |
| **串口** | 监视器或串口工具发送 `ap`（见下方 CLI） |
| **手动停止** | 串口发送 `ap_stop`，或等待 10 分钟超时 |

屏幕显示 **「热点配网」**，来源标签为 `AP`。

#### 热点与网页

| 项 | 值 |
|----|-----|
| SSID | `AgentDisplay-XXXX`（`XXXX` 为 MAC 后两字节，如 `AgentDisplay-9344`） |
| 密码 | **无**（开放网络） |
| 设备 IP | `192.168.4.1` |
| 配网页 | [http://192.168.4.1/](http://192.168.4.1/) |

手机/电脑连接热点后，浏览器打开上述地址（部分系统会自动弹出 captive portal）。

#### 网页表单字段

与 BLE 协议字段一一对应，保存后由 `prov_cfg.c` 统一处理：

| 字段 | 说明 |
|------|------|
| WiFi 名称 / 密码 | 目标路由器 SSID 与密码 |
| 后端 IP / 端口 | 拼为 `ws://<host>:<port>/ws` |
| 设备静态 IP / 掩码 / 网关 | 可选；留空则 DHCP |

点击 **「保存并连接」** 后：

1. 设备先返回 `{"ok":true}`，网页弹出 **「配置保存成功」** 提示
2. 约 **3.5 s** 后设备保存 NVS、关闭热点、切换 STA 并重连 WiFi/WS
3. 用户应 **切回原 WiFi**，等待设备上线

#### 设备 HTTP API（热点模式下）

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/` | 配网 HTML 页面 |
| `GET` | `/api/config` | 读取当前内存中的配置（JSON） |
| `POST` | `/api/config` | 提交配置（JSON body），成功返回 `{"ok":true}` 并延迟应用 |

`GET /api/config` 响应示例：

```json
{
  "ok": true,
  "ssid": "MyWiFi",
  "password": "12345678",
  "host": "192.168.10.167",
  "port": 8000,
  "ip": "",
  "netmask": "",
  "gateway": "",
  "profile_count": 1,
  "active": 0
}
```

`POST /api/config` 请求体：

```json
{
  "ssid": "MyWiFi",
  "password": "12345678",
  "host": "192.168.10.167",
  "port": 8000,
  "ip": "",
  "netmask": "",
  "gateway": ""
}
```

成功响应：`{"ok":true}`（约 3.5 s 后应用并关闭热点）。

#### 串口 CLI（调试）

控制台（USB Serial/JTAG / CH343，默认 **COMx @ 115200**）启动后约 2 s 出现提示：

```text
>>> CLI: ap | ap_stop | ble_stop | help
```

| 命令 | 作用 |
|------|------|
| `ap` | 立即进入热点配网 |
| `ap_stop` | 关闭热点，恢复 STA 尝试 |
| `ble_stop` | 退出 BLE 配网窗口 |
| `help` | 列出命令 |

内置终端示例（PowerShell / Git Bash，项目根目录）：

```bash
scripts/idf_build.bat -p COMx monitor
# 出现 CLI 提示后输入：
ap
```

> 串口只能被一个程序占用：若已打开 `monitor`，不要再另开串口工具发命令。

#### 多 Profile 与轮询

NVS 最多保存 **5 条** profile（WiFi 与 `ws_url` 绑定在同一条）。启动后若当前 profile 连不上，会轮询切换其他已保存配置；全部失败后进入热点配网。

#### 实现文件

| 文件 | 职责 |
|------|------|
| `main/ap_prov.c` | Soft AP、`esp_http_server`、内嵌配网页 |
| `main/prov_cfg.c` | BLE / AP 共享的解析、NVS 保存、`net_apply_config` |
| `main/net_ws.c` | 30 s 超时触发、`net_force_ap_prov()` |
| `main/serial_cli.c` | 串口 `ap` 等命令 |
| `main/ui.c` | 热点配网 UI（「热点配网」+ `AP` 来源） |

---

## 五、后端服务

### 5.1 架构与模块

目录 `backend/`，入口 `main.py`，默认 `0.0.0.0:8000`。

| 模块 | 职责 |
|------|------|
| `main.py` | WebSocket `/ws`、REST API、Hook 队列轮询 |
| `ws_manager.py` | 设备连接、`audio_upload`、TTS 泵、`push_append` |
| `voice_pipeline.py` | ASR → LLM →（可选）TTS 编排 |
| `voice_session.py` | 会话 PCM / 分片队列 |
| `asr.py` | Vosk 流式 `VoskStreamRecognizer` |
| `llm.py` | OpenAI 兼容 Chat Completions |
| `chat_context.py` | 多轮上下文 |
| `ble_prov.py` | 网页 BLE 配网 |
| `dashboard.html` | 状态推送、语音日志 |

启动 / 停止（仓库根目录）：

```bat
start.bat
stop.bat
```

### 5.2 WebSocket 协议

端点：`ws://<pc-ip>:8000/ws`。连接后首帧须为 JSON text；二进制帧仅紧跟 `audio_upload`（整段）或 `audio_chunk` 头。

**约定**

| 项 | 值 |
|----|-----|
| 音频格式 | 16 kHz / mono / s16le |
| 流式上传块 | 4096 B（`VOICE_UPLOAD_CHUNK`） |
| 等待 `session` | 设备侧最长 **2.5 s**（`net_ws.c`） |
| TTS 泵送突发上限 | **16384 B**（`AUDIO_BURST_BYTES`） |
| 设备保活 | 服务端每 **2 s** 发 `ping`；**5 s** 无上行则判离线 |

`volume_percent` 仅设备端增益生效；后端 TTS **不二次缩放 PCM**。

---

#### 5.2.1 连接与保活

**ESP → PC：`hello`**（`WEBSOCKET_EVENT_CONNECTED` 后自动发送）

```json
{"type":"hello","role":"device","rssi":-58}
```

**PC → ESP：`ack`**（连接后立即下发；含可选校时）

```json
{"type":"ack","role":"device","server_time":1735689600}
```

**双向：`ping` / `pong`**

```json
{"type":"ping"}
```

```json
{"type":"pong","server_time":1735689600}
```

设备收到 `ping` 回 `{"type":"pong"}`（无 `server_time`）；服务端 `ping` 可带 `server_time`。

---

#### 5.2.2 Agent 状态下行

**PC → ESP：`status`**（Hook / Dashboard 推送；`gif` 与 `status` 同键）

```json
{
  "type": "status",
  "status": "THINKING",
  "source": "CURSOR",
  "text": "正在分析代码…",
  "gif": "THINKING",
  "time": "14:32:05",
  "status_detail": "tool_running",
  "tool_category": "read",
  "task_label": "explore"
}
```

`text` 可省略；`time` 可为字符串或 Unix 秒数，用于校时。`status_detail` / `tool_category` / `task_label` 为可选扩展字段。

**PC → ESP：`text`**（仅更新字幕，不改中部 GIF）

```json
{
  "type": "text",
  "text": "你好，有什么可以帮你？",
  "source": "BOT"
}
```

**PC → ESP：`append`**（流式 ASR / LLM 字幕增量）

```json
{
  "type": "append",
  "text": "你好",
  "source": "VOICE",
  "role": "assistant",
  "reset": true
}
```

| 字段 | 说明 |
|------|------|
| `role` | `user`（ASR）或 `assistant`（LLM） |
| `reset` | `true` 时开始新段落（覆盖当前行） |

**ESP → PC：`display`**（设备周期性上报当前屏上状态，供 Dashboard 同步）

```json
{
  "type": "display",
  "status": "CODING",
  "source": "CURSOR",
  "gif": "CODING"
}
```

节流：`WS 连接后 400 ms` 内不上报；同类状态最小间隔 **120 ms**。

---

#### 5.2.3 语音上传（ESP → PC）

**流式（主路径）**

① text 开流：

```json
{
  "type": "audio_upload",
  "stream": true,
  "sample_rate": 16000,
  "channels": 1,
  "bit_depth": 16,
  "audio_len": 0
}
```

② PC 回 `session`（见 §5.2.4）

③ 多帧 **binary** PCM（每块优先 4096 B）

④ text 结束：

```json
{
  "type": "audio_end",
  "session_id": "a1b2c3d4e5f6",
  "total_bytes": 48000
}
```

过短片段取消（已开流）：

```json
{
  "type": "audio_end",
  "session_id": "a1b2c3d4e5f6",
  "total_bytes": 3200,
  "discard": true
}
```

**整段上传（回退）**

帧 1 text：

```json
{
  "type": "audio_upload",
  "sample_rate": 16000,
  "channels": 1,
  "bit_depth": 16,
  "audio_len": 48000
}
```

帧 2 **binary**：长度须等于 `audio_len`。

流式判定：`stream == true` **或** `audio_len == 0`。

---

#### 5.2.4 语音下行（PC → ESP）

**`session`**（`audio_upload` 后立即回复）

```json
{
  "type": "session",
  "session_id": "a1b2c3d4e5f6",
  "volume_percent": 33
}
```

`session_id` 为 12 位 hex。默认音量见 `backend/settings.json`（**33%**）。

**`status`（语音收尾）**

```json
{
  "type": "status",
  "status": "IDLE",
  "text": "",
  "source": "VOICE"
}
```

设备在 `WAIT_REPLY` / `PLAYING` 收到 `source=VOICE` 且 `status` 为 `IDLE`/`ERROR` 时结束本轮。

**TTS：`audio_chunk` + binary**

text 头：

```json
{
  "type": "audio_chunk",
  "session_id": "a1b2c3d4e5f6",
  "len": 4096,
  "end": false
}
```

紧跟 binary PCM（`len` 字节）。仅结束标记：

```json
{
  "type": "audio_chunk",
  "session_id": "a1b2c3d4e5f6",
  "len": 0,
  "end": true
}
```

**`config`**（连接时 / Dashboard 改音量或开关时推送）

```json
{
  "type": "config",
  "volume_percent": 90,
  "voice_enabled": true
}
```

`voice_enabled=false` 时后端拒绝新 `audio_upload` 并中止进行中的听流。

---

#### 5.2.5 设备配置（WiFi Profile，经 WS）

最多 **5** 条 profile；Dashboard `POST /api/device/wifi-profiles/*` 透传下列帧。

**PC → ESP：查询**

```json
{"type":"get_wifi_profiles"}
```

**ESP → PC：列表**

```json
{
  "type": "wifi_profiles",
  "ok": true,
  "count": 2,
  "active": 0,
  "profiles": [
    {
      "index": 0,
      "ssid": "MyWiFi",
      "password": "secret",
      "host": "192.168.1.100",
      "port": 8000,
      "ip": "192.168.1.50",
      "netmask": "255.255.255.0",
      "gateway": "192.168.1.1"
    }
  ]
}
```

失败示例：

```json
{
  "type": "wifi_profiles",
  "ok": false,
  "error": "最多保存 5 条",
  "count": 0,
  "active": 0,
  "profiles": []
}
```

**PC → ESP：保存**（`index` 省略或 `-1` 为追加；命中 `active` 槽位则自动 `net_apply_config`）

```json
{
  "type": "wifi_profile_save",
  "index": 0,
  "ssid": "MyWiFi",
  "password": "secret",
  "host": "192.168.1.100",
  "port": 8000,
  "ip": "",
  "netmask": "",
  "gateway": ""
}
```

**PC → ESP：删除 / 切换**

```json
{"type":"wifi_profile_delete","index":1}
```

```json
{"type":"wifi_profile_activate","index":0}
```

成功后设备再发一条 `wifi_profiles` 全量列表。

---

#### 5.2.6 诊断与错误

**PC → ESP：请求诊断**

```json
{"type":"debug"}
```

**ESP → PC：诊断回复**

```json
{
  "type": "debug",
  "dram_free": 180224,
  "dram_largest": 98304,
  "psram_free": 6123456,
  "psram_largest": 4194304,
  "rec_bytes": 12288,
  "rec_cap": 640000,
  "play_bytes": 0,
  "play_cap": 524288,
  "vad_phase": 0,
  "noise_rms": 0.0042,
  "noise_peak": 0.0310,
  "start_rms": 0.0147,
  "start_peak": 0.1085,
  "last_rms": 0.0021,
  "last_peak": 0.0180,
  "pdm_gain": 5.0
}
```

**PC → ESP：`error`**

```json
{"type":"error","detail":"voice pipeline busy"}
```

```json
{"type":"error","detail":"audio_len mismatch: expected 48000, got 40960"}
```

```json
{"type":"error","detail":"voice chat disabled"}
```

---

#### 5.2.7 Dashboard 专用（`role` ≠ `device`）

连接后 `ack` 同 §5.2.1。另接收设备转发的 `display`、`status`、`text`，以及：

```json
{
  "type": "transcript",
  "time": "2026-03-14 10:30:00",
  "text": "今天天气怎么样",
  "session_id": "a1b2c3d4e5f6",
  "role": "user"
}
```

```json
{
  "type": "backend_log",
  "time": "10:30:01",
  "level": "info",
  "text": "[voice] stream end session=…"
}
```

### 5.3 语音处理流水线

```text
audio_upload(stream) → VoiceSession + VoskStreamRecognizer
  → binary chunks → AcceptWaveform
  → audio_end → FinalResult
  → LLM 流式 → append 字幕
  → [VOICE_TTS] edge-tts → audio_chunk 泵送
  → IDLE (source=VOICE) 收尾
```

| 步骤 | 说明 |
|------|------|
| 过短 PCM | < 6400 B → `IDLE`「没听清，请再说一次」 |
| 忙控制 | `_voice_busy`；超时 `VOICE_BUSY_TIMEOUT_SEC=55` |
| TTS 泵送 | 每突发最多 **16384 B**，间隔 `AUDIO_SEND_INTERVAL_SEC=0.02` |
| 多轮 | `CHAT_CONTEXT_MAX_TURNS=8`，空闲 `CHAT_CONTEXT_IDLE_SEC=300` 清空 |
| Profile 轮询 | 多 profile 且未连上时，每 **15 s** 切换一条（`PROFILE_ROTATE_MS`） |

#### 环境变量（`backend/.env`）

| 变量 | 说明 | 默认 |
|------|------|------|
| `LLM_BASE_URL` / `LLM_API_KEY` / `LLM_MODEL` | LLM 初始种子（首次生成 `llm.json`） | 见 [§5.4](#54-llm-配置与对接) |
| `VOICE_TTS` | TTS 开关 | `0` |
| `TTS_VOICE` / `TTS_RATE` | edge-tts 参数 | `zh-CN-XiaoxiaoNeural` / `+20%` |
| `ASR_ENGINE` | 识别引擎 | `vosk` |
| `VOSK_MODEL_PATH` | Vosk 模型目录 | `backend/models/vosk-model-small-cn-0.22` |

#### 异常与并发

| 场景 | 行为 |
|------|------|
| WS 未连接 | VAD 不录音 |
| 流式开流失败 | 回退整段 `audio_upload` |
| WS 断开 | `voice_on_ws_lost()`；UI `OFFLINE`，保留 `agent_status` 缓存 |
| WS 重连 | `_sync_device_state` 恢复 Cursor/Pi 表情 |
| 后端忙 | `voice pipeline busy` |
| 播放中 | 停麦；异 `session_id` chunk 丢弃 |

可选 HTTP：`POST /voice/upload`、`GET /voice/audio/{id}`、`POST /api/volume`。

### 5.4 LLM 配置与对接

设备语音对话（ASR 识别完成后）会调用 **OpenAI 兼容的 Chat Completions** 接口（HTTP 流式，`stream: true`）。配置分两层：

| 文件 | 作用 | 是否入库 |
|------|------|----------|
| `backend/.env` | 首次启动时的**种子值**；无 `llm.json` 时据此生成「默认」profile | 否（gitignore） |
| `backend/llm.json` | **运行时实际生效**的配置；Dashboard 保存即写此文件，热更新无需重启 | 否（gitignore） |

日常推荐在 Dashboard 顶栏点 **「大模型配置」** 管理；也可先写 `.env` 再启动后端自动生成 `llm.json`。

#### 5.4.1 字段说明

每条 profile 含四个字段（与 Dashboard 表单一致）：

| 字段 | 必填 | 说明 |
|------|------|------|
| **名称** | 否 | 备注，如 `千问`、`2api 中转`；留空则用模型名 |
| **Base URL** | 是 | API 根地址，见 [URL 拼接规则](#542-base-url-拼接规则) |
| **API Key** | 是 | Bearer 令牌（`Authorization: Bearer <key>`） |
| **模型名称** | 是 | 服务商侧的 model id，如 `gpt-4o`、`qwen3.8-max` |

最多保存 **8** 条 profile，其中一条为 **当前启用**；语音流水线始终读启用项。

#### 5.4.2 Base URL 拼接规则

后端按 `llm.py` 中 `chat_completions_url()` 自动补全路径：

| 你填写的 Base URL | 实际 POST 地址 |
|-------------------|----------------|
| `https://2api.store` | `https://2api.store/v1/chat/completions` |
| `https://api.openai.com/v1` | `https://api.openai.com/v1/chat/completions` |
| `https://dashscope.aliyuncs.com/compatible-mode/v1` | `…/compatible-mode/v1/chat/completions` |
| 已含 `/chat/completions` 的完整 URL | 原样使用 |

填写时**不要**手动加 `/chat/completions`，只填服务商文档给出的 API 根或 `/v1` 前缀即可。

#### 5.4.3 配置方式

**方式 A：`.env` 种子（适合首次部署）**

```bash
cd backend
cp .env.example .env
# 编辑 .env，至少填写 LLM_API_KEY
```

```ini
LLM_BASE_URL=https://2api.store
LLM_API_KEY=sk-xxxxxxxx
LLM_MODEL=gpt-5.6-luna
```

启动 `start.bat` 后，若不存在 `llm.json`，会自动从 `.env` 生成一条名为「默认」的 profile 并写入 `llm.json`。

**方式 B：Dashboard 网页（适合日常修改 / 多模型切换）**

1. 浏览器打开 `http://<pc-ip>:8000`
2. 顶栏 → **大模型配置**
3. 点 **新增** 或已有条目旁的 **编辑**
4. 填写 Base URL、API Key、模型名称
5. 点 **保存并启用** — 立即生效，**无需重启后端**
6. 仅保存不切换：点 **保存**；切换已有条目：点 **启用**

**API Key 编辑注意**：列表与表单中 Key 为脱敏显示（前 4 + 后 4）。编辑已有条目时，**留空或只输入 `****` 表示保留原密钥**；要更换密钥需输入完整新 Key。

**方式 C：直接编辑 `llm.json`（高级）**

```json
{
  "active": "3b1ddf919139",
  "profiles": [
    {
      "id": "default",
      "name": "默认",
      "base_url": "https://2api.store",
      "api_key": "sk-xxxxxxxx",
      "model": "gpt-5.6-luna"
    },
    {
      "id": "3b1ddf919139",
      "name": "千问",
      "base_url": "https://dashscope.aliyuncs.com/compatible-mode/v1",
      "api_key": "sk-xxxxxxxx",
      "model": "qwen3.8-max"
    }
  ]
}
```

保存后下次 API 调用自动加载；也可在 Dashboard 点 **刷新列表** 同步到表单。

#### 5.4.4 常见服务商示例

| 场景 | Base URL | 模型名示例 | Key 获取 |
|------|----------|------------|----------|
| 2api 等中转站 | `https://2api.store` | 控制台所列模型 id | 中转站控制台 |
| OpenAI 官方 | `https://api.openai.com/v1` | `gpt-4o` | platform.openai.com |
| 阿里云百炼（千问兼容） | `https://dashscope.aliyuncs.com/compatible-mode/v1` | `qwen-plus`、`qwen3.8-max` | 百炼控制台 API-Key |
| DeepSeek | `https://api.deepseek.com` | `deepseek-chat` | platform.deepseek.com |
| 本地 Ollama | `http://127.0.0.1:11434/v1` | `llama3` 等本地模型名 | 通常可填占位 `ollama` |
| LM Studio | `http://127.0.0.1:1234/v1` | 本地加载的模型 id | 通常可填占位 |

接口须支持 **流式** Chat Completions；若返回 400 且与 `stream_options` 相关，后端会自动重试不带 usage 的流式请求。

#### 5.4.5 验证与排错

| 现象 | 处理 |
|------|------|
| 语音识别后无回复 / 日志 `LLM_API_KEY is empty` | 在 Dashboard **保存并启用** 一条带有效 Key 的 profile |
| HTTP 401 / 403 | 检查 Key 是否过期、Base URL 是否与服务商一致 |
| HTTP 404 | Base URL 多写了 `/chat/completions`，改回根或 `/v1` |
| 模型不存在 | 核对模型 id 与控制台可用列表 |
| 想确认当前生效项 | Dashboard **大模型配置** 中带「当前」标签的条目；或看后端启动日志 `[llm] loaded … active=…` |
| 查看历史请求 | Dashboard → **大模型对话历史**；落盘 `log/llm_chat.jsonl` |

REST：`GET /api/llm` 返回当前配置（Key 脱敏）；`POST /api/llm` 新增/更新；`POST /api/llm/activate` 切换启用；`POST /api/llm/delete` 删除（至少保留 1 条）。

### 5.5 Agent Hook 集成

Hook 装在**用户目录**，对所有仓库生效；**不要**在本仓库再放 `.cursor/hooks.json`。

| Agent | 路径 |
|-------|------|
| Pi | `%USERPROFILE%\.pi\agent\extensions\agent-display.ts` |
| Cursor | `%USERPROFILE%\.cursor\hooks.json`、`hooks\agent_display_hook.py` |

Cursor：写入 `event_queue.jsonl`，后端每 100 ms 清空；`source=CURSOR`。Pi：`source=PI`；30 s 无事件 → `STALE`。

#### 状态总表

| 状态 | 中文 | GIF | Pi | Cursor |
|------|------|-----|----|--------|
| IDLE | 空闲 | IDLE.gif | `session_start` | `sessionStart` |
| THINKING | 思考中 | THINKING.gif | `agent_start` / MCP | `beforeSubmitPrompt` / `afterAgentThought` |
| CODING | 编码中 | CODING.gif | `edit` / `write` | Write 类 `preToolUse` |
| READING | 读取中 | READING.gif | `read` | Read / Grep / Glob |
| TESTING | 测试中 | TESTING.gif | `bash` | Shell |
| WAITING | 等待中 | WAITING.gif | `agent_settled` | `stop` / AskQuestion |
| DONE | 已完成 | DONE.gif | `agent_end` | `afterAgentResponse` |
| ERROR | 错误 | ERROR.gif | 失败 `tool_result` | `postToolUseFailure` |
| OFFLINE | 离线（仅断网） | OFFLINE.gif | `session_shutdown`（设备忽略） | —（`sessionEnd` → IDLE） |
| STALE | 已过期 | STALE.gif | 30 s 无事件 | — |
| TOOL | 使用工具 | TOOL.gif | 工具调用 | `preToolUse` |
| EAR / SPEAKING | 收听/播报 | EAR / SPEAKING.gif | — | 语音 `voice_overlay` |

Cursor **不要**注册 `beforeAgentResponse`（3.x 非法事件名会导致 hooks 加载失败）。

---

## 六、构建与部署

### 6.1 依赖清单

#### 固件（ESP-IDF 5.4.2）

`main/idf_component.yml` 自动下载：

| 组件 | 版本 |
|------|------|
| `lvgl/lvgl` | 9.2.2 |
| `espressif/esp_websocket_client` | 1.8.0 |

#### PC 脚本

| 脚本 | 依赖 |
|------|------|
| `scripts/gen_frame_player.py` | Python 3 + Pillow |
| `scripts/gen_cjk_font.py` | Python 3 + Node.js + `lv_font_conv` |
| `scripts/flash_*.py` | esptool（IDF 自带或 pip） |

#### 后端 Python 3.10+

```bash
cd backend && pip install -r requirements.txt
```

| 包 | 用途 |
|----|------|
| `fastapi` / `uvicorn` | Web + WS |
| `httpx` | LLM API |
| `vosk` | 默认 ASR |
| `edge-tts` / `miniaudio` | TTS |
| `bleak` | 网页 BLE 配网 |

复制 `backend/.env.example` → `backend/.env`，填写 `LLM_API_KEY` 等；也可启动后在 Dashboard **大模型配置** 中填写（见 [§5.4](#54-llm-配置与对接)）。

### 6.2 构建与烧录

需 ESP-IDF 5.4.2。Git Bash 下 `export.sh` 可能失败，可用 `scripts/idf_build.bat`。详细环境见 [开发环境搭建.md](docs/开发环境搭建.md)。

```bat
scripts\idf_build.bat build
idf.py -p COMx flash
python scripts/flash_font.py -p COMx
python scripts/flash_animations.py -p COMx
idf.py -p COMx monitor
```

字库与表情分区可单独更新，不必每次全量 flash。

#### 预编译固件下载（免编译）

不想本地编译时，可直接下载三个分区镜像（**v1.0**，ESP-IDF 5.4.2）：

| 文件 | 分区偏移 | 直链 |
|------|----------|------|
| `esp32s3_agent_display.bin` | `0x10000` | [Releases](https://github.com/ATongHru/AgentDisplay/releases/download/v1.0/esp32s3_agent_display.bin) |
| `font_cjk_16.bin` | `0x400000` | [Releases](https://github.com/ATongHru/AgentDisplay/releases/download/v1.0/font_cjk_16.bin) |
| `animations.bin` | `0x600000` | [Releases](https://github.com/ATongHru/AgentDisplay/releases/download/v1.0/animations.bin) |

仓库内路径：`firmware/releases/v1.0/`（主程序）+ `firmware/data/`（字库、表情）。烧录步骤与 SHA256 见 [firmware/releases/v1.0/README.md](firmware/releases/v1.0/README.md)。

### 6.3 仓库结构

```text
esp32s3-agent-display/
├── README.md                  中文文档
├── README.en.md               English documentation
├── start.bat / stop.bat
├── partitions.csv
├── sdkconfig.defaults
├── docs/
│   ├── 开发环境搭建.md
│   └── dev-setup.en.md
├── backend/                 FastAPI + Dashboard + ASR/LLM + BLE
├── main/                    固件源码
├── scripts/                 构建、动画、字库、烧录
├── third_party/emoji-gif/   状态 GIF 源
├── third_party/fonts/       通用字表 + extra_symbols.txt
└── firmware/
    ├── data/                animations.bin / font_cjk_16.bin
    └── releases/v1.0/       预编译主程序 esp32s3_agent_display.bin
```

---

## 附录

### Emoji 来源

[Noto Emoji Animation](https://googlefonts.github.io/noto-emoji-animation/) → `third_party/emoji-gif/` → `gen_frame_player.py`。

### GIF 裁切

[SkyloongGIFsHelper](https://github.com/PuddingTower/SkyloongGIFsHelper) 按 90×90 裁切后放入 `third_party/emoji-gif/`。

### Vosk 中文模型

1. 从 [Vosk Models](https://alphacephei.com/vosk/models) 下载 **vosk-model-small-cn-0.22**
2. 解压到 `backend/models/vosk-model-small-cn-0.22/`
3. 或设置 `VOSK_MODEL_PATH`

模型约 40 MB；`backend/models/` 已 gitignore。

---

## 许可证

本项目**自有源码**采用 [MIT License](LICENSE)，版权归 [ATongHru](https://github.com/ATongHru)（床头指挥官）所有。

第三方依赖与资源（ESP-IDF、LVGL、Noto Emoji、Vosk、`edge-tts`、字库生成用 SimHei 等）的许可说明见 [NOTICE](NOTICE)。再分发固件或后端时，请一并保留 `LICENSE` 与 `NOTICE`，并遵守 CC BY 4.0 对表情资源的署名要求。
