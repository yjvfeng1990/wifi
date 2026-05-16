# ESP32-S3 WiFi 路由器

ESP32-S3 实现三接口网络共享，支持 STA/AP/USB 同时运行，STA 作为上游，AP 和 USB 作为下游，通过 NAPT 共享上网。

## 功能特性

| 接口 | 角色 | 说明 |
|------|------|------|
| **WiFi STA** | 上游 (WAN) | 连接上级 WiFi 热点，走 NAPT 转发 |
| **WiFi AP** | 下游 (LAN) | 热点供手机/电脑接入，DHCP+DNS 自动下发 |
| **USB NCM** | 下游 (LAN) | Windows USB 网卡，DHCP+DNS 自动下发 |

- **NAPT 共享上网**：AP 和 USB 客户端通过 STA 共享网络
- **双模共存**：STA + AP 同时运行，互不影响
- **Web 管理页面**：配置 SSID/密码、扫描附近 WiFi、查看实时吞吐、切换模式
- **WiFi 扫描**：异步后台扫描，轮询获取结果，按信号强度排序，点击自动填入 SSID
- **DHCP 客户端列表**：Web 页面显示 AP 和 USB 下挂设备的 MAC 地址与 IP
- **热插拔**：USB 断开/重连自动恢复 DHCP
- **DNS 转发**：AP 和 USB 客户端通过 DHCP 自动获取 DNS 服务器

## 吞吐监控

Web 页面实时显示 6 个方向的速度指标（单位：Bytes/s）：

| 字段 | 含义 |
|------|------|
| `sta_down_bps` | STA Download — 上游收到（用户下载） |
| `sta_up_bps` | STA Upload — 上游发送（用户上传） |
| `ap_down_bps` | AP Download — AP 侧下行 |
| `ap_up_bps` | AP Upload — AP 侧上行 |
| `usb_down_bps` | USB Download — USB 侧下行 |
| `usb_up_bps` | USB Upload — USB 侧上行 |

## 硬件

- ESP32-S3 (QFN56 rev v0.2)
- PSRAM: 8MB | Flash: 16MB
- WiFi + BLE 5

## 快速开始

```bash
# 1. 设置 ESP-IDF 环境
. C:\ESP-IDF\v6.0.1\esp-idf\export.ps1

# 2. 编译
idf.py build

# 3. 烧录
python -m esptool --chip esp32s3 -p COM2 -b 460800 --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size detect --flash-freq 80m \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/wifi_esp32.bin
```

## 依赖

通过 `idf.py reconfigure` 自动安装的 managed_components：
- `espressif/esp_tinyusb` — USB CDC/NCM 协议栈
- `espressif/tinyusb` — TinyUSB 核心库

## 配置项

| 项 | 默认值 | 说明 |
|----|--------|------|
| AP SSID | `ESP32-S3-Config` | AP 热点名称 |
| AP Password | `12345678` | AP 密码（至少8位） |
| AP Channel | `1` | WiFi 信道 |
| AP DHCP Lease | `3600s` | DHCP 租约时间 |
| AP DNS | `8.8.8.8` | 下发给客户端的 DNS 服务器 |
| USB IP | `192.168.5.1/24` | USB 网卡地址段 |

## Web API

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/wifi/status` | WiFi 状态（含吞吐数据） |
| POST | `/api/wifi/connect` | STA 连接指定 WiFi |
| POST | `/api/wifi/mode` | 切换 STA/AP/APSTA 模式 |
| POST | `/api/wifi/ap/start` | 启动 AP 热点 |
| POST | `/api/wifi/ap/stop` | 停止 AP 热点 |
| GET | `/api/wifi/scan` | 异步扫描：首次返回 `{"scanning":true}`，轮询至扫描完成后返回结果 JSON |
| GET | `/api/dhcp/clients` | DHCP 客户端列表 |
| POST | `/api/restart` | 重启设备 |

## 架构

```
手机/PC
   │
   ├─ WiFi AP (192.168.4.x) ──┐
   │                            │
   └─ USB NCM (192.168.5.x) ────┼── NAPT ── WiFi STA ── 上游路由器
                                │           │
                                └───────────┘
                                   (IP forwarding + NAPT)
```

## 已知问题与修复历史

### WiFi 扫描异步架构（v5）

扫描从同步阻塞改为异步轮询模式：

1. 用户点击扫描 → 前端调用 `/api/wifi/scan` → 后端启动独立 FreeRTOS task → 立即返回 `{"scanning":true}`
2. 前端每 500ms 轮询 `/api/wifi/scan` → 扫描完成后返回结果 JSON
3. 页面显示所有 AP，点击自动填入 SSID 输入框

**旧同步方案的致命问题**：`esp_wifi_scan_start(block=true)` 阻塞 HTTP 处理线程 3-5 秒，期间 WiFi 射频跳频导致 STA 连接的 TCP 中断，浏览器收不到响应 → 显示 "No networks found"。

### WIFI_EVENT_SCAN_DONE 结果竞争

之前的代码在 `wifi_service.cpp` 的 WiFi 事件处理器中注册了 `WIFI_EVENT_SCAN_DONE` → `collect_scan_results()`，会抢先消费扫描结果。当 web_server 的异步 scan task 醒来后，`esp_wifi_scan_get_ap_records()` 返回 0 — 所有结果已被事件处理器拿走。修复方法：移除该事件处理分支，让 scan task 独占结果。

### SSID 特殊字符与 HTML 属性注入

SSID 可能包含 `"`、`&`、`'` 和不可见控制字符，直接拼入 HTML `onclick` 属性会破坏 DOM。

- **后端**：`json_escape_ssid()` 函数处理 `"` `\` `\b` `\f` `\n` `\r` `\t` 及 ASCII < 0x20（`\u00XX` 编码）
- **前端**：SSID 通过 `data-ssid` HTML 属性传递（`"`→`&quot;`），onclick 用 `this.getAttribute('data-ssid')` 取出，避免字符串注入

### updateStatus 覆盖表单字段

`updateStatus()` 每 2 秒轮询 WiFi 状态，之前无条件覆盖 STA/AP 的 SSID 和密码输入框。修复：仅在输入框为空（`!el.value`）时才自动填充，用户手动输入或扫描选中后不受影响。

### 切换 WiFi 重连

切换 SSID 时新增 `esp_wifi_scan_stop()` + `esp_wifi_disconnect()` 两步前置操作，确保无扫描竞争且旧连接完全断开后再设新配置 + 连接。

### HTTPD URI 路由表溢出

`HTTPD_DEFAULT_CONFIG()` 默认 `max_uri_handlers = 8`。当注册超过 8 个路由时，`httpd_register_uri_handler()` 静默失败。手动增大至 `config.max_uri_handlers = 16`。

### DHCP DNS 选项格式（ESP-IDF v6.0.1）

`esp_netif_dhcps_option(ESP_NETIF_DOMAIN_NAME_SERVER)` 的参数类型为 `uint8_t` 标志位（`0x02`），不是 DNS IP 地址。正确用法两步：

```c
// 1. 启用 DNS 选项（标志位）
uint8_t dns_enable = 0x02;
esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET,
    ESP_NETIF_DOMAIN_NAME_SERVER, &dns_enable, sizeof(dns_enable));

// 2. 设置实际 DNS IP
esp_netif_dns_info_t dns;
IP4_ADDR(&dns.ip.u_addr.ip4, 8, 8, 8, 8);
esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
```

必须在 `esp_netif_dhcps_stop()` 和 `esp_netif_dhcps_start()` 之间执行。

### NAPT 线程安全

`ip_napt_enable_netif()` 必须通过 `tcpip_callback()` 在 lwIP TCP/IP 线程中调用，直接从事件处理线程调用会导致 NAT 表损坏。

### double-free 崩溃

`esp_netif_receive` 失败后内部已释放 buffer，再手动 `free(buf_copy)` 导致 double-free。修复：移除回调中的 `free(buf_copy)`。

## Web 管理页面

![Web管理页面截图](https://github.com/user-attachments/assets/ff37252e-347e-4b3d-bf65-308be6c9870e)