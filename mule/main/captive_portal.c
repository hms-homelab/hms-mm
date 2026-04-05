#include "captive_portal.h"
#include "config.h"
#include "nvs_config.h"
#include "uart_handler.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "cJSON.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "portal";

/* ── DNS hijack server ── */

static TaskHandle_t s_dns_task = NULL;

static void dns_server_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(NULL); return; }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family = AF_INET, .sin_port = htons(53),
        .sin_addr.s_addr = inet_addr("192.168.4.1"),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock); vTaskDelete(NULL); return;
    }

    uint8_t portal_ip[4] = {192, 168, 4, 1};
    uint8_t buf[512];
    struct sockaddr_in client;
    socklen_t clen = sizeof(client);

    while (true) {
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&client, &clen);
        if (len < 12) continue;

        buf[2] = 0x81; buf[3] = 0x80;
        buf[6] = 0x00; buf[7] = 0x01;

        int pos = 12;
        while (pos < len && buf[pos] != 0) pos += buf[pos] + 1;
        pos += 5;
        if (pos + 16 > (int)sizeof(buf)) continue;

        buf[pos++] = 0xC0; buf[pos++] = 0x0C;
        buf[pos++] = 0x00; buf[pos++] = 0x01;
        buf[pos++] = 0x00; buf[pos++] = 0x01;
        buf[pos++] = 0; buf[pos++] = 0; buf[pos++] = 0; buf[pos++] = 0;
        buf[pos++] = 0x00; buf[pos++] = 0x04;
        memcpy(buf + pos, portal_ip, 4); pos += 4;

        sendto(sock, buf, pos, 0, (struct sockaddr *)&client, clen);
    }
}

/* ── HTML ── */

static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Miner-Mule Setup</title>"
    "<style>"
    "body{background:#0F1120;color:#E0E0E0;font-family:system-ui;margin:0;padding:20px;}"
    ".card{background:#1A1D35;border-radius:12px;padding:24px;max-width:380px;margin:40px auto;}"
    "h1{color:#667EEA;font-size:20px;margin:0 0 4px;}"
    "h2{color:#888;font-size:12px;margin:0 0 20px;font-weight:normal;}"
    "h3{color:#667EEA;font-size:14px;margin:20px 0 4px;border-top:1px solid #333;padding-top:16px;}"
    "label{display:block;color:#888;font-size:12px;margin:12px 0 4px;}"
    "select,input{width:100%;padding:10px;background:#252840;border:1px solid #333;border-radius:8px;"
    "color:#E0E0E0;font-size:14px;box-sizing:border-box;}"
    "button{width:100%;padding:12px;background:#667EEA;color:#fff;border:none;border-radius:8px;"
    "font-size:15px;font-weight:600;cursor:pointer;margin-top:20px;}"
    "button:disabled{background:#444;}"
    ".status{color:#4ADE80;font-size:13px;margin-top:12px;text-align:center;}"
    "</style></head><body>"
    "<div class='card'>"
    "<h1>hms-mm Setup</h1>"
    "<h2>Miner &amp; Mule WiFi Configuration</h2>"
    "<form id='f'>"
    "<label>Home WiFi Network</label>"
    "<select name='ssid' id='ssid'><option>Scanning...</option></select>"
    "<label>Home WiFi Password</label>"
    "<input type='password' name='pass' id='pass' placeholder='WiFi password'>"
    "<h3>ezShare SD Card</h3>"
    "<label>ezShare SSID</label>"
    "<input type='text' name='ez_ssid' id='ez_ssid' value='ez Share'>"
    "<label>ezShare Password</label>"
    "<input type='text' name='ez_pass' id='ez_pass' value='88888888'>"
    "<button type='submit' id='btn'>Save &amp; Reboot</button>"
    "</form>"
    "<div class='status' id='st'></div>"
    "</div>"
    "<script>"
    "fetch('/scan').then(r=>r.json()).then(d=>{"
    "let s=document.getElementById('ssid');s.innerHTML='';"
    "d.forEach(n=>{let o=document.createElement('option');o.value=n;o.textContent=n;s.appendChild(o);});"
    "}).catch(()=>{document.getElementById('st').textContent='Scan failed';});"
    "document.getElementById('f').onsubmit=function(e){"
    "e.preventDefault();let b=document.getElementById('btn');b.disabled=true;b.textContent='Saving...';"
    "fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:'ssid='+encodeURIComponent(document.getElementById('ssid').value)"
    "+'&pass='+encodeURIComponent(document.getElementById('pass').value)"
    "+'&ez_ssid='+encodeURIComponent(document.getElementById('ez_ssid').value)"
    "+'&ez_pass='+encodeURIComponent(document.getElementById('ez_pass').value)"
    "}).then(r=>r.text()).then(t=>{document.getElementById('st').textContent=t;});"
    "};"
    "</script></body></html>";

/* ── Helpers ── */

static size_t url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_size - 1; i++) {
        if (src[i] == '+') dst[j++] = ' ';
        else if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = {src[i+1], src[i+2], 0};
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else dst[j++] = src[i];
    }
    dst[j] = '\0';
    return j;
}

static bool parse_field(char *body, const char *name, char *out, size_t out_size)
{
    out[0] = '\0';
    size_t nlen = strlen(name);
    char *pos = body;
    while ((pos = strstr(pos, name)) != NULL) {
        if (pos != body && *(pos-1) != '&') { pos += nlen; continue; }
        if (pos[nlen] != '=') { pos += nlen; continue; }
        break;
    }
    if (!pos) return false;
    char *val = pos + nlen + 1;
    char *end = strchr(val, '&');
    char saved = 0;
    if (end) { saved = *end; *end = '\0'; }
    url_decode(val, out, out_size);
    if (end) *end = saved;
    return true;
}

/* ── HTTP handlers ── */

static esp_err_t handle_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_redirect(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t handle_scan(httpd_req_t *req) {
    uint16_t num = 0;
    esp_wifi_scan_start(NULL, true);
    esp_wifi_scan_get_ap_num(&num);
    if (num > 20) num = 20;
    wifi_ap_record_t *aps = calloc(num, sizeof(wifi_ap_record_t));
    esp_wifi_scan_get_ap_records(&num, aps);

    char buf[1024] = "[";
    size_t pos = 1;
    for (int i = 0; i < num; i++) {
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\"%s\"", (char *)aps[i].ssid);
        if (pos >= sizeof(buf) - 10) break;
    }
    buf[pos++] = ']'; buf[pos] = 0;
    free(aps);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, pos);
}

static esp_err_t handle_save(httpd_req_t *req) {
    char body[512] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data"); return ESP_FAIL; }
    body[len] = 0;

    char ssid[33] = {0}, pass[65] = {0};
    char ez_ssid[33] = {0}, ez_pass[65] = {0};
    parse_field(body, "ssid", ssid, sizeof(ssid));
    parse_field(body, "pass", pass, sizeof(pass));
    parse_field(body, "ez_ssid", ez_ssid, sizeof(ez_ssid));
    parse_field(body, "ez_pass", ez_pass, sizeof(ez_pass));

    if (strlen(ssid) == 0) {
        return httpd_resp_send(req, "Please select a network", HTTPD_RESP_USE_STRLEN);
    }

    // Store home WiFi in mule NVS
    nvs_config_set_wifi(ssid, pass);

    // Store ezShare in mule NVS (for reference)
    if (strlen(ez_ssid) > 0) {
        nvs_config_set_ezshare(ez_ssid, ez_pass);
    }

    // Send ezShare creds to miner via UART so it stores them in its own NVS
    if (strlen(ez_ssid) > 0) {
        cJSON *cfg = cJSON_CreateObject();
        cJSON_AddStringToObject(cfg, "type", "set_config");
        cJSON_AddStringToObject(cfg, "ez_ssid", ez_ssid);
        cJSON_AddStringToObject(cfg, "ez_pass", ez_pass);
        char *json_str = cJSON_PrintUnformatted(cfg);
        if (json_str) {
            uart_send_json(json_str);
            free(json_str);
            ESP_LOGI(TAG, "Sent ezShare creds to miner via UART");
        }
        cJSON_Delete(cfg);

        // Wait for miner to ACK and reboot
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    httpd_resp_send(req, "Saved! Rebooting both devices...", HTTPD_RESP_USE_STRLEN);

    if (s_dns_task) { vTaskDelete(s_dns_task); s_dns_task = NULL; }
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

/* ── Public API ── */

void captive_portal_start(void)
{
    char serial[16] = "0000";
    nvs_config_get_serial(serial, sizeof(serial));
    size_t slen = strlen(serial);
    const char *suffix = slen >= 4 ? serial + slen - 4 : serial;
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "MM-Setup-%s", suffix);

    ESP_LOGI(TAG, "Starting captive portal AP: %s", ap_ssid);

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    ap_config.ap.channel = PORTAL_AP_CHANNEL;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.max_connection = PORTAL_MAX_CONN;

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();

    xTaskCreate(dns_server_task, "dns_hijack", 4096, NULL, 5, &s_dns_task);

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_uri_handlers = 8;
    http_cfg.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    httpd_start(&server, &http_cfg);

    httpd_uri_t u1 = { .uri = "/",     .method = HTTP_GET,  .handler = handle_root };
    httpd_uri_t u2 = { .uri = "/scan",  .method = HTTP_GET,  .handler = handle_scan };
    httpd_uri_t u3 = { .uri = "/save",  .method = HTTP_POST, .handler = handle_save };
    httpd_uri_t u4 = { .uri = "/*",     .method = HTTP_GET,  .handler = handle_redirect };

    httpd_register_uri_handler(server, &u1);
    httpd_register_uri_handler(server, &u2);
    httpd_register_uri_handler(server, &u3);
    httpd_register_uri_handler(server, &u4);

    ESP_LOGI(TAG, "Captive portal at http://192.168.4.1/");

    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
