# 预编译固件 v1.0

ESP-IDF **5.4.2** 构建，适用于 **ESP32-S3 N16R8**（16 MB Flash）+ 本仓库 `partitions.csv` 分区表。

## 文件清单

| 文件 | 分区 | 偏移 | 大小 | 说明 |
|------|------|------|------|------|
| [esp32s3_agent_display.bin](esp32s3_agent_display.bin) | `app0` | `0x10000` | ~1.5 MB | 主程序固件 |
| [font_cjk_16.bin](../../data/font_cjk_16.bin) | `cjk_font` | `0x400000` | ~893 KB | 16px 中文字库 |
| [animations.bin](../../data/animations.bin) | `animations` | `0x600000` | ~4.4 MB | 离线 RLE 表情 |

SHA256：

```
esp32s3_agent_display.bin  4b7583b6e499628cd3c09cfc46878e017d8e8f429f59dd74ac1bbbfb56df4494
font_cjk_16.bin              27bb0dd61205ebf4b9c6ba900ee9e71d838120d86e4aebb9c09fb832e944186a
animations.bin               53d99d0844fc6f2317e79d4887a9f94d5aaf8c9bb066a20075e4d321daf5109e
```

也可从 [GitHub Releases](https://github.com/ATongHru/AgentDisplay/releases/tag/v1.0) 下载三个 `.bin` 的打包附件。

## 烧录方式

### 方式 A：已有 ESP-IDF 环境（推荐）

在仓库根目录，将 `COMx` 换成实际串口：

```bat
idf.py -p COMx flash
python scripts\flash_font.py -p COMx
python scripts\flash_animations.py -p COMx
```

`idf.py flash` 会写入 bootloader、分区表与主程序；字库与表情需单独烧录（上两行）。

### 方式 B：仅 esptool（已有 bootloader / 分区表）

若设备此前已用本仓库完整烧录过，可只更新对应分区：

```bash
esptool.py --chip esp32s3 -p COMx -b 921600 write_flash 0x10000 esp32s3_agent_display.bin
esptool.py --chip esp32s3 -p COMx -b 921600 write_flash 0x400000 font_cjk_16.bin
esptool.py --chip esp32s3 -p COMx -b 921600 write_flash 0x600000 animations.bin
```

**全新板子**须先执行一次 `idf.py flash`（或烧录 `build/bootloader/bootloader.bin` + `build/partition_table/partition-table.bin`），再烧上述三个文件。

### 方式 C：GitHub Releases 直链

```
https://github.com/ATongHru/AgentDisplay/releases/download/v1.0/esp32s3_agent_display.bin
https://github.com/ATongHru/AgentDisplay/releases/download/v1.0/font_cjk_16.bin
https://github.com/ATongHru/AgentDisplay/releases/download/v1.0/animations.bin
```
