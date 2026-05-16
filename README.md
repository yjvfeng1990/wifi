<img width="780" height="4460" alt="3019ee298b7ac1e1f8f661ceeb5a48eb" src="https://github.com/user-attachments/assets/2c662b61-ee73-4452-9cf6-a26b57196296" />
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
- **Web 管理页面**：配置 SSID/密码、查看实时吞吐、切换模式
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
- PSRAM: 8MB | Flash: 2MB
- WiFi + BLE 5

## 快速开始

```bash
# 1. 设置 ESP-IDF 环境
. $IDF_PATH/export.ps1    # Windows PowerShell
# . $IDF_PATH/export.sh   # Linux / macOS

# 2. 编译
idf.py build

# 3. 烧录 + 串口监视
idf.py -p COM2 flash monitor
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

### DHCP DNS 选项格式（ESP-IDF v6.0.1）

`esp_netif_dhcps_option(ESP_NETIF_DOMAIN_NAME_SERVER)` 的参数类型为 `uint8_t` 标志位（`0x02`），不是 DNS IP 地址。正确用法需要两步：

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

且必须在 `esp_netif_dhcps_stop()` 和 `esp_netif_dhcps_start()` 之间执行。

### NAPT 线程安全

`ip_napt_enable_netif()` 必须通过 `tcpip_callback()` 在 lwIP TCP/IP 线程中调用，直接从事件处理线程调用会导致 NAT 表损坏。
