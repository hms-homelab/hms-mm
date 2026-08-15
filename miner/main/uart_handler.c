/**
 * @file uart_handler.c
 * @brief Scanner C3 UART Handler - JSON Protocol Communication
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "cJSON.h"
#include "config.h"

static const char *TAG = LOG_TAG_UART;
static QueueHandle_t uart_queue = NULL;
static bool uart_initialized = false;

/* Serialises whole frames onto the wire. The mule has always had this; the
 * miner did not, because for a long time scanner_task was the only thing that
 * ever transmitted. Splitting the O2Ring BLE work into its own task makes that
 * assumption false, and two interleaved writes would produce one unparseable
 * line plus one lost frame — the kind of corruption that reads as a flaky
 * cable. Added ahead of the split so the ordering is never briefly wrong. */
static SemaphoreHandle_t uart_tx_mutex = NULL;

/**
 * @brief Initialize UART with JSON protocol settings
 */
esp_err_t uart_handler_init(void) {
    if (uart_initialized) {
        ESP_LOGW(TAG, "UART already initialized");
        return ESP_OK;
    }

    // Configure UART parameters
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // Install UART driver
    esp_err_t ret = uart_driver_install(UART_PORT_NUM, UART_RX_BUFFER_SIZE,
                                        UART_TX_BUFFER_SIZE, UART_QUEUE_SIZE,
                                        &uart_queue, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure UART parameters
    ret = uart_param_config(UART_PORT_NUM, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART parameters: %s", esp_err_to_name(ret));
        uart_driver_delete(UART_PORT_NUM);
        return ret;
    }

    // Set UART pins
    ret = uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(ret));
        uart_driver_delete(UART_PORT_NUM);
        return ret;
    }

    uart_tx_mutex = xSemaphoreCreateMutex();
    if (uart_tx_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create UART TX mutex");
        uart_driver_delete(UART_PORT_NUM);
        return ESP_ERR_NO_MEM;
    }

    uart_initialized = true;
    ESP_LOGI(TAG, "UART initialized successfully (TX: GPIO%d, RX: GPIO%d, Baud: %d)",
             UART_TX_PIN, UART_RX_PIN, UART_BAUD_RATE);

    return ESP_OK;
}

/**
 * @brief Send JSON message via UART
 * @param json_str JSON string to send (must NOT include newline)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t uart_send_json(const char *json_str) {
    if (!uart_initialized) {
        ESP_LOGE(TAG, "UART not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (json_str == NULL) {
        ESP_LOGE(TAG, "NULL JSON string");
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(json_str);
    if (len == 0) {
        ESP_LOGE(TAG, "Empty JSON string");
        return ESP_ERR_INVALID_ARG;
    }

    /* Hold across BOTH writes: the body and its newline terminator must stay
     * adjacent on the wire, or two concurrent senders produce one corrupt
     * frame and one orphaned fragment. */
    if (xSemaphoreTake(uart_tx_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "UART TX mutex timeout — frame dropped");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_OK;

    // Send JSON string
    if (uart_write_bytes(UART_PORT_NUM, json_str, len) < 0) {
        ESP_LOGE(TAG, "Failed to write UART bytes");
        result = ESP_FAIL;
    } else if (uart_write_bytes(UART_PORT_NUM, "\n", 1) < 0) {
        // Send newline delimiter
        ESP_LOGE(TAG, "Failed to write newline");
        result = ESP_FAIL;
    } else {
        // Wait for transmission to complete
        uart_wait_tx_done(UART_PORT_NUM, pdMS_TO_TICKS(1000));
        ESP_LOGD(TAG, "Sent JSON (%u bytes): %.100s...", (unsigned)len, json_str);
    }

    xSemaphoreGive(uart_tx_mutex);
    return result;
}

/* ── Line assembler ──────────────────────────────────────────────────────
 *
 * The driver hands us an undelimited byte stream; frames are newline
 * terminated. A block read can therefore return a partial frame, one whole
 * frame, or several frames plus a fragment of the next — so whatever is not
 * yet consumed has to survive between calls. That is what s_accum is for, and
 * what makes block reads safe here.
 *
 * The previous implementation read ONE BYTE PER uart_read_bytes() call, which
 * never over-reads and so needed no buffer. At 115200 that cost ~11k driver
 * calls/second; at 921600 it would be ~92k, which is the wrong shape of work
 * to be doing per byte on a C3 that is also driving WiFi or BLE.
 */
static char   s_accum[UART_LINE_MAX];
static size_t s_accum_len;

/* Pull whatever the driver has into the accumulator, blocking at most
 * wait_ms. Returns the driver's return value (<0 on error). */
static int accum_fill(uint32_t wait_ms)
{
    if (s_accum_len >= sizeof(s_accum)) return 0;      /* full; let the caller drain */
    int n = uart_read_bytes(UART_PORT_NUM,
                            (uint8_t *)s_accum + s_accum_len,
                            sizeof(s_accum) - s_accum_len,
                            pdMS_TO_TICKS(wait_ms));
    if (n > 0) s_accum_len += (size_t)n;
    return n;
}

/* Consume one complete frame if the accumulator holds one. Writes it to out
 * NUL-terminated with the newline stripped, and returns its length.
 * Returns -1 when no complete frame is buffered yet. */
static int accum_take_line(char *out, size_t out_size)
{
    char *nl = memchr(s_accum, '\n', s_accum_len);
    if (!nl) {
        /* No terminator and no room left: this frame can never complete, so
         * drop everything and resync on the next newline. Without this the
         * link would wedge permanently on one oversized/corrupt frame. */
        if (s_accum_len >= sizeof(s_accum)) {
            ESP_LOGE(TAG, "no frame terminator in %u B — dropping buffer to resync",
                     (unsigned)s_accum_len);
            s_accum_len = 0;
        }
        return -1;
    }

    size_t line_len = (size_t)(nl - s_accum);
    size_t consumed = line_len + 1;                    /* frame + its newline */
    size_t copy     = line_len < out_size - 1 ? line_len : out_size - 1;
    if (copy < line_len)
        ESP_LOGW(TAG, "frame of %u B truncated into a %u B buffer",
                 (unsigned)line_len, (unsigned)out_size);

    memcpy(out, s_accum, copy);
    out[copy] = '\0';

    s_accum_len -= consumed;
    memmove(s_accum, s_accum + consumed, s_accum_len);
    return (int)copy;
}

/**
 * @brief Non-blocking peek for a mid-stream "proxy_abort" from the mule.
 *
 * Called between chunks while streaming, so it must be cheap and must NOT
 * disturb anything else in flight. Unlike the original it does not consume
 * what it finds: a complete frame that is not an abort is left in the
 * accumulator for uart_receive_json() to deliver normally. The old version
 * read up to 256 bytes and threw them away, which was only safe while
 * proxy_abort was the sole message the mule could send mid-stream — adding
 * OTA, version and reboot frames breaks that assumption, and a swallowed
 * command would look exactly like a link fault.
 */
bool uart_check_proxy_abort(void)
{
    if (!uart_initialized) return false;

    accum_fill(0);                                     /* non-blocking */
    if (s_accum_len == 0) return false;

    char *nl = memchr(s_accum, '\n', s_accum_len);
    if (!nl) return false;                             /* frame still arriving */

    /* Cheap gate first: the quoted form only appears as a JSON *value*, and a
     * parse on every peek would be wasteful when the buffered frame is some
     * unrelated command we are deliberately leaving in place. */
    size_t line_len = (size_t)(nl - s_accum);
    *nl = '\0';
    bool looks_like_abort = strstr(s_accum, "\"proxy_abort\"") != NULL;
    *nl = '\n';
    if (!looks_like_abort) return false;

    /* Confirm it really is one before consuming it. */
    char saved = s_accum[line_len];
    s_accum[line_len] = '\0';
    cJSON *msg = cJSON_Parse(s_accum);
    s_accum[line_len] = saved;

    bool is_abort = false;
    if (msg) {
        cJSON *type = cJSON_GetObjectItem(msg, "type");
        is_abort = cJSON_IsString(type) && strcmp(type->valuestring, "proxy_abort") == 0;
        cJSON_Delete(msg);
    }
    if (!is_abort) return false;

    size_t consumed = line_len + 1;
    s_accum_len -= consumed;
    memmove(s_accum, s_accum + consumed, s_accum_len);
    return true;
}

/**
 * @brief Receive JSON message via UART (blocking with timeout)
 * @param buffer Buffer to store received JSON string
 * @param buffer_size Size of buffer
 * @param timeout_ms Timeout in milliseconds
 * @return Number of bytes received (excluding newline), or -1 on error/timeout
 */
int uart_receive_json(char *buffer, size_t buffer_size, uint32_t timeout_ms)
{
    if (!uart_initialized) {
        ESP_LOGE(TAG, "UART not initialized");
        return -1;
    }
    if (buffer == NULL || buffer_size == 0) {
        ESP_LOGE(TAG, "Invalid buffer");
        return -1;
    }

    /* An earlier block read (or an abort peek) may already have buffered a
     * whole frame — deliver that before touching the driver. */
    int got = accum_take_line(buffer, buffer_size);
    if (got >= 0) return got;

    TickType_t start   = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    do {
        if (accum_fill(20) < 0) {
            ESP_LOGE(TAG, "UART read error");
            return -1;
        }
        got = accum_take_line(buffer, buffer_size);
        if (got >= 0) return got;
    } while ((xTaskGetTickCount() - start) <= timeout);

    /* An idle poll expiring is the normal state of this link, not a fault: the
     * miner polls once a second and the mule only speaks when it has something
     * to say. Warning here emitted a line every 1.1 s forever, which floods the
     * console and pushes everything worth reading out of the log ring. Only a
     * STRANDED PARTIAL FRAME is worth a warning. */
    if (s_accum_len > 0)
        ESP_LOGW(TAG, "UART receive timeout with %u B of an incomplete frame buffered",
                 (unsigned)s_accum_len);
    else
        ESP_LOGD(TAG, "UART receive timeout (idle)");
    return -1;
}

/**
 * @brief Parse received JSON and extract field
 * @param json_str JSON string to parse
 * @param field Field name to extract
 * @param value_out Output buffer for field value (must be freed by caller if type is string)
 * @param value_type Expected value type (cJSON_String, cJSON_Number, etc.)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t uart_parse_json_field(const char *json_str, const char *field,
                                void *value_out, int value_type) {
    if (json_str == NULL || field == NULL || value_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *item = cJSON_GetObjectItem(root, field);
    if (item == NULL) {
        ESP_LOGE(TAG, "Field '%s' not found in JSON", field);
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    if (item->type != value_type) {
        ESP_LOGE(TAG, "Field '%s' has wrong type (expected %d, got %d)",
                 field, value_type, item->type);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    // Extract value based on type
    switch (value_type) {
        case cJSON_String:
            *(char **)value_out = strdup(item->valuestring);
            break;

        case cJSON_Number:
            *(int *)value_out = item->valueint;
            break;

        case cJSON_True:
        case cJSON_False:
            *(bool *)value_out = cJSON_IsTrue(item);
            break;

        default:
            ESP_LOGE(TAG, "Unsupported JSON type: %d", value_type);
            cJSON_Delete(root);
            return ESP_ERR_NOT_SUPPORTED;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief Deinitialize UART
 */
void uart_handler_deinit(void) {
    if (uart_initialized) {
        uart_driver_delete(UART_PORT_NUM);
        uart_initialized = false;
        ESP_LOGI(TAG, "UART deinitialized");
    }
}
