#include "file_server.h"
#include "file_cache.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "file_srv";

#define MAX_PATH 256

static void normalize_path(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '%' && r[1] == '5' && (r[2] == 'C' || r[2] == 'c')) {
            *w++ = '/'; r += 3;
        } else if (*r == '\\') {
            *w++ = '/'; r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

// GET /dir?dir=A:DATALOG[/YYYYMMDD]
static esp_err_t handle_dir(httpd_req_t *req)
{
    char query[MAX_PATH * 2] = {0};
    char dir_param[MAX_PATH] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "dir", dir_param, sizeof(dir_param));
    }
    if (dir_param[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing dir param");
        return ESP_OK;
    }

    char *rel_path = dir_param;
    if (rel_path[0] == 'A' && rel_path[1] == ':') rel_path += 2;
    normalize_path(rel_path);
    if (rel_path[0] == '/') rel_path++;

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, "<html><body><pre>\r\n");

    size_t count = 0;
    const cached_file_t *files = file_cache_get_all(&count);

    // Determine if listing date folders or files within a folder
    bool is_root = (strcmp(rel_path, "DATALOG") == 0 || strcmp(rel_path, "DATALOG/") == 0);

    if (is_root) {
        // List unique date folders from cached file paths
        char seen[32][16];
        int seen_count = 0;

        for (size_t i = 0; i < count; i++) {
            // Extract date folder from path like "DATALOG/20260204/file.edf"
            const char *p = files[i].path;
            if (strncmp(p, "DATALOG/", 8) == 0 || strncmp(p, "DATALOG\\", 8) == 0) {
                char folder[16] = {0};
                const char *start = p + 8;
                const char *slash = strchr(start, '/');
                if (!slash) slash = strchr(start, '\\');
                if (slash) {
                    size_t len = slash - start;
                    if (len < sizeof(folder)) {
                        strncpy(folder, start, len);
                        // Check if already seen
                        bool found = false;
                        for (int j = 0; j < seen_count; j++) {
                            if (strcmp(seen[j], folder) == 0) { found = true; break; }
                        }
                        if (!found && seen_count < 32) {
                            strncpy(seen[seen_count], folder, sizeof(seen[0]));
                            seen_count++;
                            char line[256];
                            snprintf(line, sizeof(line),
                                     "                    &lt;DIR&gt;   "
                                     "<a href=\"dir?dir=A:DATALOG\\%s\"> %s</a>\r\n",
                                     folder, folder);
                            httpd_resp_sendstr_chunk(req, line);
                        }
                    }
                }
            }
        }
    } else {
        // List files matching this directory
        for (size_t i = 0; i < count; i++) {
            char norm_path[MAX_PATH];
            strncpy(norm_path, files[i].path, sizeof(norm_path));
            normalize_path(norm_path);

            // Check if file is in this directory
            size_t dir_len = strlen(rel_path);
            if (strncmp(norm_path, rel_path, dir_len) == 0 && norm_path[dir_len] == '/') {
                const char *filename = norm_path + dir_len + 1;
                if (strchr(filename, '/') == NULL) {  // No deeper subdirectories
                    long size_kb = (long)((files[i].size + 1023) / 1024);
                    if (size_kb < 1) size_kb = 1;
                    char line[512];
                    snprintf(line, sizeof(line),
                             "                   %5ldKB  "
                             "<a href=\"download?file=%s\"> %s</a>\r\n",
                             size_kb, files[i].path, filename);
                    httpd_resp_sendstr_chunk(req, line);
                }
            }
        }
    }

    httpd_resp_sendstr_chunk(req, "</pre></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// GET /download?file=DATALOG\YYYYMMDD\filename
static esp_err_t handle_download(httpd_req_t *req)
{
    char query[MAX_PATH * 2] = {0};
    char file_param[MAX_PATH] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "file", file_param, sizeof(file_param)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing file param");
        return ESP_OK;
    }

    // Try both normalized and original path
    const cached_file_t *f = file_cache_get(file_param);
    if (!f) {
        normalize_path(file_param);
        f = file_cache_get(file_param);
    }

    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_send(req, (const char *)f->content, f->size);

    ESP_LOGI(TAG, "GET /download %s (%zu bytes)", f->path, f->size);
    return ESP_OK;
}

// GET /api/status
static esp_err_t handle_status(httpd_req_t *req)
{
    int64_t up = esp_timer_get_time() / 1000000LL;
    int secs = (int)(up % 60), mins = (int)((up / 60) % 60), hrs = (int)(up / 3600);

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"FILE_SERVER\",\"wifi\":%s,"
             "\"cached_files\":%zu,\"cached_bytes\":%zu,"
             "\"uptime\":\"%dh%02dm%02ds\"}",
             wifi_manager_is_connected() ? "true" : "false",
             file_cache_count(), file_cache_total_bytes(),
             hrs, mins, secs);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

void file_server_register(httpd_handle_t server)
{
    httpd_uri_t uris[] = {
        { .uri = "/dir",        .method = HTTP_GET, .handler = handle_dir },
        { .uri = "/download",   .method = HTTP_GET, .handler = handle_download },
        { .uri = "/api/status", .method = HTTP_GET, .handler = handle_status },
    };
    for (int i = 0; i < 3; i++) httpd_register_uri_handler(server, &uris[i]);
    ESP_LOGI(TAG, "Registered: /dir /download /api/status");
}
