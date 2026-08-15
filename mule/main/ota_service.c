/**
 * @file ota_service.c
 * @brief Firmware update endpoints (see ota_service.h).
 */

#include "ota_service.h"
#include "ota_miner.h"
#include "config.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ota";

/* Bytes pulled per read. Small on purpose: this buffer is live at the same
 * time as the TLS record buffer, and contiguous heap is the scarce resource on
 * this chip, not throughput. */
#define OTA_HTTP_BUF        2048
#define OTA_HTTP_TIMEOUT_MS 20000

static volatile ota_state_t s_state;
static volatile bool        s_cancel;
static const char          *s_target = "";
static volatile uint32_t    s_written;
static volatile uint32_t    s_total;
static const char          *s_error;
static TaskHandle_t         s_worker;

/* ── status and lockout ───────────────────────────────────────────── */

void ota_service_get_status(ota_status_t *out)
{
    if (!out) return;
    out->state = s_state;
    out->state_str = (s_state == OTA_IDLE)    ? "idle"
                   : (s_state == OTA_RUNNING) ? "running"
                                              : "finalizing";
    out->target  = s_target;
    out->written = s_written;
    out->total   = s_total;
    out->error   = s_error;
}

bool ota_service_reject_if_busy(httpd_req_t *req)
{
    if (s_state == OTA_IDLE) return false;
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_hdr(req, "Retry-After", "30");
    httpd_resp_sendstr(req, "A firmware update is in progress.");
    return true;
}

void ota_service_confirm_boot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return;

    /* Reaching here means WiFi came up and the server started, which is as
     * much as this board can prove about itself. */
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
        ESP_LOGW(TAG, "new image confirmed valid");
}

/* ── applying to the mule itself ──────────────────────────────────── */

static esp_err_t mule_apply(esp_http_client_handle_t client, int content_len)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return ESP_ERR_NOT_FOUND;
    if (content_len > 0 && (size_t)content_len > part->size) {
        s_error = "image is larger than the partition";
        return ESP_ERR_INVALID_SIZE;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(part,
                                  content_len > 0 ? (size_t)content_len : OTA_SIZE_UNKNOWN,
                                  &handle);
    if (err != ESP_OK) { s_error = "could not open the partition"; return err; }

    char *buf = malloc(OTA_HTTP_BUF);
    if (!buf) { esp_ota_abort(handle); s_error = "out of memory"; return ESP_ERR_NO_MEM; }

    while (!s_cancel) {
        int n = esp_http_client_read(client, buf, OTA_HTTP_BUF);
        if (n < 0) { err = ESP_FAIL; s_error = "download failed"; break; }
        if (n == 0) break;                       /* complete */
        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) { s_error = "flash write failed"; break; }
        s_written += n;
    }
    free(buf);

    if (s_cancel)             { esp_ota_abort(handle); s_error = "cancelled"; return ESP_FAIL; }
    if (err != ESP_OK)        { esp_ota_abort(handle); return err; }
    if (s_written == 0)       { esp_ota_abort(handle); s_error = "server sent no data"; return ESP_FAIL; }

    s_state = OTA_FINALIZING;
    err = esp_ota_end(handle);                   /* validates, including SHA256 */
    if (err != ESP_OK) { s_error = "image failed validation"; return err; }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) { s_error = "could not set the boot partition"; return err; }
    return ESP_OK;
}

/* ── applying to the miner ────────────────────────────────────────── */

static esp_err_t miner_apply(esp_http_client_handle_t client, int content_len)
{
    /* The miner must be told the exact size up front: it refuses a short
     * image, which is the check that catches a truncated download. */
    if (content_len <= 0) {
        s_error = "the server did not report a size, which the miner requires";
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ota_miner_begin((uint32_t)content_len);
    if (err != ESP_OK) { s_error = "the miner would not start an update"; return err; }

    char *buf = malloc(OTA_HTTP_BUF);
    if (!buf) { ota_miner_end(false); s_error = "out of memory"; return ESP_ERR_NO_MEM; }

    while (!s_cancel) {
        int n = esp_http_client_read(client, buf, OTA_HTTP_BUF);
        if (n < 0) { err = ESP_FAIL; s_error = "download failed"; break; }
        if (n == 0) break;
        err = ota_miner_write((const uint8_t *)buf, n);
        if (err != ESP_OK) { s_error = "the miner rejected a chunk"; break; }
        s_written = ota_miner_written();
    }
    free(buf);

    if (s_cancel) { ota_miner_end(false); s_error = "cancelled"; return ESP_FAIL; }
    if (err != ESP_OK) { ota_miner_end(false); return err; }

    s_state = OTA_FINALIZING;
    err = ota_miner_end(true);
    if (err != ESP_OK) s_error = "the miner rejected the finished image";
    return err;
}

/* ── the URL-pull worker ──────────────────────────────────────────── */

typedef struct { char url[256]; bool miner; } ota_job_t;

static void ota_worker(void *arg)
{
    ota_job_t *job = (ota_job_t *)arg;

    esp_http_client_config_t cfg = {
        .url = job->url,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = false,
        /* Verify against the bundled roots. This installs executable code, so
         * an unverified connection would let anyone on the path choose the
         * firmware. http:// URLs skip TLS entirely and are the lighter path on
         * a LAN. */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = ESP_FAIL;

    if (!client) {
        s_error = "could not open the connection";
    } else if ((err = esp_http_client_open(client, 0)) != ESP_OK) {
        s_error = "could not reach the server";
    } else {
        int content_len = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGE(TAG, "server returned %d", status);
            s_error = "the server did not return the file";
            err = ESP_FAIL;
        } else {
            s_total = content_len > 0 ? (uint32_t)content_len : 0;
            ESP_LOGW(TAG, "updating %s from %s (%d B)",
                     job->miner ? "miner" : "mule", job->url, content_len);
            err = job->miner ? miner_apply(client, content_len)
                             : mule_apply(client, content_len);
        }
        esp_http_client_close(client);
    }
    if (client) esp_http_client_cleanup(client);

    bool was_mule = !job->miner;
    free(job);

    if (err == ESP_OK) {
        s_error = NULL;
        s_state = OTA_IDLE;
        if (was_mule) {
            ESP_LOGW(TAG, "update applied, restarting");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
    } else {
        ESP_LOGE(TAG, "update failed: %s", s_error ? s_error : "unknown");
        s_state = OTA_IDLE;
    }

    s_worker = NULL;
    vTaskDelete(NULL);
}

/* ── POST /api/ota ────────────────────────────────────────────────── */

static esp_err_t handle_ota(httpd_req_t *req)
{
    if (ota_service_reject_if_busy(req)) return ESP_FAIL;

    int len = req->content_len;
    if (len <= 0 || len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    char body[513];
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "short body"); return ESP_FAIL; }
        got += r;
    }
    body[got] = '\0';

    cJSON *j = cJSON_Parse(body);
    if (!j) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad JSON"); return ESP_FAIL; }
    cJSON *url_j = cJSON_GetObjectItem(j, "url");
    cJSON *tgt_j = cJSON_GetObjectItem(j, "target");

    if (!cJSON_IsString(url_j) ||
        (strncmp(url_j->valuestring, "http://", 7) != 0 &&
         strncmp(url_j->valuestring, "https://", 8) != 0)) {
        cJSON_Delete(j);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "url must be http or https");
        return ESP_FAIL;
    }

    ota_job_t *job = calloc(1, sizeof(*job));
    if (!job) { cJSON_Delete(j); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
    strncpy(job->url, url_j->valuestring, sizeof(job->url) - 1);
    job->miner = cJSON_IsString(tgt_j) && strcmp(tgt_j->valuestring, "miner") == 0;
    cJSON_Delete(j);

    s_state   = OTA_RUNNING;
    s_cancel  = false;
    s_written = 0;
    s_total   = 0;
    s_error   = NULL;
    s_target  = job->miner ? "miner" : "mule";

    /* 8 KB: TLS, the HTTP client and base64 all run on this stack. */
    if (xTaskCreate(ota_worker, "ota", 8192, job, 5, &s_worker) != pdPASS) {
        s_state = OTA_IDLE;
        free(job);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not start");
        return ESP_FAIL;
    }

    /* 202: the work outlives this request. Poll /api/status to follow it. */
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_sendstr(req, "Update started. Watch /api/status for progress.");
    return ESP_OK;
}

static esp_err_t handle_cancel(httpd_req_t *req)
{
    if (s_state == OTA_IDLE) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "No update is running.");
        return ESP_OK;
    }
    s_cancel = true;
    httpd_resp_sendstr(req, "Cancelling.");
    return ESP_OK;
}

/* ── body-upload: POST /api/update/{mule,miner} ───────────────────── */

static esp_err_t handle_upload(httpd_req_t *req, bool miner)
{
    if (ota_service_reject_if_busy(req)) return ESP_FAIL;

    int total = req->content_len;
    if (total <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no image in the body");
        return ESP_FAIL;
    }

    s_state   = OTA_RUNNING;
    s_cancel  = false;
    s_written = 0;
    s_total   = (uint32_t)total;
    s_error   = NULL;
    s_target  = miner ? "miner" : "mule";

    char *buf = malloc(OTA_HTTP_BUF);
    esp_ota_handle_t handle = 0;
    const esp_partition_t *part = NULL;
    esp_err_t err = ESP_OK;

    if (!buf) {
        err = ESP_ERR_NO_MEM;
    } else if (miner) {
        err = ota_miner_begin((uint32_t)total);
    } else {
        part = esp_ota_get_next_update_partition(NULL);
        if (!part || (size_t)total > part->size) err = ESP_ERR_INVALID_SIZE;
        else err = esp_ota_begin(part, total, &handle);
    }

    int received = 0;
    while (err == ESP_OK && received < total) {
        int want = total - received;
        if (want > OTA_HTTP_BUF) want = OTA_HTTP_BUF;
        int n = httpd_req_recv(req, buf, want);
        if (n <= 0) { err = ESP_FAIL; s_error = "upload interrupted"; break; }
        err = miner ? ota_miner_write((const uint8_t *)buf, n)
                    : esp_ota_write(handle, buf, n);
        received += n;
        s_written = (uint32_t)received;
    }
    free(buf);

    if (err == ESP_OK) {
        s_state = OTA_FINALIZING;
        if (miner) {
            err = ota_miner_end(true);
        } else {
            err = esp_ota_end(handle);
            if (err == ESP_OK) err = esp_ota_set_boot_partition(part);
        }
    } else {
        if (miner) ota_miner_end(false);
        else if (handle) esp_ota_abort(handle);
    }

    s_state = OTA_IDLE;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "upload failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, s_error ? s_error : "Update failed.");
        return ESP_FAIL;
    }

    if (miner) {
        httpd_resp_sendstr(req, "Miner updated and restarting.");
    } else {
        httpd_resp_sendstr(req, "Updated. Restarting.");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    return ESP_OK;
}

static esp_err_t handle_upload_mule(httpd_req_t *req)  { return handle_upload(req, false); }
static esp_err_t handle_upload_miner(httpd_req_t *req) { return handle_upload(req, true); }

/* ── GET /api/update ──────────────────────────────────────────────── */

static const char UPDATE_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>hms-mm firmware</title><style>"
"body{background:#0F1120;color:#E0E0E0;font-family:system-ui;margin:0;padding:20px;}"
".wrap{max-width:520px;margin:0 auto;}"
".card{background:#1A1D35;border-radius:12px;padding:20px;margin-bottom:16px;}"
"h1{color:#667EEA;font-size:20px;margin:0 0 4px;}"
"h3{color:#667EEA;font-size:13px;margin:0 0 12px;text-transform:uppercase;letter-spacing:.5px;}"
"label{display:block;color:#888;font-size:12px;margin:10px 0 4px;}"
"input{width:100%;padding:10px;background:#252840;border:1px solid #333;border-radius:8px;"
"color:#E0E0E0;font-size:13px;box-sizing:border-box;}"
".btn{display:block;width:100%;padding:12px;margin:12px 0 0;background:#667EEA;color:#fff;"
"border:none;border-radius:8px;font-size:14px;font-weight:600;cursor:pointer;}"
".btn:disabled{background:#444;color:#888;}"
"a{color:#667EEA;}.note{color:#888;font-size:12px;margin:8px 0 0;}"
".bar{height:6px;background:#252840;border-radius:3px;margin-top:12px;overflow:hidden;}"
".bar div{height:100%;width:0;background:#4ADE80;transition:width .3s;}"
"#st{font-size:12px;color:#888;margin-top:8px;}"
"</style></head><body><div class='wrap'>"
"<div class='card'><h1>Firmware</h1>"
"<p class='note'>Flash the two boards as a pair when a release says the link "
"protocol changed. The mule restarts itself; the miner is updated through it.</p></div>"

"<div class='card'><h3>From a URL</h3>"
"<label>Image address</label>"
"<input id='u' placeholder='http://nas.local/mule-2026.0.6.bin'>"
"<label>Board</label>"
"<input id='t' value='mule'>"
"<button class='btn' onclick='pull()'>Fetch and install</button>"
"<div class='bar'><div id='pb'></div></div><div id='st'></div>"
"<p class='note'>The device downloads it directly, so the address has to be "
"reachable from the device, not just from this browser.</p></div>"

"<div class='card'><h3>From a file</h3>"
"<label>Board</label><input id='t2' value='mule'>"
"<label>Image</label><input type='file' id='f'>"
"<button class='btn' onclick='up()'>Upload and install</button>"
"<div class='bar'><div id='pb2'></div></div><div id='st2'></div>"
"<p class='note'>No network needed. Use this when the device cannot reach "
"wherever the file lives.</p></div>"

"<div class='card'><a href='/'>Back to the device page</a></div>"
"</div><script>"
"function pull(){var u=document.getElementById('u').value.trim();"
"var t=document.getElementById('t').value.trim()||'mule';if(!u)return;"
"fetch('/api/ota',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({target:t,url:u})}).then(r=>r.text()).then(x=>{"
"document.getElementById('st').textContent=x;poll();});}"
"var ran=false;"
"function poll(){fetch('/api/status').then(r=>r.json()).then(d=>{var o=d.update;if(!o)return;"
"var pct=o.total?Math.round(100*o.written/o.total):0;"
"if(o.state!='idle'){ran=true;"
"document.getElementById('pb').style.width=pct+'%';"
"document.getElementById('st').textContent=o.state+' '+(o.total?pct+'%':o.written+' B');"
"setTimeout(poll,1000);return;}"
"if(!ran)return;"
"if(o.error){document.getElementById('st').textContent='Failed: '+o.error;}"
"else{document.getElementById('pb').style.width='100%';"
"document.getElementById('st').textContent='Complete. The device is restarting.';}});}"
"function up(){var f=document.getElementById('f').files[0];if(!f)return;"
"var t=document.getElementById('t2').value.trim()||'mule';"
"var x=new XMLHttpRequest();x.open('POST','/api/update/'+t);"
"x.upload.onprogress=function(e){if(e.lengthComputable){"
"var p=Math.round(100*e.loaded/e.total);document.getElementById('pb2').style.width=p+'%';"
"document.getElementById('st2').textContent=p+'%';}};"
"x.onload=function(){document.getElementById('st2').textContent=x.responseText;};"
"x.onerror=function(){document.getElementById('st2').textContent='Upload failed';};"
"x.send(f);}"
"</script></body></html>";

static esp_err_t handle_update_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, UPDATE_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t ota_service_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        { .uri = "/api/update",        .method = HTTP_GET,  .handler = handle_update_page  },
        { .uri = "/api/update/mule",   .method = HTTP_POST, .handler = handle_upload_mule  },
        { .uri = "/api/update/miner",  .method = HTTP_POST, .handler = handle_upload_miner },
        { .uri = "/api/ota",           .method = HTTP_POST, .handler = handle_ota          },
        { .uri = "/api/cancel_update", .method = HTTP_POST, .handler = handle_cancel       },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        httpd_register_uri_handler(server, &uris[i]);
    return ESP_OK;
}
