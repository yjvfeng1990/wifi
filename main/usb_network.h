#ifndef USB_NETWORK_H
#define USB_NETWORK_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t usb_network_init(void);
esp_netif_t* usb_network_get_netif(void);
void usb_network_reconnect(void);
uint64_t usb_network_get_rx_total(void);
uint64_t usb_network_get_tx_total(void);

#ifdef __cplusplus
}
#endif

#endif