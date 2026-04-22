#pragma once
#include "esp_http_server.h"

void file_server_init(void);
void file_server_register(httpd_handle_t server);
