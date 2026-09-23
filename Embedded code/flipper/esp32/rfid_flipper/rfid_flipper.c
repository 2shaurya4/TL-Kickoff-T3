/*
 * rfid_flipper.c - reads the Flipper Zero "UID Stream" app's output.
 *
 * Line protocol (115200 8N1, CRLF), see ../../README.md:
 *
 *   RDY uid_stream 1        app started
 *   CARD <hex> <sak> <atqa> card arrived, e.g. CARD 04A23B1C 08 0004
 *   GONE                    card left the field
 *   PING                    heartbeat, every 2 s
 *   BYE                     app exiting
 *
 * Unknown lines are ignored but still count as link activity, so a future
 * protocol addition cannot break an old build.
 */

#include "rfid_flipper.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "rfid_flipper";

#define LINE_MAX            96
#define RX_BUFFER_SIZE      512
#define EVENT_QUEUE_LEN     4
#define RX_TASK_STACK       3072
#define RX_TASK_PRIO        5

/* ---- Module state -------------------------------------------------- */
static QueueHandle_t  s_events;         /* rfid_uid_t, one per tap        */
static TaskHandle_t   s_task;
static portMUX_TYPE   s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static rfid_uid_t     s_last_uid;
static bool           s_last_valid;
static bool           s_present;
static int64_t        s_last_rx_us;
static volatile bool  s_running;

/* ==================================================================== */
/* Parsing                                                              */
/* ==================================================================== */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Parse an even-length hex string into bytes. Returns byte count, or -1. */
static int parse_hex(const char *s, uint8_t *out, size_t out_cap)
{
    size_t len = strlen(s);
    size_t n = len / 2;

    if (len == 0 || (len % 2) != 0 || n > out_cap) {
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        const int hi = hex_nibble(s[i * 2]);
        const int lo = hex_nibble(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)n;
}

/* "CARD 04A23B1C 08 0004" -> uid. SAK and ATQA are optional. */
static bool parse_card_line(char *line, rfid_uid_t *uid)
{
    char *saveptr = NULL;
    char *tok = strtok_r(line, " \t", &saveptr);   /* "CARD" */
    int len;

    tok = strtok_r(NULL, " \t", &saveptr);
    if (tok == NULL) {
        return false;
    }

    memset(uid, 0, sizeof(*uid));
    len = parse_hex(tok, uid->bytes, RFID_UID_MAX_LEN);
    if (len != 4 && len != 7 && len != 10) {
        return false;
    }
    uid->len = (uint8_t)len;

    tok = strtok_r(NULL, " \t", &saveptr);
    if (tok != NULL) {
        uint8_t sak;
        if (parse_hex(tok, &sak, 1) == 1) {
            uid->sak = sak;
        }
    }

    tok = strtok_r(NULL, " \t", &saveptr);
    if (tok != NULL) {
        uint8_t atqa[2];
        if (parse_hex(tok, atqa, 2) == 2) {
            uid->atqa = (uint16_t)((atqa[0] << 8) | atqa[1]);
        }
    }
    return true;
}

static void queue_tap(const rfid_uid_t *uid)
{
    rfid_uid_t dropped;

    /* Keep the newest taps: if nobody has drained the queue, the stale ones
     * are the ones worth losing. */
    if (xQueueSend(s_events, uid, 0) != pdTRUE) {
        (void)xQueueReceive(s_events, &dropped, 0);
        (void)xQueueSend(s_events, uid, 0);
        ESP_LOGW(TAG, "event queue full - dropped the oldest tap");
    }
}

static void handle_line(char *line)
{
    rfid_uid_t uid;

    portENTER_CRITICAL(&s_state_mux);
    s_last_rx_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_state_mux);

    if (strncmp(line, "CARD", 4) == 0) {
        if (!parse_card_line(line, &uid)) {
            ESP_LOGW(TAG, "malformed CARD line");
            return;
        }
        portENTER_CRITICAL(&s_state_mux);
        s_last_uid = uid;
        s_last_valid = true;
        s_present = true;
        portEXIT_CRITICAL(&s_state_mux);
        queue_tap(&uid);

    } else if (strncmp(line, "GONE", 4) == 0 || strncmp(line, "BYE", 3) == 0) {
        portENTER_CRITICAL(&s_state_mux);
        s_present = false;
        portEXIT_CRITICAL(&s_state_mux);

    } else if (strncmp(line, "RDY", 3) == 0) {
        ESP_LOGI(TAG, "Flipper app connected");
        portENTER_CRITICAL(&s_state_mux);
        s_present = false;
        portEXIT_CRITICAL(&s_state_mux);
    }
    /* PING and anything unrecognised: the timestamp above is the point. */
}

/* ==================================================================== */
/* Receive task                                                         */
/* ==================================================================== */

static void rx_task(void *arg)
{
    char line[LINE_MAX];
    size_t pos = 0;
    uint8_t chunk[64];

    (void)arg;

    while (s_running) {
        const int n = uart_read_bytes(RFID_UART_PORT, chunk, sizeof(chunk),
                                      pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            const char c = (char)chunk[i];

            if (c == '\n' || c == '\r') {
                if (pos > 0) {
                    line[pos] = '\0';
                    handle_line(line);
                    pos = 0;
                }
            } else if (pos < LINE_MAX - 1) {
                line[pos++] = c;
            } else {
                /* Overlong line: drop it rather than splitting it in two. */
                pos = 0;
            }
        }
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

/* ==================================================================== */
/* Public API                                                           */
/* ==================================================================== */

esp_err_t rfid_init(void)
{
    ESP_RETURN_ON_FALSE(s_events == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "already initialised");

    const uart_config_t cfg = {
        .baud_rate = RFID_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(RFID_UART_PORT, RX_BUFFER_SIZE, 0, 0,
                                            NULL, 0),
                        TAG, "uart_driver_install failed");

    esp_err_t err = uart_param_config(RFID_UART_PORT, &cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(RFID_UART_PORT, RFID_UART_TX_GPIO, RFID_UART_RX_GPIO,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        uart_driver_delete(RFID_UART_PORT);
        ESP_LOGE(TAG, "uart config failed: %s", esp_err_to_name(err));
        return err;
    }

    s_events = xQueueCreate(EVENT_QUEUE_LEN, sizeof(rfid_uid_t));
    if (s_events == NULL) {
        uart_driver_delete(RFID_UART_PORT);
        return ESP_ERR_NO_MEM;
    }

    s_last_valid = false;
    s_present = false;
    s_last_rx_us = 0;
    s_running = true;

    if (xTaskCreate(rx_task, "flipper_rx", RX_TASK_STACK, NULL, RX_TASK_PRIO,
                    &s_task) != pdPASS) {
        s_running = false;
        vQueueDelete(s_events);
        s_events = NULL;
        uart_driver_delete(RFID_UART_PORT);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "listening for the Flipper on RX=%d (TX=%d) at %d baud",
             (int)RFID_UART_RX_GPIO, (int)RFID_UART_TX_GPIO, RFID_UART_BAUD);
    return ESP_OK;
}

esp_err_t rfid_read_new_uid(rfid_uid_t *uid, uint32_t timeout_ms)
{
    TickType_t ticks;

    ESP_RETURN_ON_FALSE(uid != NULL, ESP_ERR_INVALID_ARG, TAG, "null arg");
    ESP_RETURN_ON_FALSE(s_events != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "call rfid_init() first");

    ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return (xQueueReceive(s_events, uid, ticks) == pdTRUE) ? ESP_OK
                                                           : ESP_ERR_TIMEOUT;
}

bool rfid_card_present(void)
{
    bool present;

    portENTER_CRITICAL(&s_state_mux);
    present = s_present;
    portEXIT_CRITICAL(&s_state_mux);
    return present;
}

esp_err_t rfid_get_last_uid(rfid_uid_t *uid)
{
    esp_err_t err;

    ESP_RETURN_ON_FALSE(uid != NULL, ESP_ERR_INVALID_ARG, TAG, "null arg");

    portENTER_CRITICAL(&s_state_mux);
    if (s_last_valid) {
        *uid = s_last_uid;
        err = ESP_OK;
    } else {
        err = ESP_ERR_NOT_FOUND;
    }
    portEXIT_CRITICAL(&s_state_mux);
    return err;
}

bool rfid_link_ok(void)
{
    int64_t last;

    portENTER_CRITICAL(&s_state_mux);
    last = s_last_rx_us;
    portEXIT_CRITICAL(&s_state_mux);

    if (last == 0) {
        return false;               /* never heard from it */
    }
    return (esp_timer_get_time() - last) < ((int64_t)RFID_LINK_TIMEOUT_MS * 1000);
}

void rfid_flush(void)
{
    if (s_events != NULL) {
        xQueueReset(s_events);
    }
}

esp_err_t rfid_deinit(void)
{
    if (s_events == NULL) {
        return ESP_OK;
    }

    s_running = false;
    while (s_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));   /* let rx_task finish its read */
    }

    uart_driver_delete(RFID_UART_PORT);
    vQueueDelete(s_events);
    s_events = NULL;
    s_last_valid = false;
    s_present = false;
    return ESP_OK;
}

/* ---- UID utilities -------------------------------------------------- */

bool rfid_uid_equal(const rfid_uid_t *a, const rfid_uid_t *b)
{
    return a != NULL && b != NULL && a->len == b->len &&
           memcmp(a->bytes, b->bytes, a->len) == 0;
}

void rfid_uid_to_str(const rfid_uid_t *uid, char *buf, size_t buf_len)
{
    size_t pos = 0;

    if (buf == NULL || buf_len == 0) {
        return;
    }
    buf[0] = '\0';
    if (uid == NULL) {
        return;
    }

    for (uint8_t i = 0; i < uid->len && pos + 3 <= buf_len; i++) {
        pos += (size_t)snprintf(&buf[pos], buf_len - pos, i ? ":%02X" : "%02X",
                                uid->bytes[i]);
    }
}

rfid_card_type_t rfid_card_type(uint8_t sak)
{
    switch (sak & 0x7F) {
    case 0x09: return RFID_TYPE_MIFARE_MINI;
    case 0x08: return RFID_TYPE_MIFARE_1K;
    case 0x18: return RFID_TYPE_MIFARE_4K;
    case 0x00: return RFID_TYPE_MIFARE_UL;
    case 0x10:
    case 0x11: return RFID_TYPE_MIFARE_PLUS;
    case 0x20: return RFID_TYPE_ISO_14443_4;
    case 0x40: return RFID_TYPE_ISO_18092;
    default:   return RFID_TYPE_UNKNOWN;
    }
}

const char *rfid_card_type_str(rfid_card_type_t type)
{
    switch (type) {
    case RFID_TYPE_MIFARE_MINI:  return "MIFARE Mini";
    case RFID_TYPE_MIFARE_1K:    return "MIFARE Classic 1K";
    case RFID_TYPE_MIFARE_4K:    return "MIFARE Classic 4K";
    case RFID_TYPE_MIFARE_UL:    return "MIFARE Ultralight / NTAG";
    case RFID_TYPE_MIFARE_PLUS:  return "MIFARE Plus";
    case RFID_TYPE_ISO_14443_4:  return "ISO 14443-4 (smart card / phone)";
    case RFID_TYPE_ISO_18092:    return "ISO 18092 (NFC P2P)";
    default:                     return "unknown";
    }
}
