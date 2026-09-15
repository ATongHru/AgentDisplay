# ESP32-S3 Agent Display

[中文](README.md) | **English** · [Development setup](docs/dev-setup.en.md) · [Prebuilt firmware v1.1](firmware/releases/v1.1/README.md)

A desktop Agent status display for **ESP32-S3 N16R8 (16 MB Flash / 8 MB PSRAM)**. The device receives Agent status over WebSocket and provides offline emoji, CJK UI, voice interaction, and BLE / AP provisioning. A PC-side FastAPI service supplies the Dashboard, ASR, LLM, and TTS pipeline.

| Layer | Current implementation |
|---|---|
| Firmware | ESP-IDF 5.4.2, FreeRTOS, LVGL 9.2.2 |
| Display / emoji | 240×240 ST7789 SPI; 90×90 RGB565 RLE offline emoji |
| Network | Wi-Fi STA + WebSocket; optional token authentication |
| Audio | 16 kHz / mono / s16le PDM input and I2S PCM playback |
| Backend | FastAPI / uvicorn, Vosk or SpeechRecognition, OpenAI-compatible LLM, edge-tts / Windows SAPI |

> This README is checked against the current source, `partitions.csv`, and v1.1 build artifacts. Credentials, models, and machine-local configuration are not included in the repository or firmware.

## Hardware

| Part | Specification / pins |
|---|---|
| Controller | ESP32-S3 N16R8, dual-core 240 MHz, 16 MB Flash, 8 MB OPI PSRAM |
| Display | ST7789 240×240: CS GPIO8, DC GPIO9, RST GPIO10, MOSI GPIO11, SCLK GPIO12 |
| I2S amplifier | DIN GPIO5, WS GPIO6, BCLK GPIO7; 5 V supply and common ground are recommended |
| PDM microphone | CLK GPIO15, DATA GPIO16 |
| Button | BOOT / GPIO0 |
| Console | USB Serial/JTAG (GPIO19 / GPIO20) |

GPIO26–32 are used by OPI Flash / PSRAM; GPIO45 / 46 are strapping pins; GPIO48 drives the on-board RGB LED. Do not reuse them. Do not put PDM DATA on GPIO3 or GPIO48.

### BOOT button

The button is ignored for the first two seconds after boot. Short presses belong to one sequence when the interval from a release to the next press is under 600 ms; the action runs about 600 ms after the final release:

| Action | Result |
|---|---|
| Two short presses | Start Soft AP provisioning |
| Three short presses | Start BLE provisioning |
| Hold for at least 3 seconds | Start BLE provisioning (retained shortcut) |
| Hold for at least 5 seconds | Show the runtime diagnostics overlay |

BLE and AP provisioning are mutually exclusive. AP pauses STA; BLE releases Wi-Fi resources for NimBLE.

## Quick start

### 1. Start the PC backend

On Windows, run `start.bat` and then open <http://127.0.0.1:8000/>. For a first setup:

```bat
copy backend\.env.example backend\.env
python -m pip install -r backend\requirements.txt
start.bat
```

The default `backend/.env` binds to `127.0.0.1` only. To serve an ESP over LAN, set `AGENT_BIND_HOST=0.0.0.0` and a strong `AGENT_API_TOKEN`, then provision the same token into the device. The Dashboard manages LLMs, volume, TTS, voice enablement, BLE provisioning, and Wi-Fi profiles of connected devices.

For Vosk, put a compatible model below `backend/models/` and select it with `ASR_ENGINE`. Models are not distributed with the project.

### 2. Provision the device

Configuration is stored in NVS and overrides menuconfig factory defaults. Up to five Wi-Fi / backend profiles are supported.

- **AP**: double-press BOOT, connect to `AgentDisplay-XXXX`, open <http://192.168.4.1/>, and enter Wi-Fi, backend address, optional token, and optional static IP.
- **BLE**: triple-press or hold BOOT, use the Dashboard BLE card, or send newline-terminated commands to the `AgentDisplay` NUS service:

```text
WIFI:<ssid>,<password>
HOST:<ip>:<port>
TOKEN:<api_token>       # optional
IP:<x.x.x.x>            # optional static IP
MASK:<x.x.x.x>
GW:<x.x.x.x>
APPLY
```

- **Serial**: use 115200 baud; `ap`, `ap_stop`, `ble_stop`, and `help` are debug commands.

After connecting, the device sends `hello`; the backend returns `ack` and the current volume configuration. Agent Hooks and the Dashboard send status through `/ws`. The PDM microphone uses VAD to record automatically; PCM is uploaded in chunks and the backend performs ASR → LLM → optional TTS. The ESP only plays returned PCM.

## Network recovery

Wi-Fi, WebSocket, and profile fallback are separate paths:

1. After Wi-Fi disconnect, `esp_wifi_connect()` uses **5 → 10 → 20 → 40 → 60 seconds** exponential backoff. Receiving an IP resets the delay to five seconds.
2. When Wi-Fi works but the backend is unavailable, the WebSocket client retries every **3 seconds**; one connection attempt waits at most **3 seconds**.
3. The next saved profile is selected only after **two completed WS failures** on the current profile and only when no voice network session is active. A planned disconnect while switching profiles is not counted as a failure.
4. On a cold boot that has never made a WS connection, 30 seconds without Wi-Fi+WS starts AP provisioning. After applying BLE/AP configuration, this AP fallback waits 60 seconds.

The task watchdog remains a five-second last-resort recovery for genuine deadlocks. The current path avoids stopping a WebSocket synchronously during an in-flight connection.

## Backend and authentication

| Item | Default / behavior |
|---|---|
| Dashboard | `http://127.0.0.1:8000/` |
| WebSocket | `ws://<pc-ip>:8000/ws` |
| LAN authentication | `AGENT_API_TOKEN`; HTTP uses `Authorization: Bearer` or `X-Api-Token`, WS uses `?token=` |
| TTS | `VOICE_TTS=0` shows captions only; `1` generates PCM speech |
| Status endpoints | `/health`, `/api/status`, `/api/logs`, `/api/history` |

Binding the backend to LAN without a token creates a security warning. Never commit `backend/.env`, `backend/llm.json`, `backend/settings.json`, Vosk models, or API keys.

## Build, flash, and prebuilt firmware

With ESP-IDF 5.4.2 activated:

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash
python scripts/flash_font.py -p COMx
python scripts/flash_animations.py -p COMx
```

`idf.py flash` writes bootloader, partition table, OTA data, and the app only. Font and animation are independent partitions and must be flashed separately. See [development setup](docs/dev-setup.en.md) for Windows details.

v1.1 is built with ESP-IDF 5.4.2 for this project's **16 MB N16R8 partition table**; it is not generic ESP32-S3 firmware. See [firmware/releases/v1.1/README.md](firmware/releases/v1.1/README.md) for complete assets, SHA-256 checksums, and esptool commands.

| Partition | Offset | Contents |
|---|---:|---|
| bootloader | `0x0000` | bootloader |
| partition table | `0x8000` | this project's 16 MB table |
| otadata | `0xE000` | initial OTA data |
| app0 | `0x10000` | application |
| cjk_font | `0x610000` | `font_cjk_16.bin` |
| animations | `0x810000` | `animations.bin` |

Updating only the app does not erase the font or animation partitions.

## Architecture and layout

```text
ESP32 ── WebSocket ── FastAPI backend ── ASR / LLM / TTS
  │                         │
  ├─ net_task (Core 0)      └─ Dashboard / Hook bridge
  ├─ audio_task (Core 0)
  ├─ lvgl_task (Core 1)     ← the only task allowed to call LVGL
  └─ app_task / btn_boot (Core 1)
```

| Path | Contents |
|---|---|
| `main/` | ESP-IDF firmware: display, audio, voice, Wi-Fi/WS, BLE/AP, UI |
| `backend/` | FastAPI, Dashboard, WS hub, ASR/LLM/TTS, Hook bridge |
| `firmware/data/` | Font and RLE animation assets |
| `firmware/releases/` | Prebuilt release artifacts and checksums |
| `scripts/` | Font/animation generation and flash tools |
| `tests/` | Backend unit tests |

Audio and networking run on Core 0; LVGL may only be used by `lvgl_task`; UI work crosses tasks through a queue; large PCM and font buffers live in PSRAM.

## Troubleshooting

| Symptom | What to check |
|---|---|
| Wi-Fi is connected but WS is offline | PC LAN IP, `AGENT_BIND_HOST`, firewall, and token; serial logs include connection/retry reasons |
| Missing display resources | Flash `firmware/data/font_cjk_16.bin` and `firmware/data/animations.bin` separately |
| AP does not appear | Double-press after the boot guard; automatic AP only runs before the first successful WS connection |
| BLE does not start | Stop AP, use an S3 N16R8 with PSRAM, and inspect serial memory logs |
| Backend is unreachable | Run `start.bat`, check `/health`, and use the PC LAN IP rather than `127.0.0.1` from the ESP |

Backend verification: `PYTHONPATH=backend python -m pytest -q`. Firmware verification: `idf.py build`.
