/**
 * @file control_server.c
 * @brief The mule's local HTTP server (see control_server.h).
 */

#include "control_server.h"
#include "config.h"
#include "log_ring.h"
#include "file_server.h"
#include "miner_link.h"
#include "ota_service.h"
#include "nvs_config.h"
#include "wifi_manager.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "control";
static httpd_handle_t s_server = NULL;

/* ── GET / ─────────────────────────────────────────────────────────
 *
 * Sent in one piece. The palette matches the setup portal deliberately: a user
 * meets the portal first and this page second, and they should look like the
 * same device rather than two unrelated tools.
 *
 * Buttons follow one convention so colour and icon agree: blue navigates,
 * green enables, red is destructive. Nothing here polls on a timer; every
 * refresh is a button press, because a page left open on a phone should not
 * keep waking a device whose whole job is to sit behind a CPAP machine.
 */
static const char PAGE_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>hms-mm</title><style>"
"body{background:#0F1120;color:#E0E0E0;font-family:system-ui;margin:0;padding:20px;}"
".wrap{max-width:520px;margin:0 auto;}"
".card{background:#1A1D35;border-radius:12px;padding:20px;margin-bottom:16px;}"
"h1{color:#667EEA;font-size:20px;margin:0 0 4px;}"
"h2{color:#888;font-size:12px;margin:0 0 16px;font-weight:normal;}"
"h3{color:#667EEA;font-size:13px;margin:0 0 12px;text-transform:uppercase;letter-spacing:.5px;}"
".btn{display:block;width:100%;padding:12px;margin:8px 0;background:#667EEA;color:#fff;"
"border:none;border-radius:8px;font-size:14px;font-weight:600;cursor:pointer;"
"text-align:center;text-decoration:none;box-sizing:border-box;}"
".btn-green{background:#16A34A;}.btn-red{background:transparent;border:1px solid #DC2626;color:#F87171;}"
".btn:disabled{background:#444;color:#888;cursor:default;}"
"table{width:100%;border-collapse:collapse;font-size:13px;}"
"td{padding:6px 0;border-bottom:1px solid #252840;}"
"td:first-child{color:#888;width:45%;}"
"td:last-child{text-align:right;font-family:ui-monospace,monospace;}"
"pre{background:#0B0D18;border-radius:8px;padding:12px;font-size:11px;line-height:1.5;"
"overflow-x:auto;white-space:pre-wrap;word-break:break-word;max-height:340px;margin:0;}"
".warn{color:#FBBF24;}.bad{color:#F87171;}.good{color:#4ADE80;}"
".note{color:#888;font-size:12px;margin:8px 0 0;}"
"</style></head><body><div class='wrap'>"

"<div class='card'>"
"<h1>hms-mm</h1><h2 id='sub'>Loading...</h2>"
"<table id='st'></table>"
"<button class='btn' onclick='load()'>Refresh status</button>"
"</div>"

"<div class='card'><h3>SD card</h3>"
"<a class='btn' href='/dir?dir=A:'>Browse card</a>"
"<a class='btn' href='/dir?dir=A:DATALOG'>CPAP data (DATALOG)</a>"
"</div>"

"<div class='card'><h3>O2 Ring</h3>"
"<div id='o2b'></div>"
"<p class='note'>The ring and the SD card share one radio, so enabling "
"Bluetooth interrupts card transfers. Leave it off unless you use a ring.</p>"
"</div>"

"<div class='card'><h3>Firmware</h3>"
"<a class='btn' href='/api/update'>Update firmware</a>"
"</div>"

"<div class='card'><h3>Logs</h3>"
"<button class='btn' onclick='logs()'>Show recent logs</button>"
"<pre id='lg' style='display:none'></pre>"
"</div>"

"<div class='card'><h3>Maintenance</h3>"
"<button class='btn' onclick='act(\"/api/reboot\",{},\"Restart the mule?\")'>Restart mule</button>"
"<button class='btn' onclick='act(\"/api/reboot\",{target:\"miner\"},\"Restart the miner?\")'>Restart miner</button>"
"<button class='btn btn-red' onclick='act(\"/api/reset\",{},"
"\"Forget the Wi-Fi network and return to setup? The card settings are kept.\")'>"
"Reset Wi-Fi</button>"
"</div>"

"</div><script>"
"function row(k,v,c){return '<tr><td>'+k+'</td><td'+(c?\" class='\"+c+\"'\":'')+'>'+v+'</td></tr>';}"
"function fmt(n){return n>=1024?(n/1024).toFixed(1)+' KB':n+' B';}"
"function load(){fetch('/api/status').then(r=>r.json()).then(d=>{"
"document.getElementById('sub').textContent='Firmware '+d.fw+' | miner '+d.miner_fw;"
"var h=row('Device',d.serial);"
"h+=row('Wi-Fi',d.wifi?('connected'+(d.rssi?' ('+d.rssi+' dBm)':'')):'down',d.wifi?'good':'bad');"
"h+=row('Uptime',d.uptime);"
"h+=row('Free memory',fmt(d.free_heap));"
"h+=row('Largest block',fmt(d.largest_block),d.largest_block<20480?'warn':'');"
"h+=row('Lowest ever',fmt(d.min_free));"
"if(d.ezshare){var e=d.ezshare,t,c;"
"if(!e.assoc&&e.reason==201){t='card not found';c='bad';}"
"else if(!e.assoc&&e.reason==202){t='wrong card password';c='bad';}"
"else if(e.assoc&&e.rssi<=-80){t='weak ('+e.rssi+' dBm)';c='warn';}"
"else if(e.assoc){t='ok ('+e.rssi+' dBm)';c='good';}"
"else{t='not connected (reason '+e.reason+')';c='warn';}"
"h+=row('Card link',t,c);h+=row('Card seen',e.age_s+'s ago');}"
"document.getElementById('st').innerHTML=h;"
"var on=d.o2_enabled;document.getElementById('o2b').innerHTML="
"'<button class=\"btn '+(on?'btn-red':'btn-green')+'\" onclick=\\'o2(!'+(on?'true':'false')+')\\'>'"
"+(on?'Disable O2 Ring':'Enable O2 Ring')+'</button>';"
"}).catch(e=>{document.getElementById('sub').textContent='Could not read status';});}"
"function o2(v){if(!confirm('The miner restarts to apply this. Continue?'))return;"
"post('/api/config',{o2_enabled:v});}"
"function act(u,b,msg){if(!confirm(msg))return;post(u,b);}"
"function post(u,b){fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify(b)}).then(r=>r.text()).then(t=>{alert(t);setTimeout(load,3000);})"
".catch(e=>alert('Request failed'));}"
"function logs(){fetch('/api/logs?n=120').then(r=>r.text()).then(t=>{"
"var p=document.getElementById('lg');p.style.display='block';p.textContent=t||'(empty)';"
"p.scrollTop=p.scrollHeight;});}"
"load();"
"</script></body></html>";

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

/* ── request body helper ──────────────────────────────────────────── */

/* Read a JSON body. Bounded: these are small control payloads, and an
 * unbounded read on a device with ~50 KB of heap is a denial of service. */
static cJSON *read_json_body(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0) return cJSON_CreateObject();      /* empty body is valid */
    if (total > 512) {
        ESP_LOGW(TAG, "rejecting %d byte body", total);
        return NULL;
    }

    char buf[513];
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) return NULL;
        got += r;
    }
    buf[got] = '\0';
    return cJSON_Parse(buf);
}

/* ── GET /api/status ──────────────────────────────────────────────── */

static esp_err_t handle_status(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_ERR_NO_MEM;
    }

    char serial[24] = {0};
    nvs_config_get_serial(serial, sizeof(serial));
    cJSON_AddStringToObject(json, "serial", serial[0] ? serial : "unknown");
    cJSON_AddStringToObject(json, "fw", FW_VERSION);
    cJSON_AddStringToObject(json, "miner_fw", miner_link_cached_version());
    cJSON_AddStringToObject(json, "state", "proxy");
    cJSON_AddBoolToObject(json, "wifi", wifi_manager_is_connected());

    int64_t up = esp_timer_get_time() / 1000000LL;
    char uptime[16];
    snprintf(uptime, sizeof(uptime), "%02d:%02d:%02d",
             (int)(up / 3600), (int)((up / 60) % 60), (int)(up % 60));
    cJSON_AddStringToObject(json, "uptime", uptime);

    wifi_ap_record_t ap;
    cJSON_AddNumberToObject(json, "rssi",
        esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0);

    cJSON_AddNumberToObject(json, "free_heap", (double)esp_get_free_heap_size());
    /* Fragmentation, not total free, is what fails an allocation on a C3: a
     * device can report plenty free and still have no contiguous block. */
    cJSON_AddNumberToObject(json, "largest_block",
        (double)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(json, "min_free", (double)esp_get_minimum_free_heap_size());

    /* From the cache, not the wire: opening the status page must never
     * interrupt a transfer. It is refreshed at boot and whenever the gate is
     * read or written explicitly. */
    cJSON_AddBoolToObject(json, "o2_enabled", miner_link_cached_o2_enabled());

    /* Update progress, so the firmware page can follow a pull it started and
     * a caller can tell "busy" from "wedged". */
    ota_status_t ota;
    ota_service_get_status(&ota);
    cJSON *upd = cJSON_AddObjectToObject(json, "update");
    if (upd) {
        cJSON_AddStringToObject(upd, "state", ota.state_str);
        cJSON_AddStringToObject(upd, "target", ota.target ? ota.target : "");
        cJSON_AddNumberToObject(upd, "written", (double)ota.written);
        cJSON_AddNumberToObject(upd, "total", (double)ota.total);
        if (ota.error) cJSON_AddStringToObject(upd, "error", ota.error);
    }

    miner_ezshare_diag_t d;
    miner_link_get_diag(&d);
    if (d.valid) {
        cJSON *ez = cJSON_AddObjectToObject(json, "ezshare");
        if (ez) {
            cJSON_AddBoolToObject(ez, "assoc", d.assoc);
            cJSON_AddNumberToObject(ez, "rssi", d.rssi);
            cJSON_AddNumberToObject(ez, "reason", d.reason);
            int64_t age_s = (esp_timer_get_time() - d.when_us) / 1000000LL;
            cJSON_AddNumberToObject(ez, "age_s", (double)age_s);
        }
    }

    char *out = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!out) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_ERR_NO_MEM;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t rc = httpd_resp_sendstr(req, out);
    free(out);
    return rc;
}

/* ── POST /api/reboot ─────────────────────────────────────────────── */

/* Restart after the response has actually been sent. Restarting inside the
 * handler drops the socket mid-reply, so the caller sees a network error and
 * cannot tell success from failure. */
static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

static esp_err_t handle_reboot(httpd_req_t *req)
{
    if (ota_service_reject_if_busy(req)) return ESP_FAIL;

    cJSON *body = read_json_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    cJSON *t = cJSON_GetObjectItem(body, "target");
    const char *target = cJSON_IsString(t) ? t->valuestring : "mule";
    bool do_miner = strcmp(target, "miner") == 0 || strcmp(target, "both") == 0;
    bool do_mule  = strcmp(target, "miner") != 0;
    cJSON_Delete(body);

    if (do_miner) {
        if (miner_link_reboot())
            ESP_LOGI(TAG, "miner restarting");
        else
            ESP_LOGW(TAG, "miner did not acknowledge the restart");
    }

    if (do_mule) {
        httpd_resp_sendstr(req, "Restarting. This page will be reachable again in a few seconds.");
        xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    } else {
        httpd_resp_sendstr(req, "Miner restarting.");
    }
    return ESP_OK;
}

/* ── POST /api/reset ──────────────────────────────────────────────── */

static esp_err_t handle_reset(httpd_req_t *req)
{
    if (ota_service_reject_if_busy(req)) return ESP_FAIL;

    /* Reset the miner first, while the link is still up: doing it after the
     * mule reboots would mean nothing is left to send the message. */
    if (!miner_link_reset())
        ESP_LOGW(TAG, "miner did not acknowledge the reset");

    nvs_config_clear_wifi();

    httpd_resp_sendstr(req,
        "Wi-Fi forgotten. The device restarts and reappears as a setup network.");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* ── POST /api/config ─────────────────────────────────────────────── */

static esp_err_t handle_config(httpd_req_t *req)
{
    if (ota_service_reject_if_busy(req)) return ESP_FAIL;

    cJSON *body = read_json_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }

    /* Fast path: toggling the O2 Ring is not provisioning, and must not
     * disturb stored credentials or restart the mule. */
    cJSON *o2 = cJSON_GetObjectItem(body, "o2_enabled");
    if (cJSON_IsBool(o2) && !cJSON_GetObjectItem(body, "wifi_ssid")) {
        bool want = cJSON_IsTrue(o2);
        cJSON_Delete(body);
        if (miner_link_set_o2_enabled(want)) {
            httpd_resp_sendstr(req, want ? "O2 Ring enabled. The miner is restarting."
                                         : "O2 Ring disabled. The miner is restarting.");
        } else {
            httpd_resp_set_status(req, "502 Bad Gateway");
            httpd_resp_sendstr(req, "The miner did not confirm the change.");
        }
        return ESP_OK;
    }

    cJSON *ws = cJSON_GetObjectItem(body, "wifi_ssid");
    cJSON *wp = cJSON_GetObjectItem(body, "wifi_pass");
    cJSON *es = cJSON_GetObjectItem(body, "ez_ssid");
    cJSON *ep = cJSON_GetObjectItem(body, "ez_pass");

    bool wrote = false;
    if (cJSON_IsString(ws) && ws->valuestring[0]) {
        nvs_config_set_wifi(ws->valuestring,
                            cJSON_IsString(wp) ? wp->valuestring : "");
        wrote = true;
    }
    if (cJSON_IsString(es) && es->valuestring[0]) {
        nvs_config_set_ezshare(es->valuestring,
                               cJSON_IsString(ep) ? ep->valuestring : "");
        wrote = true;
    }
    cJSON_Delete(body);

    if (!wrote) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "nothing to set");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "Saved. Restarting to apply.");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* ── lifecycle ────────────────────────────────────────────────────── */

esp_err_t control_server_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port     = CONTROL_HTTP_PORT;
    cfg.max_uri_handlers = 16;      /* control + data routes share this server */
    cfg.stack_size      = 8192;
    cfg.lru_purge_enable = true;

    esp_err_t rc = httpd_start(&s_server, &cfg);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(rc));
        s_server = NULL;
        return rc;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = handle_root   },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = handle_status },
        { .uri = "/api/reboot",  .method = HTTP_POST, .handler = handle_reboot },
        { .uri = "/api/reset",   .method = HTTP_POST, .handler = handle_reset  },
        { .uri = "/api/config",  .method = HTTP_POST, .handler = handle_config },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        httpd_register_uri_handler(s_server, &uris[i]);

    log_ring_register(s_server);      /* GET /api/logs */
    ota_service_register(s_server);   /* /api/update[...], /api/ota, /api/cancel_update */
    file_server_register(s_server);   /* /dir, /download, o2ring endpoints */

    ESP_LOGI(TAG, "Control server up on port %d", CONTROL_HTTP_PORT);
    return ESP_OK;
}

httpd_handle_t control_server_get_handle(void) { return s_server; }

void control_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
