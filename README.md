# wifi_esp32

ESP32-S3 WiFi STA + USB NCM 网络共享 + Web 管理页面

## 功能

- **WiFi STA 模式**：连接上游 WiFi 热点
- **USB NCM 网卡**：通过 USB 与 Windows 共享网络（NAPT 转发）
- **Web 管理页面**：配置 WiFi SSID/密码、管理 DHCP、查看状态
- **USB 网卡热插拔**：Windows 禁用/启用 USB 网卡后自动重连

## 硬件

- ESP32-S3 (QFN56 rev v0.2)
- PSRAM: 8MB | Flash: 2MB

## 构建

```bash
# 首次设置 ESP-IDF 环境
# Windows: . $IDF_PATH/export.ps1
# Linux/Mac: . $IDF_PATH/export.sh

idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## 依赖

通过 `idf.py reconfigure` 自动安装的 managed_components：
- `espressif/esp_tinyusb` (USB CDC/NCM)
- `espressif/tinyusb` (USB 协议栈)
