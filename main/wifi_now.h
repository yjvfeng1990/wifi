#ifndef WIFI_NOW_H
#define WIFI_NOW_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_NOW_MAX_PEERS         20
#define ESP_NOW_BCAST_MAC         {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}
#define ESP_NOW_PEER_NAME_MAX     32
#define ESP_NOW_MSG_NAME_MAX      32
#define ESP_NOW_MAX_MSG_TEMPLATES 10
#define ESP_NOW_MSG_DATA_MAX      250

#define ESP_NOW_MSG_PAIR_REQUEST   0x01
#define ESP_NOW_MSG_PAIR_RESPONSE  0x02
#define ESP_NOW_MSG_UNPAIR_REQUEST  0x03
#define ESP_NOW_MSG_UNPAIR_RESPONSE 0x04
#define ESP_NOW_MSG_ACK            0x05
#define ESP_NOW_MSG_DATA           0x10

#define ESP_NOW_RECV_HISTORY_MAX   20
#define ESP_NOW_SEND_HISTORY_MAX   20
#define ESP_NOW_RECV_ENTRY_JSON_MAX 1024

#define ESP_NOW_PAIR_MAGIC         0x4553504E  // "ESPN"

#define PEER_TYPE_WIFI            0
#define PEER_TYPE_ESPNOW          1

typedef struct {
    uint32_t magic;
    uint8_t type;
    uint8_t mac[6];
    uint8_t channel;
    uint8_t peer_type;
    char name[ESP_NOW_PEER_NAME_MAX];
    uint8_t pmk[16];       // 自动生成的 PMK，全零表示无效
} __attribute__((packed)) esp_now_pair_msg_t;

typedef enum {
    WIFI_NOW_STATE_IDLE = 0,
    WIFI_NOW_STATE_INIT,
    WIFI_NOW_STATE_ERROR
} wifi_now_state_t;

typedef struct {
    uint8_t mac[6];
    int     channel;
    char    name[ESP_NOW_PEER_NAME_MAX];
    uint8_t peer_type;
} wifi_now_peer_info_t;

typedef struct {
    char    name[ESP_NOW_MSG_NAME_MAX];
    uint8_t data[ESP_NOW_MSG_DATA_MAX];
    int     data_len;
} wifi_now_msg_template_t;

typedef struct {
    uint8_t mac[6];
    uint8_t data[ESP_NOW_MSG_DATA_MAX];
    int     data_len;
} wifi_now_recv_entry_t;

typedef struct {
    uint8_t mac[6];
    int     data_len;
    bool    success;
    bool    is_broadcast;
    bool    acked;
} wifi_now_send_entry_t;

typedef void (*wifi_now_recv_cb_t)(const uint8_t* mac_addr,
                                    const uint8_t* data, int data_len);
typedef void (*wifi_now_send_cb_t)(const uint8_t* mac_addr, bool success);
typedef void (*wifi_now_pair_cb_t)(const uint8_t* mac_addr, const char* name);
typedef void (*wifi_now_unpair_cb_t)(const uint8_t* mac_addr);

void wifi_now_init(void);
void wifi_now_deinit(void);
bool wifi_now_is_initialized(void);
wifi_now_state_t wifi_now_get_state(void);

void wifi_now_set_recv_callback(wifi_now_recv_cb_t cb);
void wifi_now_set_send_callback(wifi_now_send_cb_t cb);
void wifi_now_set_pair_callback(wifi_now_pair_cb_t cb);

bool wifi_now_add_peer(const uint8_t* mac_addr, uint8_t channel);
bool wifi_now_add_peer_with_name(const uint8_t* mac_addr, uint8_t channel,
                                  const char* name, uint8_t peer_type);
bool wifi_now_remove_peer(const uint8_t* mac_addr);
int  wifi_now_get_peer_count(void);
bool wifi_now_is_peer_exists(const uint8_t* mac_addr);
void wifi_now_clear_peers(void);

int  wifi_now_get_peer_list(wifi_now_peer_info_t* peers, int max_count);
void wifi_now_get_peers_json(char* buffer, size_t buffer_size);

void wifi_now_save_peers(void);
void wifi_now_load_peers(void);

int  wifi_now_send(const uint8_t* mac_addr,
                    const uint8_t* data, int len);
int  wifi_now_broadcast(const uint8_t* data, int len);

uint8_t wifi_now_get_channel(void);
bool   wifi_now_set_channel(uint8_t channel);
void   wifi_now_update_peers_channel(uint8_t channel);

void wifi_now_get_mac(uint8_t* mac_out);
const uint8_t* wifi_now_get_pmk(void);

bool wifi_now_add_msg_template(const char* name, const uint8_t* data, int data_len);
bool wifi_now_update_msg_template(int index, const char* name, const uint8_t* data, int data_len);
bool wifi_now_remove_msg_template(int index);
int  wifi_now_get_msg_template_count(void);
bool wifi_now_get_msg_template(int index, wifi_now_msg_template_t* template_out);
void wifi_now_get_msg_templates_json(char* buffer, size_t buffer_size);
void wifi_now_save_msg_templates(void);
void wifi_now_load_msg_templates(void);

bool wifi_now_send_pair_request(const uint8_t* dest_mac);
bool wifi_now_send_pair_response(const uint8_t* dest_mac);
bool wifi_now_handle_pair_message(const uint8_t* mac_addr, const uint8_t* data, int len);

bool wifi_now_send_unpair_request(const uint8_t* dest_mac);
bool wifi_now_send_unpair_response(const uint8_t* dest_mac);
bool wifi_now_handle_unpair_message(const uint8_t* mac_addr, const uint8_t* data, int len);
bool wifi_now_unpair_with_peer(const uint8_t* mac_addr);
void wifi_now_set_unpair_callback(wifi_now_unpair_cb_t cb);

// 接收消息历史
bool wifi_now_add_recv_entry(const uint8_t* mac, const uint8_t* data, int len);
int  wifi_now_get_recv_count(void);
void wifi_now_get_recv_messages_json(char* buffer, size_t buffer_size);

// 发送消息历史
bool wifi_now_add_send_entry(const uint8_t* mac, int len, bool success, bool is_broadcast);
int  wifi_now_get_send_history_count(void);
void wifi_now_get_send_history_json(char* buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif