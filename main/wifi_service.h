#ifndef WIFI_SERVICE_H
#define WIFI_SERVICE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DHCP_CLIENT_MAX 16

#define WIFI_SCAN_MAX_RESULTS 32

typedef enum {
    WIFI_OP_MODE_STA   = 0,
    WIFI_OP_MODE_AP    = 1,
    WIFI_OP_MODE_APSTA = 2
} wifi_op_mode_t;

typedef enum {
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED
} wifi_state_t;

typedef enum {
    DHCP_CLIENT_SRC_AP  = 0,
    DHCP_CLIENT_SRC_USB = 1
} dhcp_client_source_t;

typedef struct {
    dhcp_client_source_t source;
    uint8_t mac[6];
    char    ip[16];
} dhcp_client_info_t;

typedef struct {
    char ssid[33];
    int  rssi;
    int  channel;
    int  authmode;
} wifi_scan_item_t;

typedef struct {
    wifi_op_mode_t mode;
    char sta_ssid[33];
    char sta_password[65];
    wifi_state_t sta_state;
    char sta_ip[16];
    int  sta_rssi;
    char ap_ssid[33];
    char ap_password[65];
    bool ap_active;
    int  ap_clients;
    uint32_t sta_down_bps;
    uint32_t sta_up_bps;
    uint32_t ap_down_bps;
    uint32_t ap_up_bps;
    uint32_t usb_down_bps;
    uint32_t usb_up_bps;
} WiFiStatus;

typedef void (*wifi_ap_client_cb_t)(const uint8_t* client_mac, const char* client_ip, void* user_data);

void wifi_service_set_ap_client_callback(wifi_ap_client_cb_t cb, void* user_data);

void wifi_service_init(void);
void wifi_service_connect(const char* ssid, const char* password);
void wifi_service_disconnect(void);
void wifi_service_save_config(const char* ssid, const char* password);
bool wifi_service_has_config(void);
void wifi_service_get_status(WiFiStatus* status);
void wifi_service_get_status_json(char* buffer, size_t buffer_size);
int  wifi_service_get_dhcp_clients(dhcp_client_info_t* clients, int max_count);
void wifi_service_get_dhcp_clients_json(char* buffer, size_t buffer_size);

int  wifi_service_scan(wifi_scan_item_t* items, int max_count);
void wifi_service_scan_start(void);
void wifi_service_get_scan_json(char* buffer, size_t buffer_size);

void wifi_service_set_mode(wifi_op_mode_t mode);
wifi_op_mode_t wifi_service_get_mode(void);
void wifi_service_start_ap(const char* ssid, const char* password);
void wifi_service_stop_ap(void);
bool wifi_service_is_ap_active(void);
void wifi_service_get_ap_config(char* ssid, size_t ssid_len, char* password, size_t pass_len);

void wifi_service_post_connect(const char* ssid, const char* password);
void wifi_service_post_set_mode(wifi_op_mode_t mode);
void wifi_service_post_start_ap(const char* ssid, const char* password);
void wifi_service_post_stop_ap(void);

#ifdef __cplusplus
}
#endif

#endif