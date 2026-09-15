# 预编译固件 v1.1

适用于本仓库的 **ESP32-S3 N16R8（16 MB Flash / 8 MB PSRAM）** 与 `partitions.csv`。使用 ESP-IDF **5.4.2** 构建。该发布包含 Wi-Fi / WebSocket 恢复修复，以及 BOOT 双击 AP、三击 BLE 配网。

## 文件与校验和

| 文件 | Flash 偏移 | SHA-256 |
|---|---:|---|
| `bootloader.bin` | `0x0000` | `665cbdc57e7bf93d3cee3fd23a1573157e4d7067442353615eb5d104e436b093` |
| `partition-table.bin` | `0x8000` | `2bc63c733461e435943ce9f9b3cd648727de2fe6594eb48407fcded477aa9484` |
| `ota_data_initial.bin` | `0xE000` | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |
| `esp32s3_agent_display.bin` | `0x10000` | `9e6967e0f916dc0dbd2ed58712838505296f13761acb14a3b73d96305d80d0b4` |
| [`font_cjk_16.bin`](../../data/font_cjk_16.bin) | `0x610000` | `27bb0dd61205ebf4b9c6ba900ee9e71d838120d86e4aebb9c09fb832e944186a` |
| [`animations.bin`](../../data/animations.bin) | `0x810000` | `53d99d0844fc6f2317e79d4887a9f94d5aaf8c9bb066a20075e4d321daf5109e` |

## 全新板完整烧录

在本目录执行；将 `COMx` 改成真实串口：

```bash
esptool.py --chip esp32s3 --port COMx --baud 460800 write_flash \
  0x0000 bootloader.bin \
  0x8000 partition-table.bin \
  0xE000 ota_data_initial.bin \
  0x10000 esp32s3_agent_display.bin \
  0x610000 ../../data/font_cjk_16.bin \
  0x810000 ../../data/animations.bin
```

已有本项目分区表与资源时，可只写 `0x10000 esp32s3_agent_display.bin`。这不会擦除字库、动画或 `storage` 分区。

GitHub Release 附件提供同名的 bootloader、分区表、OTA data、app、字库和动画文件；请在烧录前验证 SHA-256。
