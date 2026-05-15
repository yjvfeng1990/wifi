#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdint.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    httpd_handle_t server;
} WebServer;

void web_server_start(WebServer* ws);
void web_server_stop(WebServer* ws);

#ifdef __cplusplus
}
#endif

#endif