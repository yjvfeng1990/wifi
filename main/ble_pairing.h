#ifndef BLE_PAIRING_H
#define BLE_PAIRING_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_MAX_DISCOVERED   16
#define BLE_DEV_NAME_MAX     32

typedef enum {
    BLE_PAIR_STATE_OFF       = 0,
    BLE_PAIR_STATE_ADVERTISING,
    BLE_PAIR_STATE_SCANNING,
    BLE_PAIR_STATE_ERROR
} ble_pair_state_t;

typedef struct {
    uint8_t mac[6];
    uint8_t now_mac[6];
    char    name[BLE_DEV_NAME_MAX];
    int     rssi;
} ble_discovered_device_t;

void ble_pairing_init(void);
void ble_pairing_deinit(void);
ble_pair_state_t ble_pairing_get_state(void);

bool ble_pairing_start_advertise(const char* device_name);
void ble_pairing_stop_advertise(void);
bool ble_pairing_is_advertising(void);

bool ble_pairing_start_scan(uint8_t duration_sec);
void ble_pairing_stop_scan(void);
bool ble_pairing_is_scanning(void);

int  ble_pairing_get_discovered(ble_discovered_device_t* devices, int max_count);
bool ble_pairing_pair_with_device(int index);

uint8_t* ble_pairing_get_own_now_mac(void);

#ifdef __cplusplus
}
#endif

#endif