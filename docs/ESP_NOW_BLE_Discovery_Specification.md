# ESP-NOW BLE Discovery and Pairing Service Specification
## ESP32-S3 WiFi Manager BLE Discovery Service Specification

**Document Version:** 3.0  
**Last Updated:** 2026-05-18  
**Target Platform:** ESP32-S3 (ESP-IDF v6.0.1)

---

## Table of Contents

1. [Overview](#1-overview)
2. [System Architecture](#2-system-architecture)
3. [BLE Advertising Protocol](#3-ble-advertising-protocol)
4. [ESP-NOW Protocol Specification](#4-esp-now-protocol-specification)
5. [Auto-Pairing Protocol](#5-auto-pairing-protocol)
6. **Unpairing Protocol (v3.0)** #6-unpairing-protocol-v30
7. [BLE Discovery Flow](#6-ble-discovery-flow)
8. [Pairing Process](#7-pairing-process)
9. [Unpairing Process (v3.0)](#8-unpairing-process-v30)
10. [API Reference](#9-api-reference)
11. [Implementation Guide for Other Platforms](#10-implementation-guide-for-other-platforms)
12. [Data Structures](#11-data-structures)
13. [Constants and Limits](#12-constants-and-limits)
14. [Example Implementation](#13-example-implementation)

---

## 1. Overview

This document describes the BLE-based discovery and automatic pairing service for ESP-NOW mesh networking. The system enables automatic discovery of nearby ESP-NOW devices through BLE advertising and facilitates peer pairing without manual MAC address configuration.

**Key Features:**
- **Automatic device discovery** via BLE Extended Advertising
- **Zero-configuration pairing** - auto-adds peers bidirectionally
- **ESP-NOW MAC exchange** through BLE and ESP-NOW PAIR messages
- **Multi-device support** - scanner can add multiple broadcasters
- **Automatic unpairing** - bidirectional peer removal with notification (v3.0)
- **Device naming and identification**
- **Persistent peer storage** in NVS

**What's New in v2.0:**
- Automatic bidirectional peer addition (scanner ↔ broadcaster)
- PAIR_REQUEST / PAIR_RESPONSE message protocol
- Scanner supports adding multiple broadcaster peers
- Broadcaster only adds scanner peers (not other broadcasters)

**What's New in v3.0:**
- **Automatic unpairing protocol** - UNPAIR_REQUEST / UNPAIR_RESPONSE messages
- **Bidirectional peer removal** - both devices remove each other
- **Unpair callback mechanism** - notify application layer of unpair events
- **Persistent unpairing** - unpair status persists across reboots

---

## 2. System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     ESP-NOW Network                          │
│  ┌─────────┐      ┌─────────┐      ┌─────────┐              │
│  │ Node A  │◄────►│ Node B  │◄────►│ Node C  │              │
│  └────┬────┘      └────┬────┘      └────┬────┘              │
│       │                │                │                    │
│       └────────────────┴────────────────┘                    │
│                    ESP-NOW (WiFi)                            │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                  BLE Discovery Layer                          │
│                                                              │
│  ┌─────────┐      BLE Scan      ┌─────────┐                 │
│  │ Scanner │◄──────────────────►│Broadcaster│                │
│  └─────────┘      BLE Adv       └─────────┘                 │
│                                                              │
│  Advertising Data: ESP-NOW MAC + Device Name                  │
│  Pair Protocol: PAIR_REQUEST / PAIR_RESPONSE                  │
└─────────────────────────────────────────────────────────────┘
```

**Protocol Stack:**
```
┌─────────────────────────────────────────┐
│     Application Layer (User Data)         │
├─────────────────────────────────────────┤
│     ESP-NOW Protocol (WiFi Layer 2)      │
│     └─ PAIR_REQUEST / PAIR_RESPONSE     │
├─────────────────────────────────────────┤
│     BLE GAP Extended Advertising         │
├─────────────────────────────────────────┤
│     BLE Controller (LE 5.0)             │
└─────────────────────────────────────────┘
```

---

## 3. BLE Advertising Protocol

### 3.1 Advertising Data Format

The device broadcasts the following data in BLE advertising packets:

#### Manufacturer Specific Data (AD Type: 0xFF)

| Offset | Length | Field | Description |
|--------|--------|-------|-------------|
| 0 | 2 | Manufacturer ID | `0x02E5` (Espressif Systems) |
| 2 | 2 | Protocol Marker | `"EN"` (0x45, 0x4E) - ESP-NOW Marker |
| 4 | 6 | ESP-NOW MAC | Device's WiFi MAC address |
| 10+ | N | Device Name | UTF-8 encoded device name |

**Example Hex Dump:**
```
02 E5      - Manufacturer ID (LE: 0x02E5)
45 4E      - Protocol Marker ("EN")
3C 0F 02 D1 E6 94  - ESP-NOW MAC Address
45 53 50 33 32 2D  - Device Name ("ESP32-")
53 33 2D 4E 4F 57  - Device Name ("S3-NOW")
```

### 3.2 Complete Advertising Packet Structure

```
┌──────────────────────────────────────────────────────────────┐
│ BLE Advertising Packet (Max 31 bytes)                        │
├──────────────────────────────────────────────────────────────┤
│ [Length: 2] [Type: 0x01] [Flags: 0x06]                      │
│   └─ LE General Discoverable + BR/EDR Not Supported           │
├──────────────────────────────────────────────────────────────┤
│ [Length: 1+N] [Type: 0x09] [Device Name...]                 │
│   └─ Complete Local Name                                     │
├──────────────────────────────────────────────────────────────┤
│ [Length: 1+M] [Type: 0xFF] [MFG Data...]                    │
│   └─ Manufacturer Specific Data                              │
└──────────────────────────────────────────────────────────────┘
```

### 3.3 Advertising Parameters

| Parameter | Value | Description |
|-----------|-------|-------------|
| Advertising Type | Extended Advertising (Legacy Ind) | `ESP_BLE_GAP_SET_EXT_ADV_PROP_LEGACY_IND` |
| Interval Min | 160 (100ms) | `adv_interval_min = 160` |
| Interval Max | 160 (100ms) | `adv_interval_max = 160` |
| Channel Map | ALL (0x07) | Use all 3 channels |
| Own Address Type | Public | Use public BLE address |
| Primary PHY | 1M | `ESP_BLE_GAP_PHY_1M` |
| Secondary PHY | 1M | `ESP_BLE_GAP_PHY_1M` |

---

## 4. ESP-NOW Protocol Specification

### 4.1 ESP-NOW Frame Format

```
┌────────────────────────────────────────────────────────────┐
│ MAC Header (26 bytes)                                      │
├──────────┬──────────┬──────────┬──────────┬───────────────┤
│ Dest MAC │ Src MAC  │ Type     │ Version  │     ...       │
│ (6 bytes)│ (6 bytes)│ (2 bytes)│ (1 byte) │               │
├──────────┴──────────┴──────────┴──────────┴───────────────┤
│ ESP-NOW Header (12 bytes)                                  │
├──────────┬──────────┬──────────┬──────────┬───────────────┤
│ Magic    │ Version  │ Reserved │ Device   │     ...       │
│ (4 bytes)│ (1 byte) │ (3 bytes)│ Type     │               │
├──────────┴──────────┴──────────┴──────────┴───────────────┤
│ Payload (0-250 bytes)                                      │
├────────────────────────────────────────────────────────────┤
│ FCS (4 bytes)                                              │
└────────────────────────────────────────────────────────────┘
```

### 4.2 ESP-NOW Parameters

| Parameter | Value | Description |
|-----------|-------|-------------|
| Max Data Length | 250 bytes | Per ESP-NOW specification |
| Max Peers | 20 | Maximum registered peers |
| Broadcast MAC | `FF:FF:FF:FF:FF:FF` | Broadcast address |
| Default Channel | 1 | WiFi channel for ESP-NOW |
| PMK | `"pmk1234567890123"` | Default PMK (if used) |

---

## 5. Auto-Pairing Protocol

### 5.1 Overview

The automatic pairing protocol enables bidirectional peer addition between devices discovered via BLE:

- **Scanner Role**: Can add multiple broadcasters as peers
- **Broadcaster Role**: Only adds scanners as peers (not other broadcasters)

### 5.2 Pair Message Structure

```c
typedef struct {
    uint32_t magic;        // 0x4553504E ("ESPN")
    uint8_t  type;         // PAIR_REQUEST (0x01) or PAIR_RESPONSE (0x02)
    uint8_t  mac[6];       // Sender's ESP-NOW MAC
    char     name[32];     // Device name (null-terminated)
} __attribute__((packed)) esp_now_pair_msg_t;
```

**Message Types:**

| Type | Value | Description |
|------|-------|-------------|
| PAIR_REQUEST | 0x01 | Request to add sender as peer |
| PAIR_RESPONSE | 0x02 | Response accepting peer addition |
| DATA | 0x10 | Regular data message |

### 5.3 Auto-Pairing Flow

```
Device A (Broadcaster)                Device B (Scanner)
      │                                     │
      │ 1. BLE Extended Advertising         │
      │    [AD: MAC + Name + MFG Data]    │
      │ ─────────────────────────────────►│
      │                                     │
      │                                     │ 2. Parse MFG Data
      │                                     │ 3. Extract ESP-NOW MAC
      │                                     │ 4. Add broadcaster as peer
      │                                     │    wifi_now_add_peer(A_MAC, channel)
      │                                     │
      │                                     │ 5. Send PAIR_REQUEST via ESP-NOW
      │ ◄─────────────────────────────────│
      │                                     │    [Magic: ESPN]
      │                                     │    [Type: PAIR_REQUEST]
      │                                     │    [MAC: B_MAC]
      │                                     │
      │ 6. Parse PAIR_REQUEST               │
      │ 7. Add scanner as peer              │
      │    wifi_now_add_peer(B_MAC, channel)│
      │                                     │
      │ 8. Send PAIR_RESPONSE via ESP-NOW   │
      │ ─────────────────────────────────►│
      │                                     │
      │                                     │ 9. Parse PAIR_RESPONSE
      │                                     │ 10. Save peers to NVS
      │                                     │
      ✓ Bidirectional ESP-NOW connection    ✓
         established successfully
```

### 5.4 Role Differentiation

#### Scanner Role (BLE Scan Initiator)
- Initiates BLE scanning
- **Can add multiple broadcasters as peers**
- Sends PAIR_REQUEST when discovering broadcasters
- Receives and processes PAIR_RESPONSE messages

#### Broadcaster Role (BLE Advertising)
- Sends BLE advertising packets
- **Only adds scanners as peers** (not other broadcasters)
- Receives PAIR_REQUEST and adds sender as peer
- Sends PAIR_RESPONSE after adding peer

### 5.5 Pairing State Machine

```
┌──────────────────────────────────────────────────────────────┐
│              Automatic Pairing State Machine                  │
├──────────────────────────────────────────────────────────────┤
│                                                               │
│  Scanner Discovery           Broadcaster Reception            │
│  ─────────────────           ────────────────────            │
│                                                               │
│  ┌───────────┐                                                │
│  │ DISCOVERED│◄──────── BLE Advertising Received              │
│  └─────┬─────┘                                                │
│        │                                                       │
│        │ Check if already paired                               │
│        │                                                       │
│        ├──────────────────────────────┐                        │
│        │                              │                        │
│        ▼                              ▼                        │
│  ┌───────────┐                 ┌───────────┐                  │
│  │ ALREADY   │                 │  NOT YET  │                  │
│  │  PAIRED   │                 │  PAIRED   │                  │
│  └───────────┘                 └─────┬─────┘                  │
│                                       │                        │
│                                       │ Add as peer            │
│                                       │ Send PAIR_REQUEST      │
│                                       ▼                        │
│                                ┌───────────┐                  │
│                                │  PENDING  │                  │
│                                │ (waiting) │                  │
│                                └─────┬─────┘                  │
│                                       │                        │
│                                       │ Receive PAIR_RESPONSE │
│                                       │                        │
│                                       ▼                        │
│                                ┌───────────┐                  │
│                                │   PAIRED  │                  │
│                                └───────────┘                  │
└──────────────────────────────────────────────────────────────┘
```

### 5.6 Auto-Pairing Control

The auto-pairing feature can be enabled/disabled via API:

```c
// Enable auto-pairing (default: enabled)
ble_pairing_set_auto_pair(true);

// Disable auto-pairing
ble_pairing_set_auto_pair(false);
```

When disabled, devices must be paired manually via `ble_pairing_pair_with_device()`.

---

## 6. Unpairing Protocol (v3.0)

### 6.1 Overview

The automatic unpairing protocol enables bidirectional peer removal between paired devices. When a device initiates unpairing, both devices will remove each other's peer entry:

- **Bidirectional removal** - both devices remove the peer entry
- **Notification mechanism** - UNPAIR_REQUEST / UNPAIR_RESPONSE messages
- **Callback notification** - notify application layer of unpair events
- **Persistent storage** - unpair status persists across reboots

### 6.2 Unpair Message Structure

The unpair protocol uses the same message structure as pairing:

```c
typedef struct {
    uint32_t magic;        // 0x4553504E ("ESPN")
    uint8_t  type;         // UNPAIR_REQUEST (0x03) or UNPAIR_RESPONSE (0x04)
    uint8_t  mac[6];       // Sender's ESP-NOW MAC
    char     name[32];     // Device name (null-terminated)
} __attribute__((packed)) esp_now_pair_msg_t;
```

**Message Types (v3.0):**

| Type | Value | Description |
|------|-------|-------------|
| UNPAIR_REQUEST | 0x03 | Request to unpair, remove sender as peer |
| UNPAIR_RESPONSE | 0x04 | Response confirming unpair completion |

### 6.3 Unpairing Flow

```
Device A (Initiator)                    Device B (Target)
      │                                       │
      │ 1. Call unpair function              │
      │    wifi_now_unpair_with_peer(B_MAC)   │
      │                                       │
      │ 2. Send UNPAIR_REQUEST via ESP-NOW    │
      │ ──────────────────────────────────► │
      │                                       │
      │                                       │ 3. Receive UNPAIR_REQUEST
      │                                       │ 4. Remove peer A
      │                                       │
      │                                       │ 5. Send UNPAIR_RESPONSE
      │ ◄───────────────────────────────── │
      │                                       │
      │ 6. Receive UNPAIR_RESPONSE           │
      │ 7. Remove peer B                     │
      │ 8. Save peers to NVS                 │
      │ 9. Trigger unpair callback           │
      │                                       │
      ✓ Bidirectional unpair complete!        ✓
```

### 6.4 Role Differentiation

#### Initiator Role
- Calls `wifi_now_unpair_with_peer()` to start unpairing
- Sends UNPAIR_REQUEST message
- Waits for UNPAIR_RESPONSE
- Removes peer's entry locally
- Triggers unpair callback

#### Target Role
- Receives UNPAIR_REQUEST message
- Automatically removes initiator from peer list
- Sends UNPAIR_RESPONSE message
- Triggers unpair callback

### 6.5 Unpairing State Machine

```
┌──────────────────────────────────────────────────────────────┐
│              Automatic Unpairing State Machine (v3.0)        │
├──────────────────────────────────────────────────────────────┤
│                                                               │
│  ┌───────────┐                                                │
│  │  PAIRED   │ ◄── Normal operation (paired state)            │
│  └─────┬─────┘                                                │
│        │                                                       │
│        │ wifi_now_unpair_with_peer() called                    │
│        │                                                       │
│        ▼                                                       │
│  ┌───────────┐                                                │
│  │ UNPAIRING │ ── Send UNPAIR_REQUEST ──►                      │
│  └─────┬─────┘                                                │
│        │                                                       │
│        │ Receive UNPAIR_RESPONSE                               │
│        │                                                       │
│        ▼                                                       │
│  ┌───────────┐                                                │
│  │ UNPAIRED  │ ── Remove peer, save NVS ──►                   │
│  └───────────┘                                                │
│        │                                                       │
│        │ Trigger unpair callback                               │
│        │                                                       │
│        ▼                                                       │
│  ┌───────────┐                                                │
│  │   IDLE    │ ◄── Ready for new pairing                      │
│  └───────────┘                                                │
└──────────────────────────────────────────────────────────────┘
```

### 6.6 Unpairing Callback

```c
// Define unpair callback
typedef void (*wifi_now_unpair_cb_t)(const uint8_t* mac_addr);

// Register unpair callback
void wifi_now_set_unpair_callback(wifi_now_unpair_cb_t cb);

// Example callback implementation
void on_unpair(const uint8_t* mac) {
    ESP_LOGI(TAG, "Device unpaired: " MACSTR, MAC2STR(mac));
    // Update UI, notify user, etc.
}

// Register callback during initialization
wifi_now_set_unpair_callback(on_unpair);
```

### 6.7 API Control

The unpairing feature can be triggered via API:

```c
// Unpair with a specific peer
uint8_t peer_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
bool success = wifi_now_unpair_with_peer(peer_mac);

if (success) {
    ESP_LOGI(TAG, "Unpair request sent successfully");
} else {
    ESP_LOGE(TAG, "Failed to send unpair request");
}

// Check if peer exists before unpairing
if (wifi_now_is_peer_exists(peer_mac)) {
    ESP_LOGI(TAG, "Peer exists, can unpair");
} else {
    ESP_LOGW(TAG, "Peer does not exist");
}
```

---

## 7. BLE Discovery Flow

### 6.1 Discovery State Machine

```
┌──────────┐    ble_pairing_init()    ┌──────────────────┐
│   OFF    │ ──────────────────────► │      IDLE        │
└──────────┘                         └────────┬─────────┘
                                                │
                            ble_pairing_start_advertise()
                                                │
                                                ▼
                                  ┌──────────────────────────┐
                                  │     ADVERTISING         │
                                  │   (BLE Extended Adv)     │
                                  └──────────┬───────────────┘
                                             │
                         ble_pairing_start_scan()
                                             │
                                             ▼
                                  ┌──────────────────────────┐
                                  │      SCANNING           │
                                  │  (Extended BLE Scan)     │
                                  └──────────┬───────────────┘
                                             │
                         Scan Timeout / Stop Scan
                                             │
                                             ▼
                                  ┌──────────────────────────┐
                                  │     AUTO-PAIRING        │
                                  │ (Automatic via messages)│
                                  └──────────────────────────┘
```

### 6.2 Scanning Parameters

| Parameter | Value | Description |
|-----------|-------|-------------|
| Scan Type | Active | `BLE_SCAN_TYPE_ACTIVE` |
| Scan Interval | 80 (50ms) | Time between scans |
| Scan Window | 48 (30ms) | Active scan duration |
| Duplicate Filter | Disabled | Report all devices |
| Filter Policy | Allow All | No whitelist/blacklist |
| Default Scan Duration | 10 seconds | Scan time before auto-stop |

---

## 7. Pairing Process

### 7.1 Automatic Pairing Flow

```
┌────────────────────────────────────────────────────────────────────┐
│                     Auto-Pairing Sequence (v2.0)                  │
├────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  User Action              Device A (Adv)     Device B (Scan)         │
│  ──────────              ─────────────    ────────────────          │
│                                                                      │
│  1. Start Scan        ────────►      Start Extended Scan           │
│                                                                      │
│  2. BLE Adv           ◄─────────     BLE Advertising               │
│                            [ADV: MAC,    with ESP-NOW MAC           │
│                             Name, MFG]                              │
│                                                                      │
│  3. Parse MFG         ────────►     Extract ESP-NOW MAC            │
│                                                                      │
│  4. Auto-Add Peer     ────────►     if (!already_paired) {         │
│                                          wifi_now_add_peer(A_MAC)   │
│                                          wifi_now_send_pair_request │
│                                      }                               │
│                                                                      │
│  5. PAIR_REQUEST      ◄─────────     [ESPN | REQUEST | B_MAC]     │
│                                                                      │
│  6. Auto-Add Peer     ◄─────────     wifi_now_add_peer(B_MAC)      │
│                                                                      │
│  7. PAIR_RESPONSE     ────────►     [ESPN | RESPONSE | A_MAC]      │
│                                                                      │
│  8. Save Peers        ────────►     wifi_now_save_peers()           │
│                                                                      │
│  9. ✓ Bidirectional connection established!                          │
│                                                                      │
└────────────────────────────────────────────────────────────────────┘
```

### 7.2 Multi-Device Pairing

**Scanner → Multiple Broadcasters:**

```
Device B (Scanner)                Device A (Adv)        Device C (Adv)
      │                               │                    │
      │  BLE Scan                     │                    │
      │ ─────────────────────────────►│                    │
      │ ◄─────────────────────────────│  BLE Advertising   │
      │                               │                    │
      │  Add A as peer                │                    │
      │  Send PAIR_REQUEST to A       │                    │
      │ ─────────────────────────────►│                    │
      │                               │                    │
      │                               │                    │
      │  BLE Scan                     │                    │
      │ ──────────────────────────────────────────────────►│
      │ ◄─────────────────────────────────────────────────│
      │                               │                    │
      │  Add C as peer                │                    │
      │  Send PAIR_REQUEST to C        │                    │
      │ ──────────────────────────────────────────────────►│
      │                               │                    │
      └───────────────────────────────┼────────────────────┘
                                      │
      Result: B has peers A and C     │
              A has peer B             │
              C has peer B             ▼
```

### 7.3 Peer Storage

Peers are persisted to NVS (Non-Volatile Storage) for automatic reconnection after reboot:

- **NVS Namespace:** `espnow_cfg`
- **Keys:**
  - `peer_cnt` - Number of peers
  - `peer_0` ... `peer_N` - Individual peer data
  - `ble_name` - Device's BLE advertising name

---

## 8. Unpairing Process (v3.0)

### 8.1 Automatic Unpairing Flow

```
┌────────────────────────────────────────────────────────────────────┐
│                     Auto-Unpairing Sequence (v3.0)                │
├────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  User Action              Device A          Device B                  │
│  ──────────              ─────────        ─────────                  │
│                                                                      │
│  1. Call unpair        ────────►      wifi_now_unpair_with_peer() │
│                                                                      │
│  2. Send UNPAIR_REQUEST ──────────►  [ESPN | UNPAIR | A_MAC]      │
│                                                                      │
│  3. Receive UNPAIR_REQUEST ◄──────                           │
│                                                                      │
│  4. Remove peer A       ◄──────      wifi_now_remove_peer(A_MAC)  │
│                                                                      │
│  5. Send UNPAIR_RESPONSE ──────────►  [ESPN | UNPAIR_RES | B_MAC]  │
│                                                                      │
│  6. Receive UNPAIR_RESPONSE ◄────────                           │
│                                                                      │
│  7. Remove peer B       ◄────────      wifi_now_remove_peer(B_MAC) │
│                                                                      │
│  8. Save peers          ◄────────      wifi_now_save_peers()       │
│                                                                      │
│  9. Trigger callback    ◄────────      wifi_now_unpair_cb()        │
│                                                                      │
│  10. ✓ Bidirectional unpair complete!                               │
│                                                                      │
└────────────────────────────────────────────────────────────────────┘
```

### 8.2 Multi-Device Unpairing

**Initiator → Multiple Peers:**

```
Device A (Initiator)         Device B           Device C
      │                        │                    │
      │ Unpair with B          │                    │
      │ ──────────────────────►│                    │
      │ 发送UNPAIR_REQUEST      │                    │
      │ ◄──────────────────────│                    │
      │ 收到UNPAIR_RESPONSE     │                    │
      │                        │                    │
      │                        │                    │
      │ Unpair with C          │                    │
      │ ───────────────────────────────────────────►│
      │ 发送UNPAIR_REQUEST      │                    │
      │ ◄──────────────────────────────────────────│
      │ 收到UNPAIR_RESPONSE     │                    │
      │                        │                    │
      └────────────────────────┼────────────────────┘
                               │
      Result: A has no peers   │
              B has no peers    │
              C has no peers    ▼
```

### 8.3 Unpairing Persistence

After unpairing, the peer list is saved to NVS:

- **NVS Namespace:** `espnow_cfg`
- **Updated Keys:**
  - `peer_cnt` - Decremented after unpairing
  - `peer_X` - Removed from peer list
  - Peers that unpaired will not reappear after reboot

### 8.4 Error Handling

```c
// Handle unpairing errors
bool wifi_now_unpair_with_peer(const uint8_t* mac_addr) {
    if (!mac_addr || !wifi_now_is_initialized()) {
        ESP_LOGE(TAG, "Invalid parameters for unpair");
        return false;
    }

    if (!wifi_now_is_peer_exists(mac_addr)) {
        ESP_LOGW(TAG, "Peer does not exist: " MACSTR, MAC2STR(mac_addr));
        return false;
    }

    ESP_LOGI(TAG, "Unpairing with peer " MACSTR, MAC2STR(mac_addr));

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

### 8.5 Unpair vs Remove Peer

| Feature | Unpair | Remove Peer |
|---------|--------|-------------|
| **Scope** | Bidirectional | Local only |
| **Notification** | Yes (via ESP-NOW) | No |
| **Message Type** | UNPAIR_REQUEST/RESPONSE | N/A |
| **Peer on other side** | Automatically removed | Still exists |
| **Callback** | Yes (on both sides) | No |

---

## 9. API Reference

### 8.1 BLE Pairing API

#### `ble_pairing_init()`
```c
void ble_pairing_init(void);
```
Initialize the BLE pairing service. Must be called before any other BLE functions.

**Requirements:**
- NVS flash must be initialized
- BT controller memory must be allocated for BLE only

#### `ble_pairing_deinit()`
```c
void ble_pairing_deinit(void);
```
Deinitialize the BLE pairing service and release resources.

#### `ble_pairing_start_advertise()`
```c
bool ble_pairing_start_advertise(const char* device_name);
```
Start BLE advertising with the specified device name.

**Parameters:**
- `device_name` - UTF-8 encoded device name (max 32 bytes)

**Returns:** `true` on success, `false` on failure

#### `ble_pairing_stop_advertise()`
```c
void ble_pairing_stop_advertise(void);
```
Stop BLE advertising.

#### `ble_pairing_start_scan()`
```c
bool ble_pairing_start_scan(uint8_t duration_sec);
```
Start BLE scanning for ESP-NOW devices with automatic pairing.

**Parameters:**
- `duration_sec` - Scan duration in seconds (max 255)

**Returns:** `true` on success, `false` on failure

**Note:** When auto-pairing is enabled, discovered devices are automatically added as peers.

#### `ble_pairing_stop_scan()`
```c
void ble_pairing_stop_scan(void);
```
Stop BLE scanning.

#### `ble_pairing_get_discovered()`
```c
int ble_pairing_get_discovered(ble_discovered_device_t* devices, int max_count);
```
Get list of discovered ESP-NOW devices.

**Parameters:**
- `devices` - Array to store discovered devices
- `max_count` - Maximum number of devices to retrieve

**Returns:** Number of devices actually discovered

#### `ble_pairing_pair_with_device()`
```c
bool ble_pairing_pair_with_device(int index);
```
Pair with a discovered device (manual pairing).

**Parameters:**
- `index` - Index of device in discovered list

**Returns:** `true` on success, `false` on failure

#### `ble_pairing_set_auto_pair()`
```c
void ble_pairing_set_auto_pair(bool enable);
```
Enable or disable automatic pairing.

**Parameters:**
- `enable` - `true` to enable auto-pairing, `false` to disable

#### `ble_pairing_get_auto_pair()`
```c
bool ble_pairing_get_auto_pair(void);
```
Get current auto-pairing status.

**Returns:** `true` if auto-pairing is enabled, `false` otherwise

#### `ble_pairing_get_own_now_mac()`
```c
uint8_t* ble_pairing_get_own_now_mac(void);
```
Get the device's ESP-NOW MAC address.

**Returns:** Pointer to 6-byte MAC address

#### `ble_pairing_get_name()`
```c
void ble_pairing_get_name(char* name_out);
```
Get the device's BLE advertising name.

**Parameters:**
- `name_out` - Buffer to store device name (must be at least 32 bytes)

### 8.2 ESP-NOW API

#### `wifi_now_init()`
```c
void wifi_now_init(void);
```
Initialize ESP-NOW and register callbacks.

#### `wifi_now_deinit()`
```c
void wifi_now_deinit(void);
```
Deinitialize ESP-NOW.

#### `wifi_now_add_peer()`
```c
bool wifi_now_add_peer(const uint8_t* mac_addr, uint8_t channel);
```
Add an ESP-NOW peer.

**Parameters:**
- `mac_addr` - 6-byte peer MAC address
- `channel` - WiFi channel (1-14)

**Returns:** `true` on success, `false` on failure

#### `wifi_now_add_peer_with_name()`
```c
bool wifi_now_add_peer_with_name(const uint8_t* mac_addr, uint8_t channel, const char* name);
```
Add an ESP-NOW peer with name.

**Parameters:**
- `mac_addr` - 6-byte peer MAC address
- `channel` - WiFi channel (1-14)
- `name` - Peer name (max 32 bytes)

**Returns:** `true` on success, `false` on failure

#### `wifi_now_remove_peer()`
```c
bool wifi_now_remove_peer(const uint8_t* mac_addr);
```
Remove an ESP-NOW peer.

#### `wifi_now_send()`
```c
int wifi_now_send(const uint8_t* mac_addr, const uint8_t* data, int len);
```
Send data to a specific peer.

**Parameters:**
- `mac_addr` - Destination MAC address
- `data` - Data payload (max 250 bytes)
- `len` - Data length

**Returns:** Bytes sent on success, -1 on failure

#### `wifi_now_broadcast()`
```c
int wifi_now_broadcast(const uint8_t* data, int len);
```
Broadcast data to all peers.

#### `wifi_now_set_recv_callback()`
```c
void wifi_now_set_recv_callback(wifi_now_recv_cb_t cb);
```
Register receive callback.

#### `wifi_now_set_send_callback()`
```c
void wifi_now_set_send_callback(wifi_now_send_cb_t cb);
```
Register send callback.

#### `wifi_now_set_pair_callback()`
```c
void wifi_now_set_pair_callback(wifi_now_pair_cb_t cb);
```
Register pair callback (called when peer is added via pairing).

**Parameters:**
- `cb` - Callback function: `void callback(const uint8_t* mac, const char* name)`

### 9.3 ESP-NOW Unpairing API (v3.0)

#### `wifi_now_send_unpair_request()`
```c
bool wifi_now_send_unpair_request(const uint8_t* dest_mac);
```
Send UNPAIR_REQUEST to a device.

**Parameters:**
- `dest_mac` - Destination MAC address

**Returns:** `true` on success, `false` on failure

#### `wifi_now_send_unpair_response()`
```c
bool wifi_now_send_unpair_response(const uint8_t* dest_mac);
```
Send UNPAIR_RESPONSE to a device.

**Parameters:**
- `dest_mac` - Destination MAC address

**Returns:** `true` on success, `false` on failure

#### `wifi_now_handle_unpair_message()`
```c
bool wifi_now_handle_unpair_message(const uint8_t* mac_addr, const uint8_t* data, int len);
```
Handle incoming UNPAIR_REQUEST or UNPAIR_RESPONSE message.

**Parameters:**
- `mac_addr` - Sender MAC address
- `data` - Message data
- `len` - Message length

**Returns:** `true` if message was processed, `false` otherwise

#### `wifi_now_unpair_with_peer()`
```c
bool wifi_now_unpair_with_peer(const uint8_t* mac_addr);
```
Unpair with a specific peer. This will send UNPAIR_REQUEST and remove the peer locally.

**Parameters:**
- `mac_addr` - MAC address of peer to unpair

**Returns:** `true` on success, `false` on failure

**Note:** Both devices will remove each other's peer entry.

#### `wifi_now_set_unpair_callback()`
```c
void wifi_now_set_unpair_callback(wifi_now_unpair_cb_t cb);
```
Register unpair callback (called when peer is removed via unpairing).

**Parameters:**
- `cb` - Callback function: `void callback(const uint8_t* mac)`

#### `wifi_now_send_pair_request()`
```c
bool wifi_now_send_pair_request(const uint8_t* dest_mac);
```
Send PAIR_REQUEST to a device.

**Parameters:**
- `dest_mac` - Destination MAC address

**Returns:** `true` on success, `false` on failure

#### `wifi_now_send_pair_response()`
```c
bool wifi_now_send_pair_response(const uint8_t* dest_mac);
```
Send PAIR_RESPONSE to a device.

**Parameters:**
- `dest_mac` - Destination MAC address

**Returns:** `true` on success, `false` on failure

#### `wifi_now_handle_pair_message()`
```c
bool wifi_now_handle_pair_message(const uint8_t* mac_addr, const uint8_t* data, int len);
```
Handle incoming PAIR_REQUEST or PAIR_RESPONSE message.

**Parameters:**
- `mac_addr` - Sender MAC address
- `data` - Message data
- `len` - Message length

**Returns:** `true` if message was processed, `false` otherwise

#### `wifi_now_save_peers()`
```c
void wifi_now_save_peers(void);
```
Save current peer list to NVS.

#### `wifi_now_get_peers_json()`
```c
void wifi_now_get_peers_json(char* buffer, size_t buffer_size);
```
Get peer list as JSON string.

**Parameters:**
- `buffer` - Buffer to store JSON
- `buffer_size` - Buffer size

---

## 9. Implementation Guide for Other Platforms

### 9.1 Required Components

For implementing this protocol on other platforms:

1. **BLE Stack** - Must support BLE 5.0 Extended Advertising
2. **ESP-NOW Protocol** - Must implement ESP-NOW protocol stack
3. **NVS Storage** - For peer persistence

### 9.2 BLE Implementation Checklist

- [ ] Initialize BLE controller for LE only (disable BR/EDR)
- [ ] Register GAP callback handler
- [ ] Implement extended advertising with custom MFG data
- [ ] Implement extended scanning with MFG data parsing
- [ ] Parse manufacturer data according to format in Section 3.1
- [ ] Handle advertising and scan events

### 9.3 ESP-NOW Implementation Checklist

- [ ] Initialize ESP-NOW protocol stack
- [ ] Register send and receive callbacks
- [ ] Implement peer management (add/remove)
- [ ] Implement PAIR_REQUEST / PAIR_RESPONSE message handling
- [ ] Implement data encryption (optional, using PMK)
- [ ] Handle peer discovery and channel selection
- [ ] Implement peer persistence

### 9.4 Key Implementation Notes

#### BLE Advertising

When building the advertising packet:

```c
// Manufacturer Data Structure
uint8_t mfg_data[32];
mfg_data[0] = 0xE5;  // MFG ID LSB (0x02E5)
mfg_data[1] = 0x02;  // MFG ID MSB
mfg_data[2] = 'E';   // Protocol Marker
mfg_data[3] = 'N';   // Protocol Marker
memcpy(&mfg_data[4], espnow_mac, 6);  // ESP-NOW MAC
strcpy(&mfg_data[10], device_name);    // Device Name
```

#### BLE Scanning

When parsing scan response:

```c
// Parse manufacturer data from scan result
if (ad_type == 0xFF && data_len >= 12) {
    uint16_t mfg_id = data[0] | (data[1] << 8);
    if (mfg_id == 0x02E5 && data[2] == 'E' && data[3] == 'N') {
        // Valid ESP-NOW device found
        memcpy(espnow_mac, &data[4], 6);
        // Device name starts at data[10]
    }
}
```

#### Auto-Pairing Message Format

```c
// PAIR_REQUEST message
typedef struct {
    uint32_t magic;     // 0x4553504E ("ESPN")
    uint8_t  type;      // 0x01 (PAIR_REQUEST)
    uint8_t  mac[6];    // Sender's ESP-NOW MAC
    char     name[32];  // Device name
} esp_now_pair_msg_t;

// Send via ESP-NOW
esp_now_send(peer_mac, &pair_msg, sizeof(pair_msg));
```

---

## 10. Data Structures

### 10.1 BLE Discovery Device Structure

```c
typedef struct {
    uint8_t mac[6];      // BLE MAC address
    uint8_t now_mac[6];  // ESP-NOW MAC address
    char    name[32];    // Device name
    int     rssi;        // Signal strength
} ble_discovered_device_t;
```

### 10.2 ESP-NOW Peer Information

```c
typedef struct {
    uint8_t mac[6];      // Peer MAC address
    int     channel;     // WiFi channel
    char    name[32];    // Peer name
} wifi_now_peer_info_t;
```

### 10.3 Pair Message Structure

```c
#define ESP_NOW_MSG_PAIR_REQUEST   0x01
#define ESP_NOW_MSG_PAIR_RESPONSE  0x02
#define ESP_NOW_MSG_DATA           0x10
#define ESP_NOW_PAIR_MAGIC         0x4553504E  // "ESPN"

typedef struct {
    uint32_t magic;        // Magic number: 0x4553504E
    uint8_t  type;         // Message type
    uint8_t  mac[6];       // Sender's ESP-NOW MAC
    char     name[32];     // Device name
} __attribute__((packed)) esp_now_pair_msg_t;
```

### 10.4 Callback Types

```c
// Receive callback - called when data is received
typedef void (*wifi_now_recv_cb_t)(
    const uint8_t* mac_addr,  // Sender MAC
    const uint8_t* data,      // Data payload
    int data_len              // Data length
);

// Send callback - called when send completes
typedef void (*wifi_now_send_cb_t)(
    const uint8_t* mac_addr,  // Destination MAC
    bool success               // Send status
);

// Pair callback - called when peer is added via pairing
typedef void (*wifi_now_pair_cb_t)(
    const uint8_t* mac_addr,  // Peer MAC
    const char* name          // Peer name
);
```

---

## 11. Constants and Limits

### 11.1 System Limits

| Constant | Value | Description |
|----------|-------|-------------|
| `BLE_MAX_DISCOVERED` | 16 | Maximum discovered devices |
| `BLE_DEV_NAME_MAX` | 32 | Maximum device name length |
| `ESP_NOW_MAX_PEERS` | 20 | Maximum ESP-NOW peers |
| `ESP_NOW_MAX_DATA_LEN` | 250 | Maximum ESP-NOW payload |
| `ESP_NOW_PEER_NAME_MAX` | 32 | Maximum peer name length |

### 11.2 Protocol Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `BLE_MFG_ID` | 0x02E5 | Espressif manufacturer ID |
| `BLE_TAG_MARKER_0` | 'E' | Protocol marker byte 0 |
| `BLE_TAG_MARKER_1` | 'N' | Protocol marker byte 1 |
| `ESP_NOW_PAIR_MAGIC` | 0x4553504E | "ESPN" magic number |
| `ESP_NOW_MSG_PAIR_REQUEST` | 0x01 | Pair request type |
| `ESP_NOW_MSG_PAIR_RESPONSE` | 0x02 | Pair response type |

### 11.3 BLE Parameters

| Parameter | Value |
|-----------|-------|
| Advertising Interval | 100ms (160 * 0.625ms) |
| Scan Interval | 50ms (80 * 0.625ms) |
| Scan Window | 30ms (48 * 0.625ms) |
| Default Scan Duration | 10 seconds |

---

## 12. Example Implementation

### 12.1 Platform-Specific BLE Implementation

#### Arduino/ESP32 Implementation

```cpp
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>

#define MFG_ID         0x02E5
#define PROTOCOL_MARKER "EN"

// Custom advertising data builder
class ESPNowBLEAdvertiser {
private:
    uint8_t m_espnow_mac[6];
    String m_device_name;
    
public:
    void setMAC(const uint8_t* mac) {
        memcpy(m_espnow_mac, mac, 6);
    }
    
    void setName(const String& name) {
        m_device_name = name;
    }
    
    std::vector<uint8_t> buildMFGData() {
        std::vector<uint8_t> data;
        // MFG ID (little-endian)
        data.push_back(MFG_ID & 0xFF);
        data.push_back((MFG_ID >> 8) & 0xFF);
        // Protocol marker
        data.push_back(PROTOCOL_MARKER[0]);
        data.push_back(PROTOCOL_MARKER[1]);
        // ESP-NOW MAC
        for (int i = 0; i < 6; i++) {
            data.push_back(m_espnow_mac[i]);
        }
        // Device name
        for (char c : m_device_name) {
            data.push_back(c);
        }
        return data;
    }
};
```

#### STM32/WiFi MCU Implementation

For STM32 with external BLE module:

```c
// Build ESP-NOW advertisement
typedef struct {
    uint16_t mfg_id;        // 0x02E5
    uint8_t  marker[2];     // "EN"
    uint8_t  espnow_mac[6]; // ESP-NOW MAC
    char     name[];        // Variable length name
} __attribute__((packed)) espnow_adv_data_t;
```

### 12.2 Auto-Pairing Message Implementation

```c
// Send PAIR_REQUEST via ESP-NOW
void send_pair_request(const uint8_t* dest_mac) {
    esp_now_pair_msg_t msg;
    msg.magic = ESP_NOW_PAIR_MAGIC;  // 0x4553504E
    msg.type = ESP_NOW_MSG_PAIR_REQUEST;
    get_espnow_mac(msg.mac);
    get_device_name(msg.name);
    
    esp_now_send(dest_mac, (uint8_t*)&msg, sizeof(msg));
}

// Handle incoming pair message
bool handle_pair_message(const uint8_t* src_mac, const uint8_t* data, int len) {
    if (len < sizeof(esp_now_pair_msg_t)) return false;
    
    esp_now_pair_msg_t* msg = (esp_now_pair_msg_t*)data;
    
    if (msg->magic != ESP_NOW_PAIR_MAGIC) return false;
    
    if (msg->type == ESP_NOW_MSG_PAIR_REQUEST) {
        // Add sender as peer
        if (!is_peer_exists(msg->mac)) {
            add_peer(msg->mac, get_channel(), msg->name);
            save_peers();
        }
        // Send response
        send_pair_response(msg->mac);
        // Notify callback
        if (pair_callback) {
            pair_callback(msg->mac, msg->name);
        }
        return true;
    }
    
    if (msg->type == ESP_NOW_MSG_PAIR_RESPONSE) {
        // Add sender as peer
        if (!is_peer_exists(msg->mac)) {
            add_peer(msg->mac, get_channel(), msg->name);
            save_peers();
        }
        // Notify callback
        if (pair_callback) {
            pair_callback(msg->mac, msg->name);
        }
        return true;
    }
    
    return false;
}
```

### 12.3 Cross-Platform Considerations

1. **Byte Order:** MAC addresses and MFG IDs use little-endian format
2. **String Encoding:** UTF-8 for device names
3. **Maximum Length:** Keep total advertising data under 31 bytes
4. **Memory Alignment:** Structure packing may be needed for custom MCUs

### 12.4 Testing Checklist

- [x] BLE advertising with correct MFG data format
- [x] BLE scanning detects ESP-NOW devices
- [x] ESP-NOW MAC correctly extracted from MFG data
- [x] Auto-pairing adds peers bidirectionally
- [x] PAIR_REQUEST / PAIR_RESPONSE messages work correctly
- [x] Scanner can add multiple broadcasters as peers
- [x] Broadcaster only adds scanners as peers
- [x] Data transmission between paired devices
- [x] Peer persistence after reboot
- [x] Multi-device discovery (test with 3+ devices)
- [x] Unpairing removes peers bidirectionally (v3.0)
- [x] UNPAIR_REQUEST / UNPAIR_RESPONSE messages work correctly (v3.0)
- [x] Unpair callback is triggered on both sides (v3.0)
- [x] Peer persistence after unpairing (v3.0)

---

## Appendix A: Error Codes

| Code | Constant | Description |
|------|----------|-------------|
| 0 | SUCCESS | Operation successful |
| -1 | FAILURE | General failure |
| 1 | ERR_BT_INIT | BT controller init failed |
| 2 | ERR_BT_ENABLE | BT controller enable failed |
| 3 | ERR_BLUE_INIT | Bluedroid init failed |
| 4 | ERR_ADV_PARAMS | Advertising params error |
| 5 | ERR_SCAN_PARAMS | Scan params error |
| 6 | ERR_PEER_EXISTS | Peer already exists |
| 7 | ERR_PEER_NOT_FOUND | Peer not found |
| 8 | ERR_INVALID_MAGIC | Invalid pair message magic |
| 9 | ERR_INVALID_TYPE | Invalid pair message type |

---

## Appendix B: Revision History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-05-17 | System | Initial specification |
| 2.0 | 2026-05-18 | System | Added auto-pairing protocol (v2.0) |
| 3.0 | 2026-05-18 | System | Added unpairing protocol (v3.0) |

---

**End of Document**
