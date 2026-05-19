#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "wifi_now.h"
#include "ble_pairing.h"

static const char* TAG = "WIFI_NOW";
static const char* NVS_NS        = "espnow_cfg";
static const char* NVS_KEY_COUNT = "peer_cnt";
static const char* NVS_KEY_PEER  = "peer_";

static const uint8_t s_bcast_mac[] = ESP_NOW_BCAST_MAC;

static wifi_now_state_t s_state         = WIFI_NOW_STATE_IDLE;
static SemaphoreHandle_t s_state_mutex  = NULL;
static uint8_t           s_channel      = 1;

static wifi_now_peer_info_t s_peer_cache[ESP_NOW_MAX_PEERS];
static int                  s_peer_cache_count = 0;

static wifi_now_recv_cb_t s_recv_cb = NULL;
static wifi_now_send_cb_t s_send_cb = NULL;
static wifi_now_pair_cb_t s_pair_cb = NULL;
static wifi_now_unpair_cb_t s_unpair_cb = NULL;

static void espnow_recv_cb(const esp_now_recv_info_t* info,
                            const uint8_t* data, int data_len)
{
    if (!info || !data || data_len <= 0) return;

    ESP_LOGI(TAG, "Recv %d bytes from " MACSTR,
             data_len, MAC2STR(info->src_addr));

    if (data_len >= sizeof(esp_now_pair_msg_t) + 4) {
        if (wifi_now_handle_pair_message(info->src_addr, data, data_len)) {
            return;
        }

        if (wifi_now_handle_unpair_message(info->src_addr, data, data_len)) {
            return;
        }
    }

    if (s_recv_cb) {
        s_recv_cb(info->src_addr, data, data_len);
    }
}

static void espnow_send_cb(const esp_now_send_info_t* tx_info,
                            esp_now_send_status_t status)
{
    if (!tx_info) return;
    const char* status_str = (status == ESP_NOW_SEND_SUCCESS) ? "OK" : "FAIL";
    ESP_LOGI(TAG, "Send to " MACSTR " %s", MAC2STR(tx_info->des_addr), status_str);

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

    esp_err_t ret = esp_now_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %d (%s)", ret, esp_err_to_name(ret));
        s_state = WIFI_NOW_STATE_ERROR;
        return;
    }

    esp_now_register_recv_cb(espnow_recv_cb);
    esp_now_register_send_cb(espnow_send_cb);

    esp_now_set_pmk((const uint8_t*)"pmk1234567890123");

    wifi_second_chan_t second_ch = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&s_channel, &second_ch);

    s_state = WIFI_NOW_STATE_INIT;

    load_peers_from_nvs();

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
    return wifi_now_add_peer_with_name(mac_addr, channel, "");
}

bool wifi_now_add_peer_with_name(const uint8_t* mac_addr, uint8_t channel,
                                  const char* name)
{
    if (s_state != WIFI_NOW_STATE_INIT) {
        ESP_LOGE(TAG, "ESP-NOW not initialized");
        return false;
    }

    if (!mac_addr) return false;

    if (esp_now_is_peer_exist(mac_addr)) {
        int idx = find_peer_in_cache(mac_addr);
        if (idx >= 0 && name && name[0]) {
            strncpy(s_peer_cache[idx].name, name, ESP_NOW_PEER_NAME_MAX - 1);
            save_peers_to_nvs();
        }
        ESP_LOGW(TAG, "Peer " MACSTR " already exists", MAC2STR(mac_addr));
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

    esp_now_peer_num_t peer_num;
    esp_err_t ret = esp_now_get_peer_num(&peer_num);
    if (ret != ESP_OK) return 0;

    return peer_num.total_num;
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
        snprintf(mac_str, sizeof(mac_str), MACSTR, MAC2STR(s_peer_cache[i].mac));
        pos += snprintf(buffer + pos, buffer_size - pos,
                        "%s{\"mac\":\"%s\",\"channel\":%d,\"name\":\"%s\",\"type\":\"slave\"}",
                        i > 0 ? "," : "",
                        mac_str,
                        s_peer_cache[i].channel,
                        s_peer_cache[i].name);
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

    esp_err_t ret = esp_now_send(mac_addr, data, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Send failed: %d (%s)", ret, esp_err_to_name(ret));
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

    ble_pairing_get_name(msg.name);

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

        if (!wifi_now_is_peer_exists(msg->mac)) {
            wifi_now_add_peer_with_name(msg->mac, s_channel, msg->name);
            wifi_now_save_peers();
            ESP_LOGI(TAG, "Auto-added peer " MACSTR " from pair request",
                     MAC2STR(msg->mac));
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

        if (!wifi_now_is_peer_exists(msg->mac)) {
            wifi_now_add_peer_with_name(msg->mac, s_channel, msg->name);
            wifi_now_save_peers();
            ESP_LOGI(TAG, "Auto-added peer " MACSTR " from pair response",
                     MAC2STR(msg->mac));
        }

        if (s_pair_cb) {
            s_pair_cb(msg->mac, msg->name);
        }

        return true;
    }

    return false;
}