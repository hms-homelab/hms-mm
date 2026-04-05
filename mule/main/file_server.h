#pragma once
#include "esp_http_server.h"

// Register HTTP endpoints for serving cached files.
// GET /dir?dir=A:DATALOG          -> HTML listing of date folders
// GET /dir?dir=A:DATALOG\YYYYMMDD -> HTML listing of files
// GET /download?file=path         -> Raw file bytes
// GET /api/status                 -> JSON status
void file_server_register(httpd_handle_t server);
