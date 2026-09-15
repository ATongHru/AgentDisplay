# ESP32-S3 Agent Display

**中文** | [English](README.en.md) · [开发环境搭建](docs/开发环境搭建.md) · [预编译固件 v1.1](firmware/releases/v1.1/README.md)

面向 **ESP32-S3 N16R8（16 MB Flash / 8 MB PSRAM）** 的桌面 Agent 状态屏。设备通过 WebSocket 接收 Agent 状态，并提供离线表情、中文 UI、语音交互与 BLE / AP 配网；PC 端 FastAPI 服务提供 Dashboard、ASR、LLM 与 TTS。

| 层级 | 当前实现 |
|---|---|
| 固件 | ESP-IDF 5.4.2、FreeRTOS、LVGL 9.2.2 |
| 显示 / 动画 | 240×240 ST7789 SPI；90×90 RGB565 RLE 离线表情 |
| 网络 | Wi-Fi STA + WebSocket；可选 token 鉴权 |
| 音频 | 16 kHz / mono / s16le PDM 输入与 I2S PCM 播放 |
| 后端 | FastAPI / uvicorn、Vosk 或 SpeechRecognition、OpenAI 兼容 LLM、edge-tts / Windows SAPI |

> 本 README 已按当前源码、`partitions.csv` 和 v1.1 构建产物校对。凭证、模型和本机配置不包含在仓库或固件中。

## 硬件与接线

| 部件 | 规格 / 引脚 |
|---|---|
| 主控 | ESP32-S3 N16R8，双核 240 MHz，16 MB Flash，8 MB OPI PSRAM |
| 显示 | ST7789 240×240：CS GPIO8、DC GPIO9、RST GPIO10、MOSI GPIO11、SCLK GPIO12 |
| I2S 功放 | DIN GPIO5、WS GPIO6、BCLK GPIO7；建议功放 5V 供电并与 ESP 共地 |
| PDM 麦克风 | CLK GPIO15、DATA GPIO16 |
| 按键 | BOOT / GPIO0 |
| 控制台 | USB Serial/JTAG（GPIO19 / GPIO20） |

GPIO26–32 为 OPI Flash / PSRAM，GPIO45 / 46 为 strapping，GPIO48 为板载 RGB LED；请勿复用。PDM DATA 不应接 GPIO3 或 GPIO48。

### BOOT 键

启动后前 2 秒忽略按键。短按之间以“释放到下一次按下”小于 600 ms 计为同一序列；最后一次释放后约 600 ms 执行，借此区分双击与三击：

| 操作 | 结果 |
|---|---|
| 短按 2 下 | 启动 Soft AP 配网 |
| 短按 3 下 | 启动 BLE 配网 |
| 长按至少 3 秒 | 启动 BLE 配网（保留快捷入口） |
| 长按至少 5 秒 | 显示运行时诊断覆盖层 |

BLE 与 AP 互斥；AP 暂停 STA，BLE 会释放 Wi-Fi 资源以给 NimBLE 留出内存。

## 快速开始

### 1. 启动 PC 后端

Windows 下运行 `start.bat`，然后访问 <http://127.0.0.1:8000/>。首次使用：

```bat
copy backend\.env.example backend\.env
python -m pip install -r backend\requirements.txt
start.bat
```

默认仅监听 `127.0.0.1`。设备需访问 PC 时，设置 `AGENT_BIND_HOST=0.0.0.0`，并同时设置随机的 `AGENT_API_TOKEN`；配网时填入相同 token。Dashboard 可管理 LLM、音量、TTS、语音开关、BLE 配网和已连接设备的 Wi-Fi profiles。

Vosk 模型放在 `backend/models/`，并用 `ASR_ENGINE` 选择；模型不随项目分发。

### 2. 配置设备

设备配置保存在 NVS，优先于 menuconfig 出厂默认值；最多支持 5 条 Wi-Fi / 后端 profile。

- **AP**：双击 BOOT，连接 `AgentDisplay-XXXX`，打开 <http://192.168.4.1/>，填写 Wi-Fi、后端地址、可选 token 和静态 IP。
- **BLE**：三击或长按 BOOT，使用 Dashboard 的 BLE 卡片，或向 `AgentDisplay` 的 NUS 服务逐行发送：

```text
WIFI:<ssid>,<password>
HOST:<ip>:<port>
TOKEN:<api_token>       # 可选
IP:<x.x.x.x>            # 可选静态 IP
MASK:<x.x.x.x>
GW:<x.x.x.x>
APPLY
```

- **串口**：115200 baud；`ap`、`ap_stop`、`ble_stop`、`help` 可用于调试。

设备连上后会发送 `hello`，后端回 `ack` 与当前音量。Agent Hook / Dashboard 的状态走 `/ws`；PDM 麦克风经 VAD 自动录音，PCM 分块上传，由后端完成 ASR → LLM → 可选 TTS。

## 网络与恢复策略

Wi-Fi、WebSocket 与 profile 切换分开处理：

1. Wi-Fi 断开后按 **5 → 10 → 20 → 40 → 60 秒**退避调用 `esp_wifi_connect()`；获取 IP 后重置为 5 秒。
2. Wi-Fi 正常而后端不可达时，WebSocket 每 **3 秒**重试；单次连接最多等待 **3 秒**。
3. 同一 profile 出现 **两次已完成的 WS 失败**且没有语音网络会话时，才切换到下一条 profile。主动切换造成的断开不计失败。
4. 冷启动从未成功建立 WS 时，30 秒无 Wi-Fi+WS 自动启动 AP；BLE/AP 应用配置后，AP 兜底延后到 60 秒。

任务看门狗为 5 秒，仅作为真实死锁的最后恢复手段。现有策略避免在连接进行中同步停止 WS 而造成无谓重启。

## 后端与鉴权

| 项 | 默认 / 行为 |
|---|---|
| Dashboard | `http://127.0.0.1:8000/` |
| WebSocket | `ws://<pc-ip>:8000/ws` |
| LAN 鉴权 | `AGENT_API_TOKEN`；HTTP 用 `Authorization: Bearer` 或 `X-Api-Token`，WS 用 `?token=` |
| TTS | `VOICE_TTS=0` 默认只显示字幕；设为 `1` 后生成 PCM 语音 |
| 状态接口 | `/health`、`/api/status`、`/api/logs`、`/api/history` |

局域网监听不设 token 会产生安全警告。不要提交 `backend/.env`、`backend/llm.json`、`backend/settings.json`、Vosk 模型或 API key。

## 构建、烧录与发布固件

在已激活 ESP-IDF 5.4.2 的 shell 中：

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash
python scripts/flash_font.py -p COMx
python scripts/flash_animations.py -p COMx
```

`idf.py flash` 只写 bootloader、分区表、OTA data 和 app；字体与动画是独立分区，需单独烧录。Windows 环境见[开发环境搭建](docs/开发环境搭建.md)。

v1.1 为 ESP-IDF 5.4.2 构建，适用于本项目的 **16 MB N16R8 分区表**，不是通用 ESP32-S3 固件。完整资产、SHA-256 和 esptool 命令见 [firmware/releases/v1.1/README.md](firmware/releases/v1.1/README.md)。

| 分区 | 偏移 | 内容 |
|---|---:|---|
| bootloader | `0x0000` | 引导程序 |
| partition table | `0x8000` | 本项目 16 MB 分区表 |
| otadata | `0xE000` | OTA 初始数据 |
| app0 | `0x10000` | 主程序 |
| cjk_font | `0x610000` | `font_cjk_16.bin` |
| animations | `0x810000` | `animations.bin` |

更新 app 不会擦除字体或动画分区。

## 架构与仓库布局

```text
ESP32 ── WebSocket ── FastAPI backend ── ASR / LLM / TTS
  │                         │
  ├─ net_task (Core 0)      └─ Dashboard / Hook bridge
  ├─ audio_task (Core 0)
  ├─ lvgl_task (Core 1)     ← 唯一可调用 LVGL 的任务
  └─ app_task / btn_boot (Core 1)
```

| 路径 | 内容 |
|---|---|
| `main/` | ESP-IDF 固件：显示、音频、语音、Wi-Fi/WS、BLE/AP、UI |
| `backend/` | FastAPI、Dashboard、WebSocket hub、ASR/LLM/TTS、Hook bridge |
| `firmware/data/` | 字体与 RLE 动画资源 |
| `firmware/releases/` | 预编译发布物与校验和 |
| `scripts/` | 字体/动画生成与烧录工具 |
| `tests/` | 后端单元测试 |

音频和网络运行在 Core 0；LVGL 只能由 `lvgl_task` 访问；跨任务 UI 更新必须经过队列；大 PCM 和字体缓冲置于 PSRAM。

## 排障

| 现象 | 检查方式 |
|---|---|
| Wi-Fi 已连接但 WS 离线 | 检查 PC LAN IP、`AGENT_BIND_HOST`、防火墙、token；串口会输出连接/重试原因 |
| 屏幕资源缺失 | 分别烧录 `firmware/data/font_cjk_16.bin` 与 `firmware/data/animations.bin` |
| AP 未出现 | 双击须在启动保护期后完成；自动 AP 只在从未连过 WS 时触发 |
| BLE 无法启动 | 停止 AP，确认 N16R8 带 PSRAM，并查看串口内存日志 |
| 后端无法访问 | 运行 `start.bat` 后访问 `/health`；设备必须使用 PC 的 LAN IP，不能使用 `127.0.0.1` |

后端验证：`PYTHONPATH=backend python -m pytest -q`。固件验证：`idf.py build`。
