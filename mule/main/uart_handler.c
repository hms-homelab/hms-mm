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
static SemaphoreHandle_t uart_mutex = NULL;   /* one frame on the wire */

/* Held across a whole request/response conversation — see uart_link_lock().
 * Created lazily as well as at init, because a control endpoint can be served
 * before uart_handler_init() has run on a slow boot. */
static SemaphoreHandle_t link_mutex = NULL;
static bool uart_initialized = false;

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

    if (!uart_mutex) uart_mutex = xSemaphoreCreateMutex();
    if (!link_mutex) link_mutex = xSemaphoreCreateMutex();

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

    if (uart_mutex) xSemaphoreTake(uart_mutex, portMAX_DELAY);

    // Send JSON string
    int written = uart_write_bytes(UART_PORT_NUM, json_str, len);
    if (written < 0) {
        ESP_LOGE(TAG, "Failed to write UART bytes");
        if (uart_mutex) xSemaphoreGive(uart_mutex);
        return ESP_FAIL;
    }

    // Send newline delimiter
    written = uart_write_bytes(UART_PORT_NUM, "\n", 1);
    if (written < 0) {
        ESP_LOGE(TAG, "Failed to write newline");
        if (uart_mutex) xSemaphoreGive(uart_mutex);
        return ESP_FAIL;
    }

    // Wait for transmission to complete
    uart_wait_tx_done(UART_PORT_NUM, pdMS_TO_TICKS(1000));

    if (uart_mutex) xSemaphoreGive(uart_mutex);

    ESP_LOGD(TAG, "Sent JSON (%u bytes): %.100s...", (unsigned)len, json_str);
    return ESP_OK;
}

/**
 * @brief Receive JSON message via UART (blocking with timeout)
 * @param buffer Buffer to store received JSON string
 * @param buffer_size Size of buffer
 * @param timeout_ms Timeout in milliseconds
 * @return Number of bytes received (excluding newline), or -1 on error/timeout
 */
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
 * to be doing per byte on a C3 that is also serving HTTP.
 *
 * Kept deliberately identical to the miner's copy in miner/main/uart_handler.c
 * — the two must agree, so change them together.
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

int uart_receive_json(char *buffer, size_t buffer_size, uint32_t timeout_ms) {
    if (!uart_initialized) {
        ESP_LOGE(TAG, "UART not initialized");
        return -1;
    }

    if (buffer == NULL || buffer_size == 0) {
        ESP_LOGE(TAG, "Invalid buffer");
        return -1;
    }

    /* An earlier block read may already have buffered a whole frame — deliver
     * that before touching the driver. */
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

    ESP_LOGW(TAG, "UART receive timeout (%u B buffered, no frame terminator)",
             (unsigned)s_accum_len);
    return -1;
}

bool uart_link_lock(uint32_t timeout_ms)
{
    /* Lazily created: a control endpoint can be served before
     * uart_handler_init() has run, and silently not serialising would be worse
     * than a slightly late allocation. */
    if (!link_mutex) {
        link_mutex = xSemaphoreCreateMutex();
        if (!link_mutex) return false;
    }
    TickType_t wait = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(link_mutex, wait) == pdTRUE;
}

void uart_link_unlock(void)
{
    if (link_mutex) xSemaphoreGive(link_mutex);
}

/* Drop anything half-received. Called when the mule abandons a request (client
 * gone) so the NEXT request does not open with the tail of the previous
 * response still sitting in the assembler. */
void uart_rx_flush(void)
{
    s_accum_len = 0;
    uart_flush_input(UART_PORT_NUM);
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
