#ifndef WIFI_SERVICE_H
#define WIFI_SERVICE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_OP_MODE_STA = 0,
    WIFI_OP_MODE_AP  = 1
} wifi_op_mode_t;

typedef enum {
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED
} wifi_state_t;

typedef struct {
    wifi_op_mode_t mode;
    char sta_ssid[33];
    char sta_password[65];
    char ap_ssid[33];
    char ap_password[65];
    wifi_state_t state;
    char ip[16];
    int rssi;
    int ap_clients;
} WiFiStatus;

void wifi_service_init(void);
void wifi_service_connect(const char* ssid, const char* password);
void wifi_service_disconnect(void);
void wifi_service_save_config(const char* ssid, const char* password);
bool wifi_service_has_config(void);
void wifi_service_get_status(WiFiStatus* status);
void wifi_service_get_status_json(char* buffer, size_t bufferSize);

void wifi_service_set_mode(wifi_op_mode_t mode);
wifi_op_mode_t wifi_service_get_mode(void);
void wifi_service_start_ap(const char* ssid, const char* password);
void wifi_service_stop_ap(void);
void wifi_service_get_ap_config(char* ssid, size_t ssid_len, char* password, size_t pass_len);

void wifi_service_post_connect(const char* ssid, const char* password);
void wifi_service_post_set_mode(wifi_op_mode_t mode);
void wifi_service_post_start_ap(const char* ssid, const char* password);
void wifi_service_post_stop_ap(void);

#ifdef __cplusplus
}
#endif

#endif