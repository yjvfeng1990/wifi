# ESP-NOW BLE发现服务组网规范（中文版）

## 快速参考

**文档版本**: 4.0  
**更新日期**: 2026-05-25  
**目标平台**: ESP32-S3 (ESP-IDF v6.0.1)

---

## 新增功能 (v2.0)

- **自动双向配对** - 扫描端和广播端自动互相添加peer
- **PAIR_REQUEST/PAIR_RESPONSE消息协议** - 通过ESP-NOW交换配对信息
- **多设备支持** - 扫描端支持添加多个广播端
- **智能角色区分** - 广播端只添加扫描端，不添加其他广播端

## 新增功能 (v3.0)

- **退网解绑功能** - 完整的解绑机制，包括本地删除和通知对方
- **UNPAIR_REQUEST/UNPAIR_RESPONSE消息协议** - 通过ESP-NOW通知对方删除peer
- **解绑回调机制** - 通知应用层解绑事件

---

## BLE广播数据格式

设备在BLE广播中发送以下格式的厂商数据（AD Type: 0xFF）：

| 偏移 | 长度 | 字段 | 描述 |
|------|------|------|------|
| 0 | 2 | 厂商ID | `0x02E5` (乐鑫) |
| 2 | 2 | 协议标记 | `"EN"` (0x45, 0x4E) |
| 4 | 6 | ESP-NOW MAC | 设备的WiFi MAC地址 |
| 10 | 1 | 版本号 | 协议版本 (当前为1) |
| 11 | 1 | 信道 | 设备当前WiFi信道 |
| 12 | 1 | 节点类型 | `0`=WiFi, `1`=ESP-NOW |
| 13+ | N | 设备名称 | UTF-8编码设备名称 (最多19字节) |

### 关键常量

```c
#define BLE_MFG_ID           0x02E5  // 乐鑫厂商ID
#define BLE_TAG_MARKER_0     'E'     // 协议标记
#define BLE_TAG_MARKER_1     'N'     // 协议标记
#define BLE_MAX_DISCOVERED   16      // 最大发现设备数
#define BLE_DEV_NAME_MAX     32      // 最大设备名称长度
#define ESP_NOW_MAX_PEERS    20      // 最大ESP-NOW节点数
#define ESP_NOW_MAX_DATA_LEN 250     // 最大数据长度

// 自动配对消息 (v2.0)
#define ESP_NOW_PAIR_MAGIC         0x4553504E  // "ESPN"
#define ESP_NOW_MSG_PAIR_REQUEST   0x01       // 配对请求
#define ESP_NOW_MSG_PAIR_RESPONSE  0x02       // 配对响应
#define ESP_NOW_MSG_DATA           0x10       // 数据消息

// 解绑消息 (v3.0)
#define ESP_NOW_MSG_UNPAIR_REQUEST   0x03       // 解绑请求
#define ESP_NOW_MSG_UNPAIR_RESPONSE  0x04       // 解绑响应

// 节点类型 (v4.0)
#define PEER_TYPE_WIFI            0          // WiFi节点
#define PEER_TYPE_ESPNOW          1          // ESP-NOW节点

// 扩展消息类型 (v4.0)
#define ESP_NOW_MSG_UNBIND_ALL    0x06       // 一键解绑所有节点
#define ESP_NOW_MSG_ACK           0x05       // 应用层ACK确认

// 扫描补扫参数 (v4.0)
#define MAX_SCAN_RETRIES          3          // 最大补扫次数
#define RETRY_SCAN_DURATION       30         // 每次补扫持续时间(秒)
```

---

## 自动配对消息结构 (v2.0)

```c
// 配对消息结构
typedef struct {
    uint32_t magic;          // 0x4553504E ("ESPN")
    uint8_t  type;           // 消息类型 (0x01-0x06, 0x10)
    uint8_t  mac[6];         // 发送者的ESP-NOW MAC
    uint8_t  channel;        // 发送者当前WiFi信道
    uint8_t  peer_type;      // 节点类型 (PEER_TYPE_WIFI=0, PEER_TYPE_ESPNOW=1)
    char     name[32];       // 设备名称
    uint8_t  pmk[16];        // 自动生成的PMK，用于对端同步
} __attribute__((packed)) esp_now_pair_msg_t;

// 大小说明：该结构体当前为 63 字节。
```

### 消息类型

| 类型 | 值 | 描述 |
|------|-----|------|
| PAIR_REQUEST | 0x01 | 请求添加发送者为peer |
| PAIR_RESPONSE | 0x02 | 响应接受peer添加 |
| UNPAIR_REQUEST | 0x03 | 请求解绑 |
| UNPAIR_RESPONSE | 0x04 | 响应解绑确认 |
| ACK | 0x05 | 应用层ACK确认 |
| UNBIND_ALL | 0x06 | 一键解绑所有节点 (v4.0) |
| DATA | 0x10 | 常规数据消息 |

---

## 实现示例

### 构建BLE广播数据

```c
// 构建厂商特定数据
uint8_t build_adv_raw(uint8_t* buf, uint8_t buf_size) {
    uint8_t pos = 0;
    
    // Flags (0x01)
    buf[pos++] = 2;  // Length
    buf[pos++] = 0x01;  // Type: Flags
    buf[pos++] = 0x06;  // LE General Discoverable
    
    // Device Name (0x09)
    uint8_t name_len = strlen(device_name);
    if (pos + 2 + name_len <= buf_size) {
        buf[pos++] = 1 + name_len;
        buf[pos++] = 0x09;  // Type: Complete Local Name
        memcpy(buf + pos, device_name, name_len);
        pos += name_len;
    }
    
    // Manufacturer Data (0xFF)
    uint8_t mfg_total_len = 10 + name_len;  // 固定10字节 + 名称
    if (pos + 2 + mfg_total_len <= buf_size) {
        buf[pos++] = 1 + mfg_total_len;
        buf[pos++] = 0xFF;  // Type: Manufacturer Data
        
        // 厂商ID (小端序)
        buf[pos++] = 0xE5;  // MFG_ID LSB
        buf[pos++] = 0x02;  // MFG_ID MSB
        
        // 协议标记
        buf[pos++] = 'E';
        buf[pos++] = 'N';
        
        // ESP-NOW MAC
        memcpy(buf + pos, espnow_mac, 6);
        pos += 6;
        
        // 设备名称
        memcpy(buf + pos, device_name, name_len);
        pos += name_len;
    }
    
    return pos;
}
```

### 解析BLE扫描数据

```c
// 解析扫描响应中的厂商数据
bool parse_scan_mfg_data(const uint8_t* adv_data, uint8_t adv_data_len,
                         uint8_t* now_mac_out, char* name_out, int name_size) {
    uint8_t idx = 0;
    
    while (idx < adv_data_len) {
        uint8_t len = adv_data[idx];
        if (len == 0 || idx + len >= adv_data_len) break;
        
        uint8_t type = adv_data[idx + 1];
        
        // 检查厂商数据 (Type = 0xFF)
        if (type == 0xFF && len >= 12) {
            uint16_t mfg_id = adv_data[idx + 2] | ((uint16_t)adv_data[idx + 3] << 8);
            
            // 验证是ESP-NOW设备
            if (mfg_id == 0x02E5 &&
                adv_data[idx + 4] == 'E' &&
                adv_data[idx + 5] == 'N') {
                
                // 提取ESP-NOW MAC
                memcpy(now_mac_out, adv_data + idx + 6, 6);
                
                // 提取设备名称
                if (name_out && name_size > 0) {
                    int name_len = (int)len - 10;
                    if (name_len > name_size - 1) name_len = name_size - 1;
                    if (name_len > 0) {
                        memcpy(name_out, adv_data + idx + 12, name_len);
                        name_out[name_len] = '\0';
                    }
                }
                
                return true;
            }
        }
        
        idx += (len + 1);
    }
    
    return false;
}
```

### 发送配对请求 (v2.0)

```c
// 通过ESP-NOW发送配对请求
bool wifi_now_send_pair_request(const uint8_t* dest_mac) {
    if (!dest_mac || !wifi_now_is_initialized()) {
        return false;
    }
    
    esp_now_pair_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic = ESP_NOW_PAIR_MAGIC;  // 0x4553504E
    msg.type = ESP_NOW_MSG_PAIR_REQUEST;
    wifi_now_get_mac(msg.mac);
    ble_pairing_get_name(msg.name);
    
    int len = sizeof(esp_now_pair_msg_t);
    int sent = wifi_now_send(dest_mac, (const uint8_t*)&msg, len);
    
    if (sent > 0) {
        ESP_LOGI(TAG, "配对请求已发送");
        return true;
    }
    return false;
}
```

### 处理配对消息 (v2.0)

```c
// 处理收到的配对消息
bool wifi_now_handle_pair_message(const uint8_t* mac_addr, 
                                  const uint8_t* data, int len) {
    if (!mac_addr || !data || len < sizeof(esp_now_pair_msg_t)) {
        return false;
    }
    
    esp_now_pair_msg_t* msg = (esp_now_pair_msg_t*)data;
    
    if (msg->magic != ESP_NOW_PAIR_MAGIC) {
        return false;
    }
    
    if (msg->type == ESP_NOW_MSG_PAIR_REQUEST) {
        ESP_LOGI(TAG, "收到配对请求 from " MACSTR, MAC2STR(msg->mac));
        
        // 添加发送者为peer
        if (!wifi_now_is_peer_exists(msg->mac)) {
            wifi_now_add_peer_with_name(msg->mac, s_channel, msg->name);
            wifi_now_save_peers();
        }
        
        // 发送配对响应
        wifi_now_send_pair_response(msg->mac);
        
        // 通知回调
        if (s_pair_cb) {
            s_pair_cb(msg->mac, msg->name);
        }
        
        return true;
    }
    
    if (msg->type == ESP_NOW_MSG_PAIR_RESPONSE) {
        ESP_LOGI(TAG, "收到配对响应 from " MACSTR, MAC2STR(msg->mac));
        
        // 添加发送者为peer
        if (!wifi_now_is_peer_exists(msg->mac)) {
            wifi_now_add_peer_with_name(msg->mac, s_channel, msg->name);
            wifi_now_save_peers();
        }
        
        // 通知回调
        if (s_pair_cb) {
            s_pair_cb(msg->mac, msg->name);
        }
        
        return true;
    }

    return false;
}

/// 处理解绑消息 (v3.0)
bool wifi_now_handle_unpair_message(const uint8_t* mac_addr,
                                    const uint8_t* data, int len) {
    if (!mac_addr || !data || len < sizeof(esp_now_pair_msg_t)) {
        return false;
    }

    esp_now_pair_msg_t* msg = (esp_now_pair_msg_t*)data;

    if (msg->magic != ESP_NOW_PAIR_MAGIC) {
        return false;
    }

    if (msg->type == ESP_NOW_MSG_UNPAIR_REQUEST) {
        ESP_LOGI(TAG, "收到解绑请求 from " MACSTR, MAC2STR(msg->mac));

        if (wifi_now_is_peer_exists(msg->mac)) {
            wifi_now_remove_peer(msg->mac);
        }

        wifi_now_send_unpair_response(msg->mac);

        if (s_unpair_cb) {
            s_unpair_cb(msg->mac);
        }

        return true;
    }

    if (msg->type == ESP_NOW_MSG_UNPAIR_RESPONSE) {
        ESP_LOGI(TAG, "收到解绑响应 from " MACSTR, MAC2STR(msg->mac));

        if (wifi_now_is_peer_exists(msg->mac)) {
            wifi_now_remove_peer(msg->mac);
        }

        if (s_unpair_cb) {
            s_unpair_cb(msg->mac);
        }

        return true;
    }

    return false;
}

/// 解绑指定节点 (v3.0)
bool wifi_now_unpair_with_peer(const uint8_t* mac_addr) {
    if (!mac_addr || !wifi_now_is_initialized()) {
        return false;
    }

    if (!wifi_now_is_peer_exists(mac_addr)) {
        ESP_LOGW(TAG, "节点不存在: " MACSTR, MAC2STR(mac_addr));
        return false;
    }

    ESP_LOGI(TAG, "正在解绑节点 " MACSTR, MAC2STR(mac_addr));

    bool sent = wifi_now_send_unpair_request(mac_addr);

    vTaskDelay(pdMS_TO_TICKS(100));

    if (wifi_now_is_peer_exists(mac_addr)) {
        wifi_now_remove_peer(mac_addr);
    }

    if (s_unpair_cb) {
        s_unpair_cb(mac_addr);
    }

    return true;
}
```

---

## 自动配对流程 (v2.0)

```
┌──────────────────────────────────────────────────────────────────┐
│                    自动配对序列 (v2.0)                          │
├──────────────────────────────────────────────────────────────────┤
│                                                                    │
│  设备A (广播端)              设备B (扫描端)                      │
│  ────────────              ────────────────                      │
│                                                                    │
│  1. 开始BLE广播                                                 │
│  ↓                                                             │
│                                                                    │
│  2. BLE广播 ────────────────► 收到广播                          │
│     [MAC + 名称 + MFG]                                             │
│  ↓                                                             │
│                                                                    │
│  3. 解析MFG数据             提取ESP-NOW MAC                      │
│  ↓                                                             │
│                                                                    │
│  4. 如果未配对：                                                  │
│     添加A为peer ───────────► wifi_now_add_peer(A_MAC)            │
│     发送PAIR_REQUEST ──────► [ESPN | REQUEST | B_MAC]           │
│  ↓                                                             │
│                                                                    │
│  5. 收到PAIR_REQUEST      ◄───────────────                     │
│  ↓                                                             │
│                                                                    │
│  6. 添加B为peer              wifi_now_add_peer(B_MAC)            │
│  发送PAIR_RESPONSE ─────────► [ESPN | RESPONSE | A_MAC]          │
│  ↓                                                             │
│                                                                    │
│  7. 收到PAIR_RESPONSE    ◄───────────────                     │
│  保存peers到NVS ─────────► wifi_now_save_peers()                │
│  ↓                                                             │
│                                                                    │
│  ✓ 双向ESP-NOW连接建立成功!                                        │
│                                                                    │
└──────────────────────────────────────────────────────────────────┘
```

## 解绑流程 (v3.0)

```
┌──────────────────────────────────────────────────────────────────┐
│                    解绑序列 (v3.0)                             │
├──────────────────────────────────────────────────────────────────┤
│                                                                    │
│  设备A (发起解绑)           设备B (被解绑)                       │
│  ──────────────           ──────────────                       │
│                                                                    │
│  1. 调用解绑函数                                                 │
│     wifi_now_unpair_with_peer(B_MAC)                              │
│  ↓                                                             │
│                                                                    │
│  2. 发送UNPAIR_REQUEST ─────────► [ESPN | UNPAIR | B_MAC]       │
│  ↓                                                             │
│                                                                    │
│  3. 收到UNPAIR_REQUEST      ◄───────────────                   │
│  ↓                                                             │
│                                                                    │
│  4. 删除peer                    wifi_now_remove_peer(A_MAC)       │
│  发送UNPAIR_RESPONSE ─────────► [ESPN | UNPAIR_RES | B_MAC]      │
│  ↓                                                             │
│                                                                    │
│  5. 收到UNPAIR_RESPONSE    ◄───────────────                   │
│  删除peer                  wifi_now_remove_peer(B_MAC)           │
│  保存peers到NVS            wifi_now_save_peers()                 │
│  ↓                                                             │
│                                                                    │
│  触发解绑回调              if (s_unpair_cb) s_unpair_cb(B_MAC)   │
│  ↓                                                             │
│                                                                    │
│  ✓ 双向解绑完成!                                                  │
│                                                                    │
└──────────────────────────────────────────────────────────────────┘
```

### 解绑说明 (v3.0)

- **主动解绑**: 任何一方都可以发起解绑
- **双向删除**: 解绑会删除双方的peer记录
- **通知机制**: 通过ESP-NOW消息通知对方删除peer
- **回调通知**: 通知应用层解绑事件
- **持久化**: 解绑后会保存到NVS，重启后依然生效

### 角色说明 (v2.0)

**扫描端角色 (BLE扫描发起者)**
- 发起BLE扫描
- **可以添加多个广播端为peer**
- 发现广播端时发送PAIR_REQUEST
- 接收并处理PAIR_RESPONSE消息
- 开机自动启动 60s BLE 扫描，之后通过 START/STOP 按钮手动控制

**广播端角色 (BLE广播)**
- 发送BLE广播包
- **只添加扫描端为peer**（不添加其他广播端）
- 收到PAIR_REQUEST后添加发送者为peer
- 发送PAIR_RESPONSE
- 开机自动启动 60s BLE 广播，之后通过 START/STOP 按钮手动控制

**角色控制行为 (v2.1)**

| 触发方式 | 行为 | 说明 |
|----------|------|------|
| 开机 | 自动启动 BLE，60s 后停止 | MASTER 扫描 / SLAVE 广播 |
| START 按钮 | 启动/重置 BLE，新 60s 倒计时 | 已运行时仅重置计时器 |
| STOP 按钮 | 立即停止 BLE，state=IDLE | LED 关闭，remaining=0 |
| GPIO4 拉低 | 触发 START 行为 | 防抖 300ms |
| 60s 到期 | 自动停止 BLE，state=IDLE | 角色保持不变，ESP-NOW 继续工作 |

---

## BLE广播参数

| 参数 | 值 | 描述 |
|------|------|------|
| 广播类型 | 扩展广播 | Legacy Indication |
| 广播间隔 | 100~125ms (160~200) | 每100~125ms广播一次 |
| 信道映射 | 0x07 | 使用全部3个信道 |
| 主PHY | 1M | 1Mbps物理层 |
| 次PHY | 1M | 1Mbps物理层 |

---

## BLE扫描参数

| 参数 | 值 | 描述 |
|------|------|------|
| 扫描类型 | 主动扫描 | Active Scan |
| 扫描间隔 | 100ms (160) | 扫描间隔 |
| 扫描窗口 | 50ms (80) | 扫描窗口大小 |
| 默认扫描时长 | 60秒（可通过API配置） | 自动停止时间 |
| 重复过滤 | 启用 | `BLE_SCAN_DUPLICATE_ENABLE` |

---

## ESP-NOW参数

| 参数 | 值 | 描述 |
|------|------|------|
| 最大数据长度 | 250字节 | ESP-NOW规范限制 |
| 最大节点数 | 20 | 最大注册节点 |
| 广播MAC | FF:FF:FF:FF:FF:FF | 广播地址 |
| 默认信道 | 1 | WiFi信道1-14 |

---

## 数据结构

```c
// BLE发现的设备信息
typedef struct {
    uint8_t mac[6];       // BLE MAC地址
    uint8_t now_mac[6];   // ESP-NOW MAC地址
    char    name[32];     // 设备名称
    int     rssi;         // 信号强度
    uint8_t channel;      // 对方WiFi信道 (v4.0)
    uint8_t peer_type;    // 节点类型 (v4.0)
} ble_discovered_device_t;

// ESP-NOW节点信息
typedef struct {
    uint8_t mac[6];       // 节点MAC地址
    int     channel;      // WiFi信道
    char    name[32];     // 节点名称
    uint8_t peer_type;    // 节点类型 (v4.0)
} wifi_now_peer_info_t;

// 配对消息结构 (v2.0)
typedef struct {
    uint32_t magic;        // 0x4553504E ("ESPN")
    uint8_t  type;         // 消息类型
    uint8_t  mac[6];       // 发送者的ESP-NOW MAC
    char     name[32];      // 设备名称
} __attribute__((packed)) esp_now_pair_msg_t;

// 配对状态枚举
typedef enum {
    BLE_PAIR_STATE_OFF = 0,           // 未初始化
    BLE_PAIR_STATE_ADVERTISING,        // 正在广播
    BLE_PAIR_STATE_SCANNING,           // 正在扫描
    BLE_PAIR_STATE_ERROR               // 错误状态
} ble_pair_state_t;

// 回调函数类型
typedef void (*wifi_now_pair_cb_t)(const uint8_t* mac_addr, const char* name);
typedef void (*wifi_now_recv_cb_t)(const uint8_t* mac_addr, const uint8_t* data, int len);
typedef void (*wifi_now_send_cb_t)(const uint8_t* mac_addr, bool success);
```

---

## API接口摘要 (v2.0)

### BLE配对函数

| 函数 | 描述 |
|------|------|
| `ble_pairing_init()` | 初始化BLE配对服务 |
| `ble_pairing_deinit()` | 关闭BLE配对服务 |
| `ble_pairing_start_advertise(name)` | 开始BLE广播 |
| `ble_pairing_stop_advertise()` | 停止BLE广播 |
| `ble_pairing_is_advertising()` | 检查是否正在广播 |
| `ble_pairing_start_scan(sec)` | 开始BLE扫描（自动配对，sec为uint16_t） |
| `ble_pairing_stop_scan()` | 停止BLE扫描 |
| `ble_pairing_is_scanning()` | 检查是否正在扫描 |
| `ble_pairing_get_discovered(devs, max)` | 获取发现的设备列表 |
| `ble_pairing_pair_with_device(idx)` | 与指定设备配对（手动） |
| `ble_pairing_set_auto_pair(enable)` | 设置自动配对开关 (v2.0) |
| `ble_pairing_get_auto_pair()` | 获取自动配对状态 (v2.0) |
| `ble_pairing_get_own_now_mac()` | 获取本机ESP-NOW MAC |
| `ble_pairing_get_name(name_out)` | 获取本机设备名称 (v2.0) |
| `ble_pairing_get_state()` | 获取当前BLE状态 (v4.0) |
| `ble_pairing_start_adv_burst(name, sec)` | 开始限时广播模式 (v4.0) |
| `ble_pairing_stop_adv_burst()` | 停止限时广播模式 (v4.0) |
| `ble_pairing_is_burst_mode()` | 检查是否在限时广播模式 (v4.0) |
| `ble_pairing_set_name(name)` | 设置并持久化BLE设备名 (v4.0) |
| `ble_pairing_reset_controller()` | 硬复位BLE控制器 (v4.0) |

### ESP-NOW函数

| 函数 | 描述 |
|------|------|
| `wifi_now_init()` | 初始化ESP-NOW |
| `wifi_now_deinit()` | 关闭ESP-NOW |
| `wifi_now_is_initialized()` | 检查是否已初始化 |
| `wifi_now_add_peer(mac, ch)` | 添加节点 |
| `wifi_now_add_peer_with_name(mac, ch, name, peer_type)` | 添加节点（带名称和节点类型） |
| `wifi_now_remove_peer(mac)` | 移除节点 |
| `wifi_now_get_peer_count()` | 获取节点数量 |
| `wifi_now_is_peer_exists(mac)` | 检查节点是否存在 |
| `wifi_now_get_peer_list(peers, max)` | 获取节点列表 |
| `wifi_now_get_peers_json(buf, size)` | 获取节点列表JSON |
| `wifi_now_save_peers()` | 保存节点到NVS |
| `wifi_now_load_peers()` | 从NVS加载节点 |
| `wifi_now_clear_peers()` | 清除所有节点 |
| `wifi_now_send(mac, data, len)` | 发送数据 |
| `wifi_now_broadcast(data, len)` | 广播数据 |
| `wifi_now_get_channel()` | 获取当前信道 |
| `wifi_now_set_channel(ch)` | 设置信道 |
| `wifi_now_get_mac(mac_out)` | 获取本机MAC |
| `wifi_now_set_recv_callback(cb)` | 设置接收回调 |
| `wifi_now_set_send_callback(cb)` | 设置发送回调 |
| `wifi_now_set_pair_callback(cb)` | 设置配对回调 (v2.0) |
| `wifi_now_send_pair_request(mac)` | 发送配对请求 (v2.0) |
| `wifi_now_send_pair_response(mac)` | 发送配对响应 (v2.0) |
| `wifi_now_handle_pair_message(mac, data, len)` | 处理配对消息 (v2.0) |
| `wifi_now_send_unpair_request(mac)` | 发送解绑请求 (v3.0) |
| `wifi_now_send_unpair_response(mac)` | 发送解绑响应 (v3.0) |
| `wifi_now_handle_unpair_message(mac, data, len)` | 处理解绑消息 (v3.0) |
| `wifi_now_unpair_with_peer(mac)` | 解绑指定节点 (v3.0) |
| `wifi_now_set_unpair_callback(cb)` | 设置解绑回调 (v3.0) |
| `wifi_now_get_state()` | 获取ESP-NOW状态 (v4.0) |
| `wifi_now_is_peer_cached(mac)` | 检查NVS中是否存在peer (v4.0) |
| `wifi_now_get_pmk()` | 获取本机PMK指针 (v4.0) |
| `wifi_now_unbind_all()` | 一键解绑所有节点 (v4.0) |
| `wifi_now_update_peers_channel(ch)` | 更新所有peer的信道 (v4.0) |
| `wifi_now_format_peer_entry(idx, buf, size, comma)` | 格式化单个peer为JSON (v4.0) |
| `wifi_now_add_msg_template(name, data, len)` | 添加消息模板 (v4.0) |
| `wifi_now_update_msg_template(idx, name, data, len)` | 更新消息模板 (v4.0) |
| `wifi_now_remove_msg_template(idx)` | 删除消息模板 (v4.0) |
| `wifi_now_get_msg_template_count()` | 获取模板数量 (v4.0) |
| `wifi_now_get_msg_template(idx, out)` | 获取模板内容 (v4.0) |
| `wifi_now_format_msg_template(idx, buf, size, comma)` | 格式化模板为JSON (v4.0) |
| `wifi_now_get_msg_templates_json(buf, size)` | 获取模板列表JSON (v4.0) |
| `wifi_now_save_msg_templates()` | 保存模板到NVS (v4.0) |
| `wifi_now_load_msg_templates()` | 从NVS加载模板 (v4.0) |
| `wifi_now_add_recv_entry(mac, data, len)` | 添加接收历史条目 (v4.0) |
| `wifi_now_get_recv_count()` | 获取接收历史条数 (v4.0) |
| `wifi_now_get_recv_messages_json(buf, size)` | 获取接收历史JSON (v4.0) |
| `wifi_now_get_send_history_count()` | 获取发送历史条数 (v4.0) |
| `wifi_now_get_send_history_json(buf, size)` | 获取发送历史JSON (v4.0) |

---

## 自动配对控制 (v2.0)

### 启用/禁用自动配对

```c
// 启用自动配对（默认启用）
ble_pairing_set_auto_pair(true);

// 禁用自动配对
ble_pairing_set_auto_pair(false);

// 获取当前状态
bool auto_pair = ble_pairing_get_auto_pair();
```

### 自动配对说明

- **默认状态**: 自动配对是启用的
- **启用时**: 扫描发现设备后自动添加到peer，无需手动操作
- **禁用时**: 需要手动调用 `ble_pairing_pair_with_device()` 进行配对

### 扫描补扫机制 (v4.0)

ESP32-S3 BLE controller 存在内部状态机问题，在密集 BLE 环境中可能丢失部分设备的广播包。
本系统实现了自动补扫机制：

- 扫描结束后若发现设备少于 2 个，自动进行补扫（最多 3 次）
- 每次补扫前执行 BLE controller 硬复位
- 补扫持续 30 秒，使用被动扫描模式
- 补扫完成后统一处理发现的设备

---

## 多设备配对示例 (v2.0)

### 扫描端 → 多个广播端

```
设备B (扫描端)            设备A (广播端)      设备C (广播端)
    │                       │                    │
    │  BLE扫描               │                    │
    │ ─────────────────────►│                    │
    │ ◄─────────────────────│ BLE广播            │
    │                       │                    │
    │ 添加A为peer            │                    │
    │ 发送PAIR_REQUEST to A  │                    │
    │ ─────────────────────►│                    │
    │                       │                    │
    │  BLE扫描               │                    │
    │ ─────────────────────────────────────────►│
    │ ◄────────────────────────────────────────│
    │                       │                    │
    │ 添加C为peer            │                    │
    │ 发送PAIR_REQUEST to C  │                    │
    │ ─────────────────────────────────────────►│
    │                       │                    │
    结果: B有peers: A和C                              │
          A有peer: B                                  │
          C有peer: B                                  │
```

---

## 其他平台实现要点

### BLE广播实现清单

- [ ] 初始化BLE控制器（仅LE模式）
- [ ] 注册GAP回调处理器
- [ ] 实现带自定义MFG数据的扩展广播
- [ ] 实现带MFG数据解析的扩展扫描
- [ ] 按第3章格式解析厂商数据
- [ ] 处理广播和扫描事件

### ESP-NOW实现清单 (v2.0)

- [ ] 初始化ESP-NOW协议栈
- [ ] 注册发送和接收回调
- [ ] 实现节点管理（添加/移除）
- [ ] 实现PAIR_REQUEST/PAIR_RESPONSE消息处理
- [ ] 实现数据加密（可选，使用PMK）
- [ ] 处理节点发现和信道选择
- [ ] 实现节点持久化

### 关键实现注意事项

1. **字节序**: MAC地址和厂商ID使用小端序
2. **字符串编码**: UTF-8编码
3. **数据长度**: 广播数据总长度不超过31字节
4. **BLE版本**: 需要支持BLE 5.0扩展广播
5. **配对消息**: 需要在ESP-NOW初始化后才能使用

---

## 错误代码

| 代码 | 常量 | 描述 |
|------|------|------|
| 0 | SUCCESS | 操作成功 |
| -1 | FAILURE | 一般失败 |
| 1 | ERR_BT_INIT | BT控制器初始化失败 |
| 2 | ERR_BT_ENABLE | BT控制器使能失败 |
| 3 | ERR_BLUE_INIT | Bluedroid初始化失败 |
| 4 | ERR_ADV_PARAMS | 广播参数错误 |
| 5 | ERR_SCAN_PARAMS | 扫描参数错误 |
| 6 | ERR_PEER_EXISTS | 节点已存在 |
| 7 | ERR_PEER_NOT_FOUND | 节点未找到 |
| 8 | ERR_INVALID_MAGIC | 无效的配对消息魔数 |
| 9 | ERR_INVALID_TYPE | 无效的配对消息类型 |

---

## 修订历史

| 版本 | 日期 | 变更内容 |
|------|------|----------|
| 1.0 | 2026-05-17 | 初始版本 |
| 2.0 | 2026-05-18 | 添加自动配对协议 (v2.0) |
| 3.0 | 2026-05-18 | 添加退网解绑功能 (v3.0) |
| 4.0 | 2026-05-25 | 动态PMK同步、广播格式v2、扫描补扫机制、UNBIND_ALL协议、新增API函数 |

---

**详细文档请参阅**: `ESP_NOW_BLE_Discovery_Specification.md`
