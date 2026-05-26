# ESP32-S3 WiFi 路由器

ESP32-S3 实现三接口网络共享，支持 STA/AP/USB 同时运行，STA 作为上游，AP 和 USB 作为下游，通过 NAPT 共享上网。

## 功能特性

| 接口 | 角色 | 说明 |
|------|------|------|
| **WiFi STA** | 上游 (WAN) | 连接上级 WiFi 热点，走 NAPT 转发 |
| **WiFi AP** | 下游 (LAN) | 热点供手机/电脑接入，DHCP+DNS 自动下发 |
| **USB NCM** | 下游 (LAN) | Windows USB 网卡，DHCP+DNS 自动下发 |

- **NAPT 共享上网**：AP 和 USB 客户端通过 STA 共享网络
- **AP NAPT**：STA 连接 WiFi 后自动开启，STA 断开自动关闭，无独立开关
- **USB NAPT**：通过 Web API `/api/usb/napt` 独立控制（默认关闭），NVS 持久化
- **DHCP DNS**：USB 和 AP DHCP 服务器自动下发 `8.8.8.8` 作为 DNS
- **双模共存**：STA + AP 同时运行，互不影响
- **Web 管理页面**：配置 SSID/密码、扫描附近 WiFi、查看实时吞吐、切换模式
- **WiFi 扫描**：异步后台扫描，轮询获取结果，按信号强度排序，点击自动填入 SSID
- **DHCP 客户端列表**：Web 页面显示 AP 和 USB 下挂设备的 MAC 地址与 IP
- **热插拔**：USB 断开/重连自动恢复 DHCP
- **DNS 转发**：AP 和 USB 客户端通过 DHCP 自动获取 DNS 服务器
- **ESP-NOW 通信**：低延迟点对点无线通信，支持广播/单播，无需 TCP/IP 栈
- **BLE 配对**：通过蓝牙自动发现设备并交换 ESP-NOW MAC 地址，无需手动输入

## ESP-NOW

ESP-NOW 是乐鑫私有协议，可在 WiFi 射频上实现点对点高速通信（最高 ~1 Mbps），无需 AP，延迟极低，适合传感器数据透传、远程控制等场景。

### 配对流程

两个 ESP32-S3 节点配对无需手动输入 MAC 地址：

```
节点 A（开启 BLE 广播）          节点 B（扫描并配对）
┌──────────────────────┐      ┌──────────────────────┐
│ BLE 广播携带：        │      │ BLE 扫描发现 A       │
│  - 'EN' 标识          │ ───→ │ 提取 ESP-NOW MAC     │
│  - ESP-NOW MAC (6B)   │      │ 显示设备名/RSSI      │
│  - 设备名称           │      │                      │
└──────────────────────┘      │ 用户点击 "Pair"      │
                               │  → 添加到 peer 列表  │
                               │  → 保存到 Flash NVS  │
                               └──────────────────────┘
```

- 设备 A 在 Web 页面设置设备名称，点击 **Start BLE**，开启 BLE 广播
- 设备 B 点击 **Scan BLE Devices**，扫描周围设备
- 发现设备后，点击 **Pair**，ESP-NOW MAC 自动加入 peer 列表
- 配对信息自动存入 Flash NVS，重启后自动恢复

### ESP-NOW Web API

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/now/mac` | 获取本机 ESP-NOW MAC 地址 |
| GET | `/api/now/peers` | 获取已配对 peer 列表（JSON） |
| POST | `/api/now/peer/remove` | 按 MAC 地址移除 peer |

### BLE 配对 Web API

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/ble/status` | BLE 状态（广播/扫描） |
| POST | `/api/ble/advertise` | 开启/关闭 BLE 广播，`enable=0/1&name=xxx` |
| POST | `/api/ble/scan` | 启动 BLE 扫描（持续 10 秒） |
| GET | `/api/ble/devices` | 获取已发现的 BLE 设备列表 |
| POST | `/api/ble/pair` | 与指定设备配对，`index=N` |

### BLE 广播数据格式

设备通过 BLE 广播的厂商自定义数据（0xFF）携带 ESP-NOW 信息：

| 字段 | 长度 | 说明 |
|------|------|------|
| MFG ID | 2B | 0xE502（自定义） |
| Tag | 2B | `'E'` `'N'` 标识 |
| ESP-NOW MAC | 6B | WiFi STA MAC 地址 |
| 设备名称 | N 字节 | 可自定义的设备标识 |

### 代码架构

```
wifi_now.h / wifi_now.c       — ESP-NOW 核心
├── 初始化 / 反初始化
├── Peer 管理（添加/移除/清空/NVS 持久化）
├── 发送（单播/广播）
├── 回调注册（收发回调）
└── 信道管理

ble_pairing.h / ble_pairing.c — BLE 配对层
├── BLE 控制器初始化（Bluedroid）
├── GAP 回调（扫描结果/广播状态）
├── 广播构建（MPU 数据编码）
├── 扫描解析（提取 ESP-NOW MAC）
└── 配对触发 → wifi_now_add_peer()
```

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
| BLE 设备名 | `ESP32-S3-NOW` | BLE 广播中的设备标识 |

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
| GET | `/api/ble/status` | BLE 状态 |
| POST | `/api/ble/advertise` | 开启/关闭 BLE 广播 |
| POST | `/api/ble/scan` | 启动 BLE 设备扫描 |
| GET | `/api/ble/devices` | 已发现的 BLE 设备列表 |
| POST | `/api/ble/pair` | 与指定 BLE 设备配对 |
| GET | `/api/usb/napt` | 查询 USB NAPT 状态 |
| POST | `/api/usb/napt` | 开启/关闭 USB NAPT（`enable=0/1`） |
| GET | `/api/now/mac` | 本机 ESP-NOW MAC |
| GET | `/api/now/peers` | ESP-NOW Peer 列表 |
| POST | `/api/now/peer/remove` | 移除指定 Peer |

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

ESP-NOW Mesh（独立于上述架构）
   ┌─────────┐        ┌─────────┐        ┌─────────┐
   │ 节点 A  │ ←────→ │ 节点 B  │ ←────→ │ 节点 C  │
   │ (BLE配对)│        │         │        │         │
   └─────────┘        └─────────┘        └─────────┘
   通过 BLE 广播交换 ESP-NOW MAC，自动建立 peer 连接
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

`HTTPD_DEFAULT_CONFIG()` 默认 `max_uri_handlers = 8`。当注册超过 8 个路由时，`httpd_register_uri_handler()` 静默失败。已手动增大至 `config.max_uri_handlers = 24`。

### DHCP DNS 选项格式与顺序（ESP-IDF v6.0.1）

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

必须在 `esp_netif_dhcps_stop()` 和 `esp_netif_dhcps_start()` **之间**执行。

**bug：** `usb_network.cpp` 中 `apply_dhcp_options()` 原在 `dhcps_start()` 之后调用，导致 DNS 标志位设置请求被拒绝（`ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED`），DHCP 服务器 fallback 到用自己的 IP（`192.168.5.1`）作为 DNS。Windows 自动获取 DNS 后因 ESP32 未运行 DNS 代理而无法解析域名。修复：将 `apply_dhcp_options()` 移到 `dhcps_start()` 之前。

### NAPT 线程安全

`ip_napt_enable_netif()` 必须通过 `tcpip_callback()` 在 lwIP TCP/IP 线程中调用，直接从事件处理线程调用会导致 NAT 表损坏。

### double-free 崩溃

`esp_netif_receive` 失败后内部已释放 buffer，再手动 `free(buf_copy)` 导致 double-free。修复：移除回调中的 `free(buf_copy)`。

## Web 管理页面

![Web管理页面截图](https://github.com/user-attachments/assets/84edbc7a-35da-4130-a8a9-f65c49cefab8)
