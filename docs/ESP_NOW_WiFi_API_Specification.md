# ESP-NOW WiFi API 组网规范

## 概述

本文档描述了通过WiFi HTTP API实现ESP-NOW节点自动发现和组网的方法。节点设备通过连接ESP32-S3的AP（热点），使用HTTP请求交换ESP-NOW MAC地址，实现无缝组网。

**文档版本**: 5.0  
**更新日期**: 2026-05-18  
**适用平台**: ESP-IDF v6.0.1 / ESP32-S3

---

## 目录

1. [协议关键约定](#0-协议关键约定) ⚠️ **必读**
2. [组网原理](#1-组网原理)
3. [API接口列表](#2-api接口列表)
4. [组网流程](#3-组网流程)
5. [API详细说明](#4-api详细说明)
6. [消息发送与接收协议](#5-消息发送与接收协议)
7. [实现示例](#6-实现示例)
8. [错误处理](#7-错误处理)
9. [其他平台开发指南](#8-其他平台开发指南)

---

## 0. 协议关键约定

> ⚠️ **对接前必读！** 以下约定全部必须满足，否则消息发送/接收将失败。

### 0.1 PMK (Primary Master Key)

本系统 ESP-NOW 使用的 PMK 为固定值：

```
PMK: "pmk1234567890123"  (16字节)
```

**对端必须设置相同的 PMK**，否则消息会被静默丢弃，不会有任何错误提示：

```c
// 对端 ESP-NOW 初始化时必须调用
esp_now_set_pmk((const uint8_t*)"pmk1234567890123");
```

### 0.2 信道 (Channel)

- ESP-NOW 收发双方必须在**同一个 WiFi 信道**上
- 信道通过 `/api/espnow/register` 响应的 `ap_channel` 字段获取
- **不要硬编码信道值**，使用 API 返回的实际信道

### 0.3 加密设置

本系统添加 peer 时使用 **`encrypt = false`**（不加密模式），对端必须保持一致：

```c
esp_now_peer_info_t peer = {};
memcpy(peer.peer_addr, ap_mac, 6);
peer.channel = ap_channel;
peer.ifidx   = WIFI_IF_STA;
peer.encrypt = false;           // 必须为 false
esp_now_add_peer(&peer);
```

### 0.4 双向 Peer 添加

ESP-NOW 通信需要**双方互相添加对方为 peer**：
- **主机侧**：`/api/espnow/register` 收到请求后自动将节点添加为 peer ✅
- **对端侧**：注册成功后，必须用响应中的 `ap_mac` 和 `ap_channel` 调用 `esp_now_add_peer()`

### 0.5 数据格式

ESP-NOW 链路上传输的是**原始二进制字节**。通过 HTTP API 发送时，数据以 **hex 字符串** 格式编码：

```
发送 "Hello" 的完整链路:
  HTTP API: {"mac":"...", "data":"48656c6c6f"}     ← hex 编码
  主机解码: hex_decode → {0x48,0x65,0x6c,0x6c,0x6f}  ← 二进制
  ESP-NOW:  5字节二进制数据
  对端收到: uint8_t data[] = {0x48,0x65,0x6c,0x6c,0x6f}
```

### 0.6 消息大小限制

- ESP-NOW 单帧最大 **250 字节**
- HTTP API 的 `data` 字段为 hex 字符串，长度 ≤ 500 字符
- 超过限制将被截断或拒绝

---

## 1. 组网原理

### 1.1 背景

ESP-NOW是一种高效的点对点WiFi通信协议，但传统的ESP-NOW配对需要：
- 预先知道对方的MAC地址
- 通过串口或其他方式手动配置
- 使用BLE或其他方式进行MAC交换

### 1.2 WiFi API组网方案 (v2.0)

利用ESP32-S3自带的AP功能，节点设备连接后可以通过HTTP请求自动交换MAC地址：

```
节点设备                          ESP32-S3 (AP模式)
    │                                    │
    │  连接AP (192.168.4.x)              │
    │ ─────────────────────────────────►│
    │                                    │
    │  POST /api/espnow/register        │  ← 上报自己的MAC
    │  {"mac":"节点MAC", "channel":1}  │
    │ ─────────────────────────────────►│
    │                                    │
    │                                    │  ← 将节点添加为peer
    │ ◄─────────────────────────────────│
    │  {"success":true,                 │
    │   "ap_mac":"AP的MAC",             │  ← 返回AP的MAC和channel
    │   "ap_channel":1}                 │
    │                                    │
    │  节点将AP的MAC添加到peer ✅        │
    │                                    │
    ✓ 双向ESP-NOW连接建立完成            ✓
```

### 1.3 优势

- ✅ **无需预先配置MAC地址**
- ✅ **自动发现和配对**
- ✅ **支持任意支持WiFi的设备**
- ✅ **通过HTTP，易于调试**
- ✅ **支持设备命名和标识**
- ✅ **节点自动获取AP的MAC信息**

### 1.4 新增功能 (v2.0)

- **AP客户端连接回调** - AP可监听客户端连接事件
- **增强的响应格式** - `/api/espnow/register` 返回AP的MAC和channel
- **自动peer添加** - 节点上报后自动添加到peer列表

---

## 2. API接口列表

| 方法 | 路径 | 描述 |
|------|------|------|
| GET | `/api/espnow/master` | 获取主机的ESP-NOW MAC地址 |
| POST | `/api/espnow/register` | 节点注册自己的ESP-NOW MAC |
| POST | `/api/espnow/send` | **发送ESP-NOW数据到指定节点** (v4.0) |
| POST | `/api/espnow/broadcast` | **广播ESP-NOW数据到所有节点** (v4.0) |
| POST | `/api/espnow/unpair` | **解绑指定的ESP-NOW节点** (v3.0) |
| GET | `/api/now/mac` | 获取本机ESP-NOW MAC (BLE) |
| GET | `/api/now/peers` | 获取已连接的ESP-NOW节点列表 |
| POST | `/api/now/peer/remove` | 移除指定的ESP-NOW节点 |

### 新增功能 (v5.0)

- **协议关键约定** - 明确 PMK、信道、encrypt、数据格式等对接必备条件
- **消息发送与接收协议** - 完整的数据链路描述和接收端代码示例
- **调试排查清单** - 8 项逐条排查对端收不到消息的原因
- **数据格式对照表** - hex 编码 ↔ 二进制 的完整对照

### 历史版本功能

- **v4.0** — ESP-NOW消息发送、广播、消息反馈
- **v3.0** — 解绑 ESP-NOW 节点
- **v2.0** — AP客户端连接回调、增强注册响应

---

## 3. 组网流程

### 3.1 完整组网序列图 (v2.0)

```
┌─────────────────────────────────────────────────────────────────────┐
│                    节点设备组网序列 (v2.0)                         │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  节点设备              ESP32-S3 (AP)         备注                    │
│  ─────────            ─────────────         ─────────               │
│                                                                      │
│  1. WiFi连接                                                     │
│  ────────────────►  连接AP (SSID)                                 │
│                    返回IP: 192.168.4.x                            │
│                                                                      │
│  2. 初始化ESP-NOW + 设置PMK                                │
│     esp_now_set_pmk("pmk1234567890123")                     │
│                                                              │
│  3. 注册自己                                                 │
│  ────────────────►  POST /api/espnow/register                │
│                    上报: {"mac":节点MAC, "channel":1, "name":"名称"} │
│                                                                      │
│                    3. 添加节点为peer                                │
│                    wifi_now_add_peer(node_mac)                      │
│                                                                      │
│                    4. 保存peer到NVS                                 │
│                                                                      │
│  5. ◄────────────── 返回成功                                       │
│                    {"success":true,                                │
│                     "ap_mac":"AP的MAC",                           │
│                     "ap_channel":1}                                │
│                                                                      │
│  6. 添加AP为peer                                                  │
│     wifi_now_add_peer(ap_mac, ap_channel)                          │
│                                                                      │
│  7. 组网完成！                                                     │
│     现在可以使用ESP-NOW双向通信                                     │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

### 3.2 节点代码流程

```c
// ESP32/ESP8266 节点代码示例 (v2.0)

void setup() {
    // 1. 连接WiFi到ESP32-S3的AP
    WiFi.begin("ESP32-S3", "password");  // 或无密码
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
    }
    Serial.println("WiFi connected");
    
    // 2. 初始化ESP-NOW
    if (esp_now_init() == ESP_OK) {
        Serial.println("ESP-NOW initialized");
        
        // 3. 注册自己到AP (会自动获取AP的MAC)
        register_to_master();
    }
}

void register_to_master() {
    HTTPClient http;
    
    // 获取本机MAC
    uint8_t my_mac[6];
    WiFi.macAddress(my_mac);
    
    // 发送注册请求
    StaticJsonDocument<256> doc;
    doc["mac"] = macToString(my_mac);
    doc["channel"] = 1;
    doc["name"] = "ESP32-Node1";
    
    String postData;
    serializeJson(doc, postData);
    
    http.begin("http://192.168.4.1/api/espnow/register");
    http.addHeader("Content-Type", "application/json");
    
    int httpCode = http.POST(postData);
    
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        StaticJsonDocument<512> response;
        deserializeJson(response, payload);
        
        // 解析响应，获取AP的MAC和channel
        String ap_mac_str = response["ap_mac"].as<String>();
        int ap_channel = response["ap_channel"].as<int>();
        
        Serial.printf("AP MAC: %s\n", ap_mac_str.c_str());
        Serial.printf("AP Channel: %d\n", ap_channel);
        
        // 将AP的MAC转换为字节数组
        uint8_t ap_mac[6];
        parseMac(ap_mac_str.c_str(), ap_mac);
        
        // 添加AP为peer
        addPeer(ap_mac, ap_channel);
        
        Serial.println("Registration successful!");
    }
    
    http.end();
}

void addPeer(uint8_t* mac, uint8_t channel) {
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    
    if (esp_now_add_peer(&peer) == ESP_OK) {
        Serial.println("Peer added successfully");
    }
}

void loop() {
    // 使用ESP-NOW通信
    delay(100);
}
```

---

## 4. API详细说明

### 4.1 GET /api/espnow/master

获取ESP32-S3（主机）的ESP-NOW MAC地址和当前信道。

**请求**
```
GET /api/espnow/master HTTP/1.1
Host: 192.168.4.1
```

**响应**
```json
HTTP/1.1 200 OK
Content-Type: application/json

{
  "mac": "3c:0f:02:d1:e6:94",
  "channel": 1
}
```

**字段说明**

| 字段 | 类型 | 描述 |
|------|------|------|
| mac | string | ESP32-S3的ESP-NOW MAC地址 (格式: XX:XX:XX:XX:XX:XX) |
| channel | int | ESP-NOW使用的WiFi信道 (1-14) |

---

### 4.2 POST /api/espnow/register (v2.0)

节点注册自己的ESP-NOW MAC地址到主机。主机收到后会将其添加为ESP-NOW peer，并返回主机的MAC和channel信息。

**请求**
```
POST /api/espnow/register HTTP/1.1
Host: 192.168.4.1
Content-Type: application/json

{
  "mac": "aa:bb:cc:dd:ee:ff",
  "channel": 1,
  "name": "ESP32-Node1"
}
```

**字段说明**

| 字段 | 类型 | 必需 | 描述 |
|------|------|------|------|
| mac | string | 是 | 节点的ESP-NOW MAC地址 |
| channel | int | 否 | 节点使用的信道，默认1 |
| name | string | 否 | 节点名称，用于标识 |

**响应 (v2.0)**
```json
HTTP/1.1 200 OK
Content-Type: application/json

{
  "success": true,
  "ap_mac": "3c:0f:02:d1:e6:94",
  "ap_channel": 1
}
```

**字段说明 (v2.0)**

| 字段 | 类型 | 描述 |
|------|------|------|
| success | bool | 注册是否成功 |
| ap_mac | string | **主机的ESP-NOW MAC** (新增) |
| ap_channel | int | **主机当前信道** (新增) |

**错误响应**
```json
{
  "success": false,
  "ap_mac": "00:00:00:00:00:00",
  "ap_channel": 1
}
```

**重要提示**：
- 响应中包含 `ap_mac` 和 `ap_channel` 字段
- 节点需要将这两个值用于添加主机为peer
- 节点应该使用响应中的channel，而不是请求中的channel
- ⚠️ 注册成功后，节点必须在 ESP-NOW 初始化后执行以下操作：
  1. `esp_now_set_pmk((const uint8_t*)"pmk1234567890123")` — 设置 PMK
  2. `esp_now_add_peer(&peer)` — 添加主机为 peer（`peer.encrypt = false`）
  3. 使用响应中的 `ap_channel` 作为 `peer.channel`

---

### 4.3 GET /api/now/mac

获取本机的ESP-NOW MAC地址（从BLE获取）。

**请求**
```
GET /api/now/mac HTTP/1.1
Host: 192.168.4.1
```

**响应**
```json
{
  "mac": "3c:0f:02:d1:e6:94"
}
```

---

### 4.4 GET /api/now/peers

获取已连接的ESP-NOW节点列表。

**请求**
```
GET /api/now/peers HTTP/1.1
Host: 192.168.4.1
```

**响应**
```json
{
  "peers": [
    {
      "mac": "aa:bb:cc:dd:ee:ff",
      "channel": 1,
      "name": "ESP32-Node1",
      "is_peer": true
    }
  ]
}
```

---

### 4.5 POST /api/now/peer/remove

从主机中移除指定的ESP-NOW节点。

**请求**
```
POST /api/now/peer/remove HTTP/1.1
Host: 192.168.4.1
Content-Type: application/json

{
  "mac": "aa:bb:cc:dd:ee:ff"
}
```

**响应**
```json
{
  "success": true
}
```

---

### 4.6 POST /api/espnow/unpair (v3.0)

解绑指定的ESP-NOW节点。此API会向对方发送解绑请求，并删除本地的peer记录。

**请求**
```
POST /api/espnow/unpair HTTP/1.1
Host: 192.168.4.1
Content-Type: application/json

{
  "mac": "aa:bb:cc:dd:ee:ff"
}
```

**字段说明**

| 字段 | 类型 | 必需 | 描述 |
|------|------|------|------|
| mac | string | 是 | 要解绑的节点MAC地址 |

**响应**
```json
{
  "success": true
}
```

**解绑流程 (v3.0)**

```
设备A                      设备B
  │                          │
  │  POST /unpair           │
  │  {"mac":"B的MAC"}        │
  │ ───────────────────────►│
  │                          │
  │  发送UNPAIR_REQUEST via ESP-NOW
  │ ───────────────────────►│
  │                          │
  │  收到UNPAIR_REQUEST       │
  │ ◄───────────────────────│
  │                          │
  │  删除B的peer             │
  │  发送UNPAIR_RESPONSE     │
  │ ◄───────────────────────│
  │                          │
  │  删除A的peer             │
  │                          │
  ✓ 解绑完成                   ✓
```

**重要提示**：
- 解绑会双向删除peer（A和B都会被删除）
- 解绑后双方都无法通过ESP-NOW互相通信
- 如需重新通信，需要重新配对

---

### 4.7 POST /api/espnow/send (v4.0)

发送ESP-NOW数据到指定的节点。

**请求**
```
POST /api/espnow/send HTTP/1.1
Host: 192.168.4.1
Content-Type: application/json

{
  "mac": "aa:bb:cc:dd:ee:ff",
  "data": "48656c6c6f"
}
```

**字段说明**

| 字段 | 类型 | 必需 | 描述 |
|------|------|------|------|
| mac | string | 是 | 目标节点的MAC地址 (格式: XX:XX:XX:XX:XX:XX) |
| data | string | 是 | hex 编码的二进制数据 (最大500字符 = 250字节) |

> ⚠️ **data 字段为 hex 字符串**，不是原始文本。发送 "Hello" 需编码为 `"48656c6c6f"`。对端 recv_cb 收到的是解码后的原始二进制字节 `{0x48,0x65,0x6c,0x6c,0x6f}`。

**响应**
```json
{
  "success": true,
  "sent": 5
}
```

**字段说明**

| 字段 | 类型 | 描述 |
|------|------|------|
| success | bool | 发送是否成功 |
| sent | int | 实际发送的字节数 |

**示例**

```bash
# 发送 "Hello" (hex: 48656c6c6f)
curl -X POST http://192.168.4.1/api/espnow/send \
  -H "Content-Type: application/json" \
  -d '{"mac":"AA:BB:CC:DD:EE:FF","data":"48656c6c6f"}'

# 发送 JSON 数据 {"t":25} (hex: 7b2274223a32357d)
curl -X POST http://192.168.4.1/api/espnow/send \
  -H "Content-Type: application/json" \
  -d '{"mac":"AA:BB:CC:DD:EE:FF","data":"7b2274223a32357d"}'
```

**使用场景**

- 节点向主控发送传感器数据
- 主控向节点发送控制命令
- 点对点数据通信

---

### 4.8 POST /api/espnow/broadcast (v4.0)

广播ESP-NOW数据到所有已配对的节点。

**请求**
```
POST /api/espnow/broadcast HTTP/1.1
Host: 192.168.4.1
Content-Type: application/json

{
  "data": "Broadcast Message"
}
```

**字段说明**

| 字段 | 类型 | 必需 | 描述 |
|------|------|------|------|
| data | string | 是 | hex 编码的广播数据 (最大500字符 = 250字节) |

> ⚠️ 与 send API 相同，`data` 必须为 hex 编码字符串。

**响应**
```json
{
  "success": true,
  "sent": 17
}
```

**字段说明**

| 字段 | 类型 | 描述 |
|------|------|------|
| success | bool | 广播是否成功 |
| sent | int | 实际发送的字节数 |

**示例**

```bash
# 广播 "Hello" (hex: 48656c6c6f)
curl -X POST http://192.168.4.1/api/espnow/broadcast \
  -H "Content-Type: application/json" \
  -d '{"data":"48656c6c6f"}'

# 广播 JSON: {"action":"restart"} (hex: 7b22616374696f6e223a2272657374617274227d)
curl -X POST http://192.168.4.1/api/espnow/broadcast \
  -H "Content-Type: application/json" \
  -d '{"data":"7b22616374696f6e223a2272657374617274227d"}'
```

**使用场景**

- 主控向所有节点广播通知
- 固件更新广播
- 系统时间同步
- 配置批量下发

**注意事项**

- 广播会发送到所有已配对的节点
- 如果没有配对节点，广播仍然会发送（但无人接收）
- 广播数据最大250字节

---

## 5. 消息发送与接收协议

### 5.1 协议总览

本系统提供两种消息发送方式，数据链路如下：

```
                    HTTP API 层              ESP-NOW 层
┌──────────┐    ┌──────────────┐    ┌────────────────────┐
│ Web UI   │───►│ /api/espnow/ │───►│ wifi_now_send()    │
│ / curl   │    │ send/        │    │ esp_now_send()     │~~~~~~► 对端设备
│          │    │ broadcast    │    │ (hex decode → bin) │
└──────────┘    └──────────────┘    └────────────────────┘
                                         │
                                    esp_now_set_pmk("pmk1234567890123")
                                    peer.encrypt = false
                                    peer.channel = ap_channel
```

### 5.2 发送消息 (Unicast)

**数据链路**：

```
发送方（本机）                              接收方（对端）
    │                                           │
    │  ① HTTP POST /api/espnow/send             │
    │  {"mac":"对端MAC","data":"hex数据"}        │
    │                                           │
    │  ② 服务端 hex_decode → 二进制字节         │
    │                                           │
    │  ③ esp_now_send(mac, bytes, len)          │
    │  ───────────────────────────────────────► │
    │    返回 ESP_OK = 已入队                    │
    │                                           │
    │  ④ 异步回调 espnow_send_cb                │
    │  ◄─────────────────────────────────────── │
    │    status = ESP_NOW_SEND_SUCCESS/FAIL     │
    │                                           │
    │  ⑤ 接收方 esp_now_recv_cb 触发            │
    │                                           │
    │  HTTP 响应: {"success":true,"sent":N}     │
    │                                           │
```

> ⚠️ **重要**：`esp_now_send()` 返回 `ESP_OK` 仅表示数据已入队到 WiFi TX 缓冲区，**不代表对端已接收**。实际投递状态通过异步发送回调通知。HTTP API 的 `success: true` 表示入队成功。

### 5.3 广播消息 (Broadcast)

广播使用 ESP-NOW 广播地址 `FF:FF:FF:FF:FF:FF`，发送到**所有已配对的 peer**。

```
POST /api/espnow/broadcast  →  hex_decode →  esp_now_send(broadcast_mac, bytes, len)
```

- 有 N 个 peer 时，ESP-NOW 底层依次发送 N 次
- 每个 peer 各自触发 `esp_now_recv_cb`
- HTTP 响应的 `sent` 字段为单次发送的字节数

### 5.4 对端接收消息

对端通过 `esp_now_register_recv_cb()` 注册回调来接收消息。收到的数据是**原始二进制字节**。

#### 5.4.1 完整的对端接收代码

```c
#include "esp_now.h"

// 发送回调
static void on_data_sent(const uint8_t* mac, esp_now_send_status_t status) {
    printf("Send to %02X:%02X:%02X:%02X:%02X:%02X: %s\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

// 接收回调 — 数据到此是原始二进制字节
static void on_data_recv(const esp_now_recv_info_t* info,
                         const uint8_t* data, int data_len) {
    printf("Recv %d bytes from %02X:%02X:%02X:%02X:%02X:%02X\n",
           data_len,
           info->src_addr[0], info->src_addr[1], info->src_addr[2],
           info->src_addr[3], info->src_addr[4], info->src_addr[5]);

    // 方式1: 打印为可读文本
    printf("As text: %.*s\n", data_len, data);

    // 方式2: 打印为 hex
    printf("As hex: ");
    for (int i = 0; i < data_len; i++) {
        printf("%02X", data[i]);
    }
    printf("\n");

    // 方式3: 如果是 JSON 字符串
    if (data_len > 0 && data[data_len - 1] == '\0') {
        printf("JSON: %s\n", (const char*)data);
    }
}

void espnow_init_receiver(void) {
    // ⚠️ 步骤1: 设置 PMK — 必须与主机一致
    esp_now_set_pmk((const uint8_t*)"pmk1234567890123");

    // 步骤2: 初始化 ESP-NOW
    esp_now_init();

    // 步骤3: 注册回调
    esp_now_register_recv_cb(on_data_recv);
    esp_now_register_send_cb(on_data_sent);

    // 步骤4: 添加主机为 peer（加密必须为 false）
    // ap_mac 从 /api/espnow/register 响应中获取
    // ap_channel 从响应的 ap_channel 字段获取
    uint8_t ap_mac[6] = {0x3C, 0x0F, 0x02, 0xD1, 0xE6, 0x94}; // 示例
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, ap_mac, 6);
    peer.channel = 1;           // 使用 register 响应返回的 channel
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;       // ⚠️ 必须为 false
    esp_now_add_peer(&peer);
}
```

#### 5.4.2 接收数据格式对照表

| 发送内容 | HTTP data 字段 | 线路二进制 | 对端 recv_cb 收到的 |
|----------|---------------|-----------|-------------------|
| `"Hello"` | `"48656c6c6f"` | `{48,65,6c,6c,6f}` | 5 bytes: `Hello` |
| `"123"` | `"313233"` | `{31,32,33}` | 3 bytes: `123` |
| JSON `{"t":25}` | `"7b2274223a32357d"` | `{7b,22,74,22,...}` | 9 bytes: `{"t":25}` |
| 纯 hex 数据 | `"aabbccdd"` | `{aa,bb,cc,dd}` | 4 bytes: `{0xaa,0xbb,0xcc,0xdd}` |

### 5.5 收不到消息的排查清单

按照以下顺序逐项排查：

| # | 检查项 | 正确配置 | 如何验证 |
|---|--------|---------|---------|
| 1 | **PMK 一致** | 双方都设置为 `"pmk1234567890123"` | 检查双方代码是否调用 `esp_now_set_pmk()` |
| 2 | **信道一致** | 双方在同一信道 | 查看 register 响应中的 `ap_channel`，对端必须使用该信道 |
| 3 | **双向 peer** | 主机添加了节点，节点也添加了主机 | 调用 `esp_now_is_peer_exist()` 检查 |
| 4 | **encrypt = false** | 双方 peer 的 encrypt 都为 false | 检查 `esp_now_add_peer()` 调用 |
| 5 | **注册了 recv_cb** | 对端调用了 `esp_now_register_recv_cb()` | 在回调中加日志确认触发 |
| 6 | **WiFi 模式** | 对端 WiFi 已初始化为 STA 模式 | `esp_wifi_get_mode()` 检查 |
| 7 | **ESP-NOW 已初始化** | `esp_now_init()` 返回 ESP_OK | 检查返回值 |
| 8 | **数据长度** | ≤ 250 字节 | 检查 `data_len` 参数 |

### 5.6 发送端确认 (ACK)

ESP-NOW 底层在 WiFi MAC 层有 ACK 机制。当 `esp_now_send_cb` 返回 `ESP_NOW_SEND_SUCCESS` 表示 MAC 层 ACK 已收到，即对方**物理层已确认接收**。但应用层是否处理取决于对端是否注册了 recv_cb。

若要应用层确认，需要实现**应用层回执**：

```
发送方                                接收方
  │  esp_now_send(data)               │
  │ ─────────────────────────────────►│
  │                                    │
  │                    MAC层 ACK       │
  │ ◄─────────────────────────────────│
  │  send_cb: SUCCESS                 │
  │                                    │  recv_cb(data)
  │                   应用层回执       │
  │ ◄──────── esp_now_send("ACK") ────│
  │  recv_cb: 收到 "ACK"              │
  │                                    │
  ✓ 发送成功确认                       ✓
```

---

## 6. 实现示例

### 6.1 ESP32 节点实现 (v2.0)

```cpp
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_now.h>
#include <ArduinoJson.h>

const char* ssid = "ESP32-S3";
const char* password = "";  // 如果有密码的话
const char* masterIP = "192.168.4.1";

const char* DEVICE_NAME = "ESP32-Node1";

uint8_t masterMac[6];

void setup() {
    Serial.begin(115200);
    
    // 连接WiFi到ESP32-S3 AP
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected");
    Serial.println("IP: " + WiFi.localIP().toString());
    
    // 初始化ESP-NOW
    if (esp_now_init() == ESP_OK) {
        Serial.println("ESP-NOW initialized");

        // ⚠️ 设置 PMK — 必须与主机一致
        esp_now_set_pmk((const uint8_t*)"pmk1234567890123");

        // 注册回调
        esp_now_register_recv_cb(onDataReceived);
        esp_now_register_send_cb(onDataSent);
        
        // 注册到主机 (会自动获取主机的MAC)
        registerToMaster();
    }
}

void registerToMaster() {
    HTTPClient http;
    
    // 获取本机MAC
    uint8_t myMac[6];
    WiFi.macAddress(myMac);
    
    // 构建注册请求
    StaticJsonDocument<256> doc;
    doc["mac"] = macToString(myMac);
    doc["channel"] = 1;
    doc["name"] = DEVICE_NAME;
    
    String postData;
    serializeJson(doc, postData);
    
    // 发送注册请求
    http.begin("http://" + String(masterIP) + "/api/espnow/register");
    http.addHeader("Content-Type", "application/json");
    
    int httpCode = http.POST(postData);
    
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        Serial.println("Response: " + payload);
        
        // 解析响应
        StaticJsonDocument<512> response;
        DeserializationError error = deserializeJson(response, payload);
        
        if (!error && response["success"].as<bool>()) {
            // 获取主机的MAC和channel
            String macStr = response["ap_mac"].as<String>();
            int channel = response["ap_channel"].as<int>();
            
            Serial.println("Master MAC: " + macStr);
            Serial.println("Master Channel: " + String(channel));
            
            // 解析MAC地址
            parseMac(macStr.c_str(), masterMac);
            
            // 添加主机为peer
            addPeer(masterMac, channel);
            
            Serial.println("Registration successful!");
        } else {
            Serial.println("Registration failed!");
        }
    } else {
        Serial.printf("HTTP Error: %d\n", httpCode);
    }
    
    http.end();
}

void addPeer(uint8_t* mac, uint8_t channel) {
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    
    if (esp_now_add_peer(&peer) == ESP_OK) {
        Serial.println("Peer added successfully");
    } else {
        Serial.println("Failed to add peer");
    }
}

void onDataReceived(const uint8_t* mac, const uint8_t* data, int len) {
    Serial.printf("Received from %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    // 处理接收到的数据
}

void onDataSent(const uint8_t* mac, esp_now_send_status_t status) {
    Serial.printf("Send to %02X:%02X:%02X:%02X:%02X:%02X: %s\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAILED");
}

void parseMac(const char* str, uint8_t* mac) {
    int values[6];
    sscanf(str, "%x:%x:%x:%x:%x:%x",
           &values[0], &values[1], &values[2],
           &values[3], &values[4], &values[5]);
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)values[i];
    }
}

String macToString(const uint8_t* mac) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

void loop() {
    // 主循环
    delay(100);
}
```

### 6.2 ESP8266 节点实现

```cpp
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <espnow.h>

const char* ssid = "ESP32-S3";
const char* password = "";
const char* masterIP = "192.168.4.1";

const char* DEVICE_NAME = "ESP8266-Node";

uint8_t masterMac[6];

void setup() {
    Serial.begin(115200);
    
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
    }
    
    if (esp_now_init() == ESP_OK) {
        // ⚠️ 设置 PMK — 必须与主机一致
        esp_now_set_pmk((const uint8_t*)"pmk1234567890123");

        esp_now_register_recv_cb(onDataReceived);
        esp_now_register_send_cb(onDataSent);
        
        registerToMaster();
    }
}

void registerToMaster() {
    HTTPClient http;
    
    uint8_t myMac[6];
    WiFi.macAddress(myMac);
    
    StaticJsonDocument<256> doc;
    doc["mac"] = macToString(myMac);
    doc["channel"] = 1;
    doc["name"] = DEVICE_NAME;
    
    String postData;
    serializeJson(doc, postData);
    
    http.begin("http://" + String(masterIP) + "/api/espnow/register");
    http.addHeader("Content-Type", "application/json");
    
    int httpCode = http.POST(postData);
    
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        
        StaticJsonDocument<512> response;
        deserializeJson(response, payload);
        
        if (response["success"].as<bool>()) {
            String macStr = response["ap_mac"].as<String>();
            int channel = response["ap_channel"].as<int>();
            
            parseMac(macStr.c_str(), masterMac);
            addPeer(masterMac, channel);
        }
    }
    
    http.end();
}

void addPeer(uint8_t* mac, uint8_t channel) {
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = channel;
    
    esp_now_add_peer(&peer);
}

void onDataReceived(uint8_t* mac, uint8_t* data, uint8_t len) {
    // 处理接收
}

void onDataSent(uint8_t* mac, uint8_t status) {
    // 处理发送状态
}

void parseMac(const char* str, uint8_t* mac) {
    int values[6];
    sscanf(str, "%x:%x:%x:%x:%x:%x",
           &values[0], &values[1], &values[2],
           &values[3], &values[4], &values[5]);
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)values[i];
    }
}

String macToString(const uint8_t* mac) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

void loop() {
    delay(100);
}
```

### 6.3 Python 实现（树莓派/PC）

```python
import requests
import json

MASTER_IP = "192.168.4.1"
DEVICE_NAME = "RPi-Node"

def register_to_master(mac, channel, name):
    """
    注册自己到主机
    返回主机的MAC和channel信息
    """
    data = {
        "mac": mac,
        "channel": channel,
        "name": name
    }
    response = requests.post(
        f"http://{MASTER_IP}/api/espnow/register",
        json=data
    )
    return response.json()

def get_local_mac():
    """获取本机MAC地址 (Linux)"""
    import uuid
    mac = ':'.join(['{:02x}'.format((uuid.getnode() >> i) & 0xff) 
                    for i in range(0, 48, 8)][::-1])
    return mac.upper()

def main():
    print("Connecting to WiFi...")
    # 使用系统WiFi连接到ESP32-S3的AP
    # os.system(f"nmcli d wifi connect {ssid} password {password}")
    
    my_mac = get_local_mac()
    print(f"My MAC: {my_mac}")
    
    print("Registering to master...")
    result = register_to_master(my_mac, 1, DEVICE_NAME)
    
    print(f"Registration result: {result}")
    
    if result['success']:
        # 获取主机的MAC和channel
        ap_mac = result['ap_mac']
        ap_channel = result['ap_channel']
        
        print(f"✓ Successfully registered!")
        print(f"  AP MAC: {ap_mac}")
        print(f"  AP Channel: {ap_channel}")
        print(f"  Now you can add {ap_mac} as ESP-NOW peer on channel {ap_channel}")
    else:
        print("✗ Registration failed!")

if __name__ == "__main__":
    main()
```

### 6.4 JavaScript/Node.js 实现

```javascript
const http = require('http');

const MASTER_IP = '192.168.4.1';
const DEVICE_NAME = 'NodeJS-Device';

function registerToMaster(mac, channel, name) {
    return new Promise((resolve, reject) => {
        const data = JSON.stringify({ mac, channel, name });
        const options = {
            hostname: MASTER_IP,
            port: 80,
            path: '/api/espnow/register',
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Content-Length': data.length
            }
        };
        
        const req = http.request(options, (res) => {
            let response = '';
            res.on('data', chunk => response += chunk);
            res.on('end', () => {
                resolve(JSON.parse(response));
            });
        });
        
        req.on('error', reject);
        req.write(data);
        req.end();
    });
}

async function main() {
    const myMac = 'AA:BB:CC:DD:EE:FF'; // 需要获取实际MAC
    
    console.log('Registering to master...');
    const result = await registerToMaster(myMac, 1, DEVICE_NAME);
    
    if (result.success) {
        console.log(`✓ Successfully registered!`);
        console.log(`  AP MAC: ${result.ap_mac}`);
        console.log(`  AP Channel: ${result.ap_channel}`);
        console.log(`  Now you can add ${result.ap_mac} as ESP-NOW peer on channel ${result.ap_channel}`);
    } else {
        console.log('✗ Registration failed!');
    }
}

main().catch(console.error);
```

---

## 7. 错误处理

### 7.1 常见错误及解决方案

| # | 错误现象 | 原因 | 解决方案 |
|---|---------|------|----------|
| 1 | 连接AP失败 | SSID或密码错误 | 检查AP配置 |
| 2 | GET /master 返回404 | API路径错误 | 确认API路径 |
| 3 | POST /register 返回500 | JSON格式错误 | 检查JSON格式 |
| 4 | ESP-NOW添加peer失败 | MAC地址无效 | 确认MAC格式正确 |
| 5 | 发送成功(success:true)但**对端收不到** | **PMK不一致** | 对端必须设置 `esp_now_set_pmk("pmk1234567890123")` |
| 6 | 发送成功但对端收不到 | **信道不匹配** | 使用 register 响应的 `ap_channel`，对端必须同信道 |
| 7 | 发送成功但对端收不到 | **对端未添加主机为 peer** | 对端必须用 register 响应的 `ap_mac` 调用 `esp_now_add_peer()` |
| 8 | 发送成功但对端收不到 | **对端 encrypt 设置错误** | 对端 `peer.encrypt` 必须为 `false` |
| 9 | 发送成功但对端收不到 | **对端未注册 recv_cb** | 对端必须调用 `esp_now_register_recv_cb()` |
| 10 | 发送返回 success:false | MAC地址无效或数据为空 | 检查请求中的 MAC 和 data 字段 |
| 11 | 发送返回 success:false | ESP-NOW 未初始化 | 调用 `/api/espnow/master` 确认 ESP-NOW 状态 |
| 12 | 对端收到乱码 | 数据格式理解错误 | 对端收到的是原始二进制字节，非 hex 字符串 |

### 7.2 调试流程

```
发送失败排查:
  ┌─ success:false?
  │   ├─ 检查 MAC 格式: XX:XX:XX:XX:XX:XX
  │   ├─ 检查 data 字段是否存在
  │   └─ 检查 ESP-NOW 是否已初始化
  │
  └─ success:true 但对方收不到?
      ├─ 1. 对端设置了 esp_now_set_pmk("pmk1234567890123") ?
      ├─ 2. 对端 WiFi 信道 == register 响应的 ap_channel ?
      ├─ 3. 对端调用了 esp_now_add_peer(ap_mac) ?
      ├─ 4. 对端 peer.encrypt == false ?
      ├─ 5. 对端调用了 esp_now_register_recv_cb() ?
      └─ 6. 数据长度 ≤ 250 字节 ?
```

### 7.3 重试机制

```cpp
bool registerWithRetry(int maxRetries = 3) {
    for (int i = 0; i < maxRetries; i++) {
        HTTPClient http;
        http.begin(url);
        
        int code = http.POST(postData);
        if (code == 200) {
            String response = http.getString();
            // 解析响应
            StaticJsonDocument<512> doc;
            DeserializationError error = deserializeJson(doc, response);
            
            if (!error && doc["success"].as<bool>()) {
                http.end();
                return true;
            }
        }
        
        http.end();
        delay(1000 * (i + 1));  // 指数退避
    }
    return false;
}
```

---

## 8. 其他平台开发指南

### 8.1 通用要求

1. **WiFi STA模式** - 设备需要支持WiFi客户端模式
2. **HTTP Client** - 支持HTTP GET/POST请求
3. **JSON解析** - 能够解析和构建JSON数据
4. **ESP-NOW支持** - 设备需要支持ESP-NOW协议（仅ESP系列）

### 8.2 平台兼容性

| 平台 | WiFi | HTTP | JSON | ESP-NOW |
|------|------|------|------|----------|
| ESP32 | ✅ | ✅ | ✅ | ✅ |
| ESP8266 | ✅ | ✅ | ✅ | ✅ |
| 树莓派 | ✅ | ✅ | ✅ | ❌ |
| PC (Python) | ✅ | ✅ | ✅ | ❌ |
| 手机APP | ✅ | ✅ | ✅ | ❌ |
| 其他MCU | ✅ | ✅ | ✅ | ❌ |

### 8.3 非ESP设备注意事项

对于非ESP系列设备（如树莓派、PC等），由于不支持ESP-NOW协议，可以通过以下方式使用此API：

1. **仅作为配置工具** - 使用API配置ESP32设备的参数
2. **间接通信** - 通过HTTP API中转ESP-NOW数据
3. **监控和管理** - 查看设备状态、管理节点

### 8.4 重要提示

- **设置 PMK** — 对端 ESP-NOW 初始化后必须调用 `esp_now_set_pmk((const uint8_t*)"pmk1234567890123")`
- **使用响应中的 channel** — POST /register 响应中的 `ap_channel` 字段是主机实际使用的 channel
- **不要假设 channel** — 不要硬编码 channel 值，应使用 API 返回的实际值
- **encrypt = false** — 添加 peer 时必须设置 `peer.encrypt = false`
- **hex 编码数据** — send/broadcast API 的 `data` 字段必须为 hex 编码字符串
- **处理失败情况** — 检查响应中的 `success` 字段，逐一排查 8 项清单

---

## 9. 最佳实践

### 9.1 安全性

- ⚠️ **AP密码保护** - 生产环境中应设置AP密码
- ⚠️ **API认证** - 可添加token验证防止未授权注册
- ⚠️ **数据加密** - 敏感数据应加密传输

### 9.2 性能优化

- 📊 **批量注册** - 支持批量注册多个节点
- 🔄 **自动重连** - 实现WiFi和ESP-NOW的自动重连
- 📝 **日志记录** - 记录组网过程便于调试

### 9.3 可靠性

- ✅ **超时处理** - 所有网络操作设置超时
- ✅ **错误恢复** - 失败后自动重试
- ✅ **状态检查** - 定期检查连接状态

---

## 10. API测试

### 10.1 使用curl测试

```bash
# 获取主机信息
curl http://192.168.4.1/api/espnow/master

# 发送数据到指定节点 (hex 编码: 48656c6c6f = "Hello")
curl -X POST http://192.168.4.1/api/espnow/send \
  -H "Content-Type: application/json" \
  -d '{"mac":"AA:BB:CC:DD:EE:FF","data":"48656c6c6f"}'

# 广播数据到所有节点 (hex 编码)
curl -X POST http://192.168.4.1/api/espnow/broadcast \
  -H "Content-Type: application/json" \
  -d '{"data":"48656c6c6f"}'

# 注册节点 (v2.0 - 响应包含AP的MAC和channel)
curl -X POST http://192.168.4.1/api/espnow/register \
  -H "Content-Type: application/json" \
  -d '{"mac":"AA:BB:CC:DD:EE:FF","channel":1,"name":"TestNode"}'

# 查看节点列表
curl http://192.168.4.1/api/now/peers

# 解绑节点
curl -X POST http://192.168.4.1/api/espnow/unpair \
  -H "Content-Type: application/json" \
  -d '{"mac":"AA:BB:CC:DD:EE:FF"}'
```

### 10.2 浏览器测试

直接在浏览器中访问：
- `http://192.168.4.1/api/espnow/master`
- `http://192.168.4.1/api/now/peers`

---

## 附录：修订历史

| 版本 | 日期 | 作者 | 变更内容 |
|------|------|------|----------|
| 1.0 | 2026-05-17 | System | 初始版本 |
| 2.0 | 2026-05-18 | System | 更新注册响应格式，添加AP客户端回调机制 |
| 3.0 | 2026-05-18 | System | 添加退网解绑功能 |
| 4.0 | 2026-05-18 | System | 添加ESP-NOW消息发送和广播功能 |

---

**文档版本**: 4.0

---

**文档版本**: 3.0  
**更新日期**: 2026-05-18  
**适用平台**: ESP-IDF v6.0.1 / ESP32-S3
