#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "wifi_now.h"
#include "ble_pairing.h"
#include "role_control.h"

static const char* TAG = "WIFI_NOW";
static const char* NVS_NS        = "espnow_cfg";
static const char* NVS_KEY_PMK  = "pmk";
static const char* NVS_KEY_COUNT = "peer_cnt";
static const char* NVS_KEY_PEER  = "peer_";
static const char* NVS_KEY_MSG_COUNT = "msg_cnt";
static const char* NVS_KEY_MSG  = "msg_";

static const uint8_t s_bcast_mac[] = ESP_NOW_BCAST_MAC;

static wifi_now_state_t s_state         = WIFI_NOW_STATE_IDLE;
static SemaphoreHandle_t s_state_mutex  = NULL;
static uint8_t           s_channel      = 1;

static wifi_now_peer_info_t s_peer_cache[ESP_NOW_MAX_PEERS];
static int                  s_peer_cache_count = 0;

static wifi_now_msg_template_t s_msg_cache[ESP_NOW_MAX_MSG_TEMPLATES];
static int                     s_msg_cache_count = 0;

static wifi_now_recv_cb_t s_recv_cb = NULL;
static wifi_now_send_cb_t s_send_cb = NULL;
static wifi_now_pair_cb_t s_pair_cb = NULL;
static wifi_now_unpair_cb_t s_unpair_cb = NULL;

// 自动生成的 PMK (16字节)
static uint8_t s_pmk[16] = {0};

// 接收历史环形缓冲
static wifi_now_recv_entry_t s_recv_ring[ESP_NOW_RECV_HISTORY_MAX];
static int s_recv_ring_head = 0;
static int s_recv_ring_count = 0;

// 发送历史环形缓冲
static wifi_now_send_entry_t s_send_history[ESP_NOW_SEND_HISTORY_MAX];
static int s_send_history_head = 0;
static int s_send_history_count = 0;

static void send_ack_to_peer(const uint8_t* dest_mac)
{
    if (!dest_mac || !wifi_now_is_initialized()) return;

    esp_now_pair_msg_t ack_msg;
    memset(&ack_msg, 0, sizeof(ack_msg));
    ack_msg.magic = ESP_NOW_PAIR_MAGIC;
    ack_msg.type = ESP_NOW_MSG_ACK;
    wifi_now_get_mac(ack_msg.mac);

    int len = sizeof(esp_now_pair_msg_t);
    esp_now_send(dest_mac, (const uint8_t*)&ack_msg, len);
    ESP_LOGD(TAG, "ACK sent to " MACSTR, MAC2STR(dest_mac));
}

// 处理收到的ACK: 标记最近一条未确认的发送条目为已确认
static void handle_ack_message(const uint8_t* src_mac)
{
    if (!src_mac) return;

    // 从最新到最旧扫描发送历史，找到第一条匹配MAC且未acked的成功条目
    for (int i = 0; i < s_send_history_count; i++) {
        int idx = (s_send_history_head - 1 - i + ESP_NOW_SEND_HISTORY_MAX) % ESP_NOW_SEND_HISTORY_MAX;
        wifi_now_send_entry_t* entry = &s_send_history[idx];
        if (memcmp(entry->mac, src_mac, 6) == 0 && entry->success && !entry->acked) {
            entry->acked = true;
            ESP_LOGI(TAG, "ACK received from " MACSTR ", send entry %d confirmed",
                     MAC2STR(src_mac), idx);
            return;
        }
    }
    ESP_LOGW(TAG, "ACK from " MACSTR " but no matching send entry found",
             MAC2STR(src_mac));
}

static void espnow_recv_cb(const esp_now_recv_info_t* info,
                            const uint8_t* data, int data_len)
{
    if (!info || !data || data_len <= 0) return;

    // 检查是否为ACK消息
    if (data_len >= (int)sizeof(esp_now_pair_msg_t)) {
        esp_now_pair_msg_t* msg = (esp_now_pair_msg_t*)data;
        if (msg->magic == ESP_NOW_PAIR_MAGIC && msg->type == ESP_NOW_MSG_ACK) {
            handle_ack_message(info->src_addr);
            return;
        }
    }

    ESP_LOGI(TAG, "Recv %d bytes from " MACSTR,
             data_len, MAC2STR(info->src_addr));

    // 调试：dump 收到的原始字节
    char hex_dump[512] = {0};
    int hex_pos = 0;
    hex_pos += snprintf(hex_dump, sizeof(hex_dump), "dest=");
    if (info->des_addr) {
        hex_pos += snprintf(hex_dump + hex_pos, sizeof(hex_dump) - hex_pos,
                            MACSTR, MAC2STR(info->des_addr));
    } else {
        hex_pos += snprintf(hex_dump + hex_pos, sizeof(hex_dump) - hex_pos, "NULL");
    }
    hex_pos += snprintf(hex_dump + hex_pos, sizeof(hex_dump) - hex_pos, " data=");
    for (int i = 0; i < data_len && hex_pos < (int)sizeof(hex_dump) - 8; i++) {
        hex_pos += snprintf(hex_dump + hex_pos, sizeof(hex_dump) - hex_pos,
                            "%02X ", data[i]);
    }
    ESP_LOGI(TAG, "Recv %d bytes: %s", data_len, hex_dump);

    // 检查是否为配对/解绑控制消息
    bool is_control_msg = false;
    if (data_len >= (int)sizeof(esp_now_pair_msg_t)) {
        if (wifi_now_handle_pair_message(info->src_addr, data, data_len)) {
            is_control_msg = true;
        }
        if (wifi_now_handle_unpair_message(info->src_addr, data, data_len)) {
            is_control_msg = true;
        }
    }

    if (!is_control_msg) {
        // 非控制消息：自动存入接收历史
        wifi_now_add_recv_entry(info->src_addr, data, data_len);

        // 通知角色控制（LED闪一下等）
        role_control_notify_data();

        // 自动将未知发送方添加为peer（确保双向通信）
        if (!wifi_now_is_peer_exists(info->src_addr)) {
            ESP_LOGI(TAG, "Auto-adding unknown sender as peer: " MACSTR,
                     MAC2STR(info->src_addr));
            if (wifi_now_add_peer(info->src_addr, s_channel)) {
                wifi_now_save_peers();
                // 发送配对请求给对方，交换名称信息
                wifi_now_send_pair_request(info->src_addr);
            }
        }

        // 自动回复ACK给已知peer（不回复广播）
        if (info->des_addr && memcmp(info->des_addr, s_bcast_mac, 6) != 0 && wifi_now_is_peer_exists(info->src_addr)) {
            send_ack_to_peer(info->src_addr);
        }
    }

    if (!is_control_msg && s_recv_cb) {
        s_recv_cb(info->src_addr, data, data_len);
    }
}

// 跟踪最近一次发送的数据长度，供 send_cb 使用
static int s_last_send_len = 0;

static void espnow_send_cb(const esp_now_send_info_t* tx_info,
                            esp_now_send_status_t status)
{
    if (!tx_info) return;
    const char* status_str = (status == ESP_NOW_SEND_SUCCESS) ? "OK" : "FAIL";
    ESP_LOGI(TAG, "Send to " MACSTR " %s", MAC2STR(tx_info->des_addr), status_str);

    // 自动记录发送历史
    bool is_bcast = (memcmp(tx_info->des_addr, s_bcast_mac, 6) == 0);
    wifi_now_add_send_entry(tx_info->des_addr, s_last_send_len,
                             status == ESP_NOW_SEND_SUCCESS, is_bcast);

    if (s_send_cb) {
        s_send_cb(tx_info->des_addr, status == ESP_NOW_SEND_SUCCESS);
    }
}

static void load_peers_from_nvs(void)
{
    s_peer_cache_count = 0;

    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READONLY, &handle) != ESP_OK) return;

    uint8_t count = 0;
    if (nvs_get_u8(handle, NVS_KEY_COUNT, &count) != ESP_OK) {
        nvs_close(handle);
        return;
    }

    if (count > ESP_NOW_MAX_PEERS) count = ESP_NOW_MAX_PEERS;

    for (int i = 0; i < count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%s%d", NVS_KEY_PEER, i);
        size_t len = sizeof(wifi_now_peer_info_t);
        if (nvs_get_blob(handle, key, &s_peer_cache[i], &len) == ESP_OK) {
            s_peer_cache_count++;
        }
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Loaded %d peers from NVS", s_peer_cache_count);
}

static void save_peers_to_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READWRITE, &handle) != ESP_OK) return;

    nvs_set_u8(handle, NVS_KEY_COUNT, (uint8_t)s_peer_cache_count);

    for (int i = 0; i < s_peer_cache_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%s%d", NVS_KEY_PEER, i);
        nvs_set_blob(handle, key, &s_peer_cache[i], sizeof(wifi_now_peer_info_t));
    }

    nvs_commit(handle);
    nvs_close(handle);
}

static int find_peer_in_cache(const uint8_t* mac_addr)
{
    for (int i = 0; i < s_peer_cache_count; i++) {
        if (memcmp(s_peer_cache[i].mac, mac_addr, 6) == 0) return i;
    }
    return -1;
}

// 加载/生成 PMK：首次运行时随机数(8字节)+时间戳(8字节)=16字节，永久保存
static void wifi_now_init_pmk(void)
{
    nvs_handle_t handle;
    size_t len = 16;
    if (nvs_open(NVS_NS, NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_blob(handle, NVS_KEY_PMK, s_pmk, &len) == ESP_OK && len == 16) {
            nvs_close(handle);
            ESP_LOGI(TAG, "PMK loaded from NVS");
            return;
        }
        nvs_close(handle);
    }

    // 首次运行：8字节随机数 + 8字节时间戳
    esp_fill_random(s_pmk, 8);
    uint64_t ts = esp_timer_get_time();
    memcpy(s_pmk + 8, &ts, 8);

    if (nvs_open(NVS_NS, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_blob(handle, NVS_KEY_PMK, s_pmk, 16);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "New PMK generated and saved to NVS");
    }
}

// 保存接收到对端 PMK 到 NVS
static void wifi_now_set_pmk_from_peer(const uint8_t* pmk)
{
    if (!pmk) return;
    memcpy(s_pmk, pmk, 16);
    esp_now_set_pmk(s_pmk);
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_blob(handle, NVS_KEY_PMK, s_pmk, 16);
        nvs_commit(handle);
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "PMK updated from peer");
}

const uint8_t* wifi_now_get_pmk(void)
{
    return s_pmk;
}

void wifi_now_init(void)
{
    if (s_state == WIFI_NOW_STATE_INIT) {
        ESP_LOGW(TAG, "Already initialized");
        return;
    }

    if (!s_state_mutex) {
        s_state_mutex = xSemaphoreCreateMutex();
    }

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_NULL) {
        ESP_LOGE(TAG, "WiFi not started, init ESP-NOW failed");
        s_state = WIFI_NOW_STATE_ERROR;
        return;
    }

    // 初始化 PMK（首次运行自动生成并保存到 NVS）
    wifi_now_init_pmk();

    esp_err_t ret = esp_now_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %d (%s)", ret, esp_err_to_name(ret));
        s_state = WIFI_NOW_STATE_ERROR;
        return;
    }

    esp_now_register_recv_cb(espnow_recv_cb);
    esp_now_register_send_cb(espnow_send_cb);

    esp_now_set_pmk(s_pmk);

    wifi_second_chan_t second_ch = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&s_channel, &second_ch);

    // 没有WiFi STA连接时固定到信道6，不跟随WiFi默认信道
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        if (s_channel != 6) {
            s_channel = 6;
            esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);
            ESP_LOGI(TAG, "No STA connected, defaulting to channel 6");
        }
    }

    s_state = WIFI_NOW_STATE_INIT;

    load_peers_from_nvs();
    wifi_now_load_msg_templates();

    for (int i = 0; i < s_peer_cache_count; i++) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, s_peer_cache[i].mac, 6);
        peer.channel = (uint8_t)s_peer_cache[i].channel;
        peer.ifidx   = WIFI_IF_STA;
        peer.encrypt = false;
        if (esp_now_add_peer(&peer) == ESP_OK) {
            ESP_LOGI(TAG, "Restored peer: " MACSTR " ch=%d name=%s",
                     MAC2STR(s_peer_cache[i].mac),
                     s_peer_cache[i].channel,
                     s_peer_cache[i].name);
        }
    }

    esp_now_peer_info_t bcast_peer = {};
    memcpy(bcast_peer.peer_addr, s_bcast_mac, 6);
    bcast_peer.channel = s_channel;
    bcast_peer.ifidx   = WIFI_IF_STA;
    bcast_peer.encrypt = false;
    if (esp_now_add_peer(&bcast_peer) == ESP_OK) {
        ESP_LOGI(TAG, "Added broadcast peer");
    } else {
        ESP_LOGW(TAG, "Broadcast peer may already exist");
    }

    ESP_LOGI(TAG, "ESP-NOW initialized, channel=%d, peers=%d",
             s_channel, s_peer_cache_count);
}

void wifi_now_deinit(void)
{
    if (s_state != WIFI_NOW_STATE_INIT) return;

    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();

    s_recv_cb = NULL;
    s_send_cb = NULL;
    s_state   = WIFI_NOW_STATE_IDLE;

    ESP_LOGI(TAG, "ESP-NOW deinitialized");
}

bool wifi_now_is_initialized(void)
{
    return s_state == WIFI_NOW_STATE_INIT;
}

wifi_now_state_t wifi_now_get_state(void)
{
    return s_state;
}

void wifi_now_set_recv_callback(wifi_now_recv_cb_t cb)
{
    s_recv_cb = cb;
}

void wifi_now_set_send_callback(wifi_now_send_cb_t cb)
{
    s_send_cb = cb;
}

bool wifi_now_add_peer(const uint8_t* mac_addr, uint8_t channel)
{
    return wifi_now_add_peer_with_name(mac_addr, channel, "", PEER_TYPE_ESPNOW);
}

bool wifi_now_add_peer_with_name(const uint8_t* mac_addr, uint8_t channel,
                                  const char* name, uint8_t peer_type)
{
    if (s_state != WIFI_NOW_STATE_INIT) {
        ESP_LOGE(TAG, "ESP-NOW not initialized");
        return false;
    }

    if (!mac_addr) return false;

    if (esp_now_is_peer_exist(mac_addr)) {
        int idx = find_peer_in_cache(mac_addr);
        if (idx >= 0) {
            bool updated = false;
            if (name && name[0]) {
                strncpy(s_peer_cache[idx].name, name, ESP_NOW_PEER_NAME_MAX - 1);
                updated = true;
            }
            if (s_peer_cache[idx].channel != (int)channel) {
                s_peer_cache[idx].channel = channel;
                // 更新 ESP-NOW 驱动的 peer 信息（信道等）
                esp_now_peer_info_t peer_info;
                memset(&peer_info, 0, sizeof(peer_info));
                memcpy(peer_info.peer_addr, mac_addr, 6);
                peer_info.channel = channel;
                peer_info.ifidx   = WIFI_IF_STA;
                peer_info.encrypt = false;
                if (esp_now_mod_peer(&peer_info) != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to modify peer " MACSTR " ch=%d",
                             MAC2STR(mac_addr), channel);
                }
                updated = true;
            }
            if (updated) {
                save_peers_to_nvs();
            }
        }
        ESP_LOGI(TAG, "Peer " MACSTR " already exists, updated ch=%d",
                 MAC2STR(mac_addr), channel);
        return true;
    }

    if (s_peer_cache_count >= ESP_NOW_MAX_PEERS) {
        ESP_LOGE(TAG, "Peer list full (%d)", ESP_NOW_MAX_PEERS);
        return false;
    }

    wifi_interface_t ifidx = WIFI_IF_STA;
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        if (mode == WIFI_MODE_AP) {
            ifidx = WIFI_IF_AP;
            ESP_LOGI(TAG, "WiFi mode: AP, using WIFI_IF_AP");
        } else if (mode == WIFI_MODE_STA) {
            ifidx = WIFI_IF_STA;
            ESP_LOGI(TAG, "WiFi mode: STA, using WIFI_IF_STA");
        } else if (mode == WIFI_MODE_APSTA) {
            ifidx = WIFI_IF_STA;
            ESP_LOGI(TAG, "WiFi mode: APSTA, using WIFI_IF_STA");
        } else {
            ifidx = WIFI_IF_STA;
            ESP_LOGW(TAG, "WiFi mode unknown, using WIFI_IF_STA");
        }
    } else {
        ESP_LOGW(TAG, "Failed to get WiFi mode, using WIFI_IF_STA");
    }

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac_addr, 6);
    peer.channel = channel;
    peer.ifidx   = ifidx;
    peer.encrypt = false;

    ESP_LOGI(TAG, "Adding peer " MACSTR " on channel %d (ifidx=%d)",
             MAC2STR(mac_addr), channel, ifidx);

    esp_err_t ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Add peer " MACSTR " failed: %d (%s)",
                 MAC2STR(mac_addr), ret, esp_err_to_name(ret));
        return false;
    }

    wifi_now_peer_info_t* info = &s_peer_cache[s_peer_cache_count];
    memcpy(info->mac, mac_addr, 6);
    info->channel = channel;
    info->peer_type = peer_type;
    if (name && name[0]) {
        strncpy(info->name, name, ESP_NOW_PEER_NAME_MAX - 1);
    } else {
        snprintf(info->name, sizeof(info->name), "Peer-%d", s_peer_cache_count + 1);
    }
    s_peer_cache_count++;

    save_peers_to_nvs();

    ESP_LOGI(TAG, "Peer added successfully: " MACSTR " ch=%d name=%s",
             MAC2STR(mac_addr), channel, info->name);

    return true;
}

bool wifi_now_remove_peer(const uint8_t* mac_addr)
{
    if (s_state != WIFI_NOW_STATE_INIT) {
        ESP_LOGE(TAG, "ESP-NOW not initialized");
        return false;
    }

    if (!mac_addr) return false;

    esp_now_del_peer(mac_addr);

    int idx = find_peer_in_cache(mac_addr);
    if (idx >= 0) {
        if (idx < s_peer_cache_count - 1) {
            memmove(&s_peer_cache[idx], &s_peer_cache[idx + 1],
                    (s_peer_cache_count - idx - 1) * sizeof(wifi_now_peer_info_t));
        }
        s_peer_cache_count--;
        save_peers_to_nvs();
    }

    ESP_LOGI(TAG, "Peer removed: " MACSTR, MAC2STR(mac_addr));
    return true;
}

int wifi_now_get_peer_count(void)
{
    if (s_state != WIFI_NOW_STATE_INIT) return 0;
    return s_peer_cache_count;
}

bool wifi_now_is_peer_exists(const uint8_t* mac_addr)
{
    if (s_state != WIFI_NOW_STATE_INIT || !mac_addr) return false;
    return esp_now_is_peer_exist(mac_addr);
}

void wifi_now_clear_peers(void)
{
    if (s_state != WIFI_NOW_STATE_INIT) return;

    esp_now_peer_num_t peer_num;
    if (esp_now_get_peer_num(&peer_num) != ESP_OK) return;

    esp_now_peer_info_t peers[ESP_NOW_MAX_PEERS];
    int count = (peer_num.total_num < ESP_NOW_MAX_PEERS)
                ? peer_num.total_num : ESP_NOW_MAX_PEERS;

    if (esp_now_fetch_peer(true, peers) != ESP_OK) return;

    for (int i = 0; i < count; i++) {
        esp_now_del_peer(peers[i].peer_addr);
    }

    s_peer_cache_count = 0;
    memset(s_peer_cache, 0, sizeof(s_peer_cache));
    save_peers_to_nvs();

    ESP_LOGI(TAG, "All %d peers removed", count);
}

int wifi_now_get_peer_list(wifi_now_peer_info_t* peers, int max_count)
{
    int count = 0;
    for (int i = 0; i < s_peer_cache_count && count < max_count; i++) {
        peers[count] = s_peer_cache[i];
        count++;
    }
    return count;
}

void wifi_now_get_peers_json(char* buffer, size_t buffer_size)
{
    int pos = snprintf(buffer, buffer_size, "[");
    for (int i = 0; i < s_peer_cache_count; i++) {
        char mac_str[18];
        const char* type_str = (s_peer_cache[i].peer_type == PEER_TYPE_WIFI) ? "WiFi" : "ESP-NOW";
        snprintf(mac_str, sizeof(mac_str), MACSTR, MAC2STR(s_peer_cache[i].mac));
        pos += snprintf(buffer + pos, buffer_size - pos,
                        "%s{\"mac\":\"%s\",\"channel\":%d,\"name\":\"%s\",\"type\":\"%s\"}",
                        i > 0 ? "," : "",
                        mac_str,
                        s_peer_cache[i].channel,
                        s_peer_cache[i].name,
                        type_str);
    }
    snprintf(buffer + pos, buffer_size - pos, "]");
}

void wifi_now_save_peers(void)
{
    save_peers_to_nvs();
}

void wifi_now_load_peers(void)
{
    s_peer_cache_count = 0;
    for (int i = 0; i < ESP_NOW_MAX_PEERS; i++) {
        esp_now_del_peer(s_peer_cache[i].mac);
    }
    load_peers_from_nvs();
    for (int i = 0; i < s_peer_cache_count; i++) {
        wifi_now_add_peer(s_peer_cache[i].mac, (uint8_t)s_peer_cache[i].channel);
    }
}

int wifi_now_send(const uint8_t* mac_addr, const uint8_t* data, int len)
{
    if (s_state != WIFI_NOW_STATE_INIT) {
        ESP_LOGE(TAG, "ESP-NOW not initialized");
        return -1;
    }

    if (!mac_addr || !data || len <= 0 || len > ESP_NOW_MAX_DATA_LEN) {
        ESP_LOGE(TAG, "Invalid send params");
        return -1;
    }

    s_last_send_len = len;
    
    // 调试输出: 检查peer是否存在
    bool peer_exists = esp_now_is_peer_exist(mac_addr);
    ESP_LOGD(TAG, "Send to " MACSTR " len=%d, peer_exists=%d",
             MAC2STR(mac_addr), len, peer_exists);
    
    esp_err_t ret = esp_now_send(mac_addr, data, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Send failed: %d (%s)", ret, esp_err_to_name(ret));
        // 失败也记录
        wifi_now_add_send_entry(mac_addr, len, false,
                                (memcmp(mac_addr, s_bcast_mac, 6) == 0));
        return -1;
    }

    return 0;
}

int wifi_now_broadcast(const uint8_t* data, int len)
{
    return wifi_now_send(s_bcast_mac, data, len);
}

uint8_t wifi_now_get_channel(void)
{
    if (s_state == WIFI_NOW_STATE_INIT) {
        wifi_second_chan_t second_ch;
        esp_wifi_get_channel(&s_channel, &second_ch);
    }
    return s_channel;
}

bool wifi_now_set_channel(uint8_t channel)
{
    if (channel < 1 || channel > 14) {
        ESP_LOGE(TAG, "Invalid channel: %d", channel);
        return false;
    }

    esp_err_t ret = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set channel %d failed: %d (%s)",
                 channel, ret, esp_err_to_name(ret));
        return false;
    }

    s_channel = channel;
    ESP_LOGI(TAG, "Channel set to %d", channel);
    return true;
}

void wifi_now_update_peers_channel(uint8_t channel)
{
    if (s_state != WIFI_NOW_STATE_INIT) return;

    s_channel = channel;

    // 更新广播 peer 的信道
    if (esp_now_is_peer_exist(s_bcast_mac)) {
        esp_now_peer_info_t bcast = {};
        memcpy(bcast.peer_addr, s_bcast_mac, 6);
        bcast.channel = channel;
        bcast.ifidx   = WIFI_IF_STA;
        bcast.encrypt = false;
        esp_now_mod_peer(&bcast);
    }

    // 更新所有缓存的 peer 信道
    for (int i = 0; i < s_peer_cache_count; i++) {
        if (s_peer_cache[i].channel != (int)channel) {
            s_peer_cache[i].channel = channel;
            esp_now_peer_info_t peer = {};
            memcpy(peer.peer_addr, s_peer_cache[i].mac, 6);
            peer.channel = channel;
            peer.ifidx   = WIFI_IF_STA;
            peer.encrypt = false;
            if (esp_now_mod_peer(&peer) == ESP_OK) {
                ESP_LOGI(TAG, "Updated peer " MACSTR " to channel %d",
                         MAC2STR(s_peer_cache[i].mac), channel);
            }
        }
    }
    save_peers_to_nvs();

    ESP_LOGI(TAG, "All peers updated to channel %d", channel);
}

void wifi_now_get_mac(uint8_t* mac_out)
{
    if (!mac_out) return;
    esp_read_mac(mac_out, ESP_MAC_WIFI_STA);
}

void wifi_now_set_pair_callback(wifi_now_pair_cb_t cb)
{
    s_pair_cb = cb;
}

bool wifi_now_send_pair_request(const uint8_t* dest_mac)
{
    if (!dest_mac || !wifi_now_is_initialized()) {
        return false;
    }

    esp_now_pair_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic = ESP_NOW_PAIR_MAGIC;
    msg.type = ESP_NOW_MSG_PAIR_REQUEST;
    wifi_now_get_mac(msg.mac);
    msg.channel = s_channel;
    msg.peer_type = PEER_TYPE_ESPNOW;

    ble_pairing_get_name(msg.name);

    // 将本机 PMK 附带在配对请求中发送给对端
    memcpy(msg.pmk, s_pmk, 16);

    int len = sizeof(esp_now_pair_msg_t);
    int sent = wifi_now_send(dest_mac, (const uint8_t*)&msg, len);
    if (sent == 0) {
        ESP_LOGI(TAG, "Pair request sent to " MACSTR, MAC2STR(dest_mac));
        return true;
    }
    return false;
}

void wifi_now_set_unpair_callback(wifi_now_unpair_cb_t cb)
{
    s_unpair_cb = cb;
}

bool wifi_now_send_unpair_request(const uint8_t* dest_mac)
{
    if (!dest_mac || !wifi_now_is_initialized()) {
        return false;
    }

    esp_now_pair_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic = ESP_NOW_PAIR_MAGIC;
    msg.type = ESP_NOW_MSG_UNPAIR_REQUEST;
    wifi_now_get_mac(msg.mac);

    ble_pairing_get_name(msg.name);

    int len = sizeof(esp_now_pair_msg_t);
    int sent = wifi_now_send(dest_mac, (const uint8_t*)&msg, len);
    if (sent == 0) {
        ESP_LOGI(TAG, "Unpair request sent to " MACSTR, MAC2STR(dest_mac));
        return true;
    }
    return false;
}

bool wifi_now_send_unpair_response(const uint8_t* dest_mac)
{
    if (!dest_mac || !wifi_now_is_initialized()) {
        return false;
    }

    esp_now_pair_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic = ESP_NOW_PAIR_MAGIC;
    msg.type = ESP_NOW_MSG_UNPAIR_RESPONSE;
    wifi_now_get_mac(msg.mac);

    ble_pairing_get_name(msg.name);

    int len = sizeof(esp_now_pair_msg_t);
    int sent = wifi_now_send(dest_mac, (const uint8_t*)&msg, len);
    if (sent == 0) {
        ESP_LOGI(TAG, "Unpair response sent to " MACSTR, MAC2STR(dest_mac));
        return true;
    }
    return false;
}

bool wifi_now_handle_unpair_message(const uint8_t* mac_addr, const uint8_t* data, int len)
{
    if (!mac_addr || !data || len < sizeof(esp_now_pair_msg_t)) {
        return false;
    }

    esp_now_pair_msg_t* msg = (esp_now_pair_msg_t*)data;

    if (msg->magic != ESP_NOW_PAIR_MAGIC) {
        return false;
    }

    if (msg->type == ESP_NOW_MSG_UNPAIR_REQUEST) {
        ESP_LOGI(TAG, "Unpair request from " MACSTR, MAC2STR(msg->mac));

        if (wifi_now_is_peer_exists(msg->mac)) {
            wifi_now_remove_peer(msg->mac);
            ESP_LOGI(TAG, "Auto-removed peer " MACSTR " from unpair request",
                     MAC2STR(msg->mac));
        }

        wifi_now_send_unpair_response(msg->mac);

        if (s_unpair_cb) {
            s_unpair_cb(msg->mac);
        }

        return true;
    }

    if (msg->type == ESP_NOW_MSG_UNPAIR_RESPONSE) {
        ESP_LOGI(TAG, "Unpair response from " MACSTR, MAC2STR(msg->mac));

        if (wifi_now_is_peer_exists(msg->mac)) {
            wifi_now_remove_peer(msg->mac);
            ESP_LOGI(TAG, "Auto-removed peer " MACSTR " from unpair response",
                     MAC2STR(msg->mac));
        }

        if (s_unpair_cb) {
            s_unpair_cb(msg->mac);
        }

        return true;
    }

    return false;
}

bool wifi_now_unpair_with_peer(const uint8_t* mac_addr)
{
    if (!mac_addr || !wifi_now_is_initialized()) {
        ESP_LOGE(TAG, "Invalid parameters for unpair");
        return false;
    }

    if (!wifi_now_is_peer_exists(mac_addr)) {
        ESP_LOGW(TAG, "Peer " MACSTR " does not exist", MAC2STR(mac_addr));
        return false;
    }

    ESP_LOGI(TAG, "Unpairing with peer " MACSTR, MAC2STR(mac_addr));

    wifi_now_send_unpair_request(mac_addr);

    vTaskDelay(pdMS_TO_TICKS(100));

    if (wifi_now_is_peer_exists(mac_addr)) {
        wifi_now_remove_peer(mac_addr);
    }

    if (s_unpair_cb) {
        s_unpair_cb(mac_addr);
    }

    return true;
}

bool wifi_now_send_pair_response(const uint8_t* dest_mac)
{
    if (!dest_mac || !wifi_now_is_initialized()) {
        return false;
    }

    esp_now_pair_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic = ESP_NOW_PAIR_MAGIC;
    msg.type = ESP_NOW_MSG_PAIR_RESPONSE;
    wifi_now_get_mac(msg.mac);
    msg.channel = s_channel;
    msg.peer_type = PEER_TYPE_ESPNOW;

    ble_pairing_get_name(msg.name);

    int len = sizeof(esp_now_pair_msg_t);
    int sent = wifi_now_send(dest_mac, (const uint8_t*)&msg, len);
    if (sent == 0) {
        ESP_LOGI(TAG, "Pair response sent to " MACSTR, MAC2STR(dest_mac));
        return true;
    }
    return false;
}

bool wifi_now_handle_pair_message(const uint8_t* mac_addr, const uint8_t* data, int len)
{
    if (!mac_addr || !data || len < sizeof(esp_now_pair_msg_t)) {
        return false;
    }

    esp_now_pair_msg_t* msg = (esp_now_pair_msg_t*)data;

    if (msg->magic != ESP_NOW_PAIR_MAGIC) {
        return false;
    }

    if (msg->type == ESP_NOW_MSG_PAIR_REQUEST) {
        ESP_LOGI(TAG, "Pair request from " MACSTR " (name: %s)",
                  MAC2STR(msg->mac), msg->name);

        // 总是更新peer名称（包括已存在的peer），wifi_now_add_peer_with_name内部处理新建/更新
        wifi_now_add_peer_with_name(msg->mac, msg->channel, msg->name, msg->peer_type);
        wifi_now_save_peers();
        ESP_LOGI(TAG, "Pair request: added/updated peer " MACSTR " name=%s",
                 MAC2STR(msg->mac), msg->name);

        // 从配对请求中提取 PMK 并更新
        uint8_t zero_pmk[16] = {0};
        if (memcmp(msg->pmk, zero_pmk, 16) != 0) {
            wifi_now_set_pmk_from_peer(msg->pmk);
        }

        wifi_now_send_pair_response(msg->mac);

        if (s_pair_cb) {
            s_pair_cb(msg->mac, msg->name);
        }

        return true;
    }

    if (msg->type == ESP_NOW_MSG_PAIR_RESPONSE) {
        ESP_LOGI(TAG, "Pair response from " MACSTR " (name: %s)",
                  MAC2STR(msg->mac), msg->name);

        // 总是更新peer名称（包括已存在的peer）
        wifi_now_add_peer_with_name(msg->mac, msg->channel, msg->name, msg->peer_type);
        wifi_now_save_peers();
        ESP_LOGI(TAG, "Pair response: added/updated peer " MACSTR " name=%s",
                 MAC2STR(msg->mac), msg->name);

        // 从配对响应中提取 PMK 并更新
        uint8_t zero_pmk[16] = {0};
        if (memcmp(msg->pmk, zero_pmk, 16) != 0) {
            wifi_now_set_pmk_from_peer(msg->pmk);
        }

        if (s_pair_cb) {
            s_pair_cb(msg->mac, msg->name);
        }

        return true;
    }

    return false;
}bool wifi_now_add_msg_template(const char* name, const uint8_t* data, int data_len)
{
    if (!name || !data || data_len <= 0 || data_len > ESP_NOW_MSG_DATA_MAX) {
        ESP_LOGE(TAG, "Invalid message template parameters");
        return false;
    }

    if (s_msg_cache_count >= ESP_NOW_MAX_MSG_TEMPLATES) {
        ESP_LOGE(TAG, "Too many message templates");
        return false;
    }

    strncpy(s_msg_cache[s_msg_cache_count].name, name, ESP_NOW_MSG_NAME_MAX - 1);
    s_msg_cache[s_msg_cache_count].name[ESP_NOW_MSG_NAME_MAX - 1] = '\0';
    memcpy(s_msg_cache[s_msg_cache_count].data, data, data_len);
    s_msg_cache[s_msg_cache_count].data_len = data_len;
    s_msg_cache_count++;

    ESP_LOGI(TAG, "Added message template '%s' (%d bytes)", name, data_len);
    return true;
}

bool wifi_now_update_msg_template(int index, const char* name, const uint8_t* data, int data_len)
{
    if (index < 0 || index >= s_msg_cache_count) {
        ESP_LOGE(TAG, "Invalid message template index %d", index);
        return false;
    }

    if (!name || !data || data_len <= 0 || data_len > ESP_NOW_MSG_DATA_MAX) {
        ESP_LOGE(TAG, "Invalid message template parameters");
        return false;
    }

    strncpy(s_msg_cache[index].name, name, ESP_NOW_MSG_NAME_MAX - 1);
    s_msg_cache[index].name[ESP_NOW_MSG_NAME_MAX - 1] = '\0';
    memcpy(s_msg_cache[index].data, data, data_len);
    s_msg_cache[index].data_len = data_len;

    ESP_LOGI(TAG, "Updated message template '%s' (%d bytes)", name, data_len);
    return true;
}

bool wifi_now_remove_msg_template(int index)
{
    if (index < 0 || index >= s_msg_cache_count) {
        ESP_LOGE(TAG, "Invalid message template index %d", index);
        return false;
    }

    ESP_LOGI(TAG, "Removed message template '%s'", s_msg_cache[index].name);

    for (int i = index; i < s_msg_cache_count - 1; i++) {
        memcpy(&s_msg_cache[i], &s_msg_cache[i + 1], sizeof(wifi_now_msg_template_t));
    }
    s_msg_cache_count--;

    return true;
}

int wifi_now_get_msg_template_count(void)
{
    return s_msg_cache_count;
}

bool wifi_now_get_msg_template(int index, wifi_now_msg_template_t* template_out)
{
    if (index < 0 || index >= s_msg_cache_count || !template_out) {
        return false;
    }

    memcpy(template_out, &s_msg_cache[index], sizeof(wifi_now_msg_template_t));
    return true;
}

void wifi_now_get_msg_templates_json(char* buffer, size_t buffer_size)
{
    if (!buffer) return;

    int pos = snprintf(buffer, buffer_size, "[");
    for (int i = 0; i < s_msg_cache_count; i++) {
        char data_hex[ESP_NOW_MSG_DATA_MAX * 2 + 1] = {0};
        for (int j = 0; j < s_msg_cache[i].data_len; j++) {
            snprintf(&data_hex[j * 2], sizeof(data_hex) - j * 2, "%02x", s_msg_cache[i].data[j]);
        }

        pos += snprintf(buffer + pos, buffer_size - pos,
                        "%s{\"index\":%d,\"name\":\"%s\",\"data\":\"%s\",\"data_len\":%d}",
                        i > 0 ? "," : "",
                        i,
                        s_msg_cache[i].name,
                        data_hex,
                        s_msg_cache[i].data_len);
    }
    snprintf(buffer + pos, buffer_size - pos, "]");
}

void wifi_now_save_msg_templates(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return;
    }

    nvs_set_u8(handle, NVS_KEY_MSG_COUNT, (uint8_t)s_msg_cache_count);

    for (int i = 0; i < s_msg_cache_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%s%d", NVS_KEY_MSG, i);
        nvs_set_blob(handle, key, &s_msg_cache[i], sizeof(wifi_now_msg_template_t));
    }

    nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Saved %d message templates", s_msg_cache_count);
}

void wifi_now_load_msg_templates(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return;
    }

    uint8_t count = 0;
    size_t required_size = 0;
    nvs_get_u8(handle, NVS_KEY_MSG_COUNT, &count);

    if (count > ESP_NOW_MAX_MSG_TEMPLATES) count = ESP_NOW_MAX_MSG_TEMPLATES;
    s_msg_cache_count = 0;

    for (int i = 0; i < count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%s%d", NVS_KEY_MSG, i);

        required_size = sizeof(wifi_now_msg_template_t);
        ret = nvs_get_blob(handle, key, &s_msg_cache[s_msg_cache_count], &required_size);
        if (ret == ESP_OK) {
            s_msg_cache_count++;
        }
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Loaded %d message templates", s_msg_cache_count);
}

// ========== 接收消息环形缓冲 ==========

bool wifi_now_add_recv_entry(const uint8_t* mac, const uint8_t* data, int len)
{
    if (!mac || !data || len <= 0) return false;

    wifi_now_recv_entry_t* entry = &s_recv_ring[s_recv_ring_head];
    memcpy(entry->mac, mac, 6);
    int copy_len = (len > ESP_NOW_MSG_DATA_MAX) ? ESP_NOW_MSG_DATA_MAX : len;
    memcpy(entry->data, data, copy_len);
    entry->data_len = copy_len;

    s_recv_ring_head = (s_recv_ring_head + 1) % ESP_NOW_RECV_HISTORY_MAX;
    if (s_recv_ring_count < ESP_NOW_RECV_HISTORY_MAX) {
        s_recv_ring_count++;
    }

    return true;
}

int wifi_now_get_recv_count(void)
{
    return s_recv_ring_count;
}

// 判断数据是否为可读文本（ASCII + UTF-8 中文等）
static bool is_text_content(const uint8_t* data, int len)
{
    for (int i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (b >= 0x20 && b <= 0x7E) continue;           // 可见 ASCII
        if (b == '\t' || b == '\n' || b == '\r') continue; // 常见控制符
        if (b >= 0xC0 && b <= 0xFD) {
            // UTF-8 多字节序列起始
            int follow;
            if (b >= 0xFC) follow = 5;
            else if (b >= 0xF8) follow = 4;
            else if (b >= 0xF0) follow = 3;
            else if (b >= 0xE0) follow = 2;
            else follow = 1;
            for (int j = 0; j < follow; j++) {
                i++;
                if (i >= len || (data[i] & 0xC0) != 0x80) return false;
            }
            continue;
        }
        return false;  // 非文本字节
    }
    return true;
}

void wifi_now_get_recv_messages_json(char* buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) return;

    int pos = snprintf(buffer, buffer_size, "[");
    int start = s_recv_ring_count < ESP_NOW_RECV_HISTORY_MAX ? 0
              : s_recv_ring_head;
    int total = s_recv_ring_count;

    for (int i = 0; i < total && pos < (int)buffer_size - 50; i++) {
        int idx = (start + i) % ESP_NOW_RECV_HISTORY_MAX;
        wifi_now_recv_entry_t* entry = &s_recv_ring[idx];

        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), MACSTR, MAC2STR(entry->mac));

        char data_buf[ESP_NOW_RECV_ENTRY_JSON_MAX] = {0};
        int dp = 0;

        if (is_text_content(entry->data, entry->data_len)) {
            // 文本内容（含中文等 UTF-8），JSON 转义后原样输出
            for (int j = 0; j < entry->data_len && dp < (int)sizeof(data_buf) - 10; j++) {
                uint8_t b = entry->data[j];
                if (b == '"' || b == '\\') { data_buf[dp++] = '\\'; data_buf[dp++] = (char)b; }
                else if (b == '\n') { data_buf[dp++] = '\\'; data_buf[dp++] = 'n'; }
                else if (b == '\r') { data_buf[dp++] = '\\'; data_buf[dp++] = 'r'; }
                else if (b == '\t') { data_buf[dp++] = '\\'; data_buf[dp++] = 't'; }
                else if (b < 0x20) {
                    dp += snprintf(&data_buf[dp], sizeof(data_buf) - dp, "\\u%04x", b);
                } else {
                    data_buf[dp++] = (char)b;  // UTF-8 多字节原样通过
                }
            }
        } else {
            // 二进制内容，HEX 格式输出
            for (int j = 0; j < entry->data_len && dp < (int)sizeof(data_buf) - 4; j++) {
                if (j > 0) data_buf[dp++] = ' ';
                dp += snprintf(&data_buf[dp], sizeof(data_buf) - dp, "%02X", entry->data[j]);
            }
        }
        data_buf[dp] = '\0';

        pos += snprintf(buffer + pos, buffer_size - pos,
                        "%s{\"mac\":\"%s\",\"len\":%d,\"data\":\"%s\"}",
                        i > 0 ? "," : "", mac_str, entry->data_len, data_buf);
    }
    snprintf(buffer + pos, buffer_size - pos, "]");
}

// ========== 发送历史环形缓冲 ==========

bool wifi_now_add_send_entry(const uint8_t* mac, int len, bool success, bool is_broadcast)
{
    if (!mac || len <= 0) return false;

    wifi_now_send_entry_t* entry = &s_send_history[s_send_history_head];
    memcpy(entry->mac, mac, 6);
    entry->data_len = len;
    entry->success = success;
    entry->is_broadcast = is_broadcast;
    entry->acked = false;

    s_send_history_head = (s_send_history_head + 1) % ESP_NOW_SEND_HISTORY_MAX;
    if (s_send_history_count < ESP_NOW_SEND_HISTORY_MAX) {
        s_send_history_count++;
    }

    return true;
}

int wifi_now_get_send_history_count(void)
{
    return s_send_history_count;
}

void wifi_now_get_send_history_json(char* buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) return;

    int pos = snprintf(buffer, buffer_size, "[");
    int start = s_send_history_count < ESP_NOW_SEND_HISTORY_MAX ? 0
              : s_send_history_head;
    int total = s_send_history_count;

    for (int i = 0; i < total && pos < (int)buffer_size - 50; i++) {
        int idx = (start + i) % ESP_NOW_SEND_HISTORY_MAX;
        wifi_now_send_entry_t* entry = &s_send_history[idx];

        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), MACSTR, MAC2STR(entry->mac));

        pos += snprintf(buffer + pos, buffer_size - pos,
                        "%s{\"mac\":\"%s\",\"len\":%d,\"success\":%s,\"broadcast\":%s,\"acked\":%s}",
                        i > 0 ? "," : "",
                        mac_str,
                        entry->data_len,
                        entry->success ? "true" : "false",
                        entry->is_broadcast ? "true" : "false",
                        entry->acked ? "true" : "false");
    }
    snprintf(buffer + pos, buffer_size - pos, "]");
}
