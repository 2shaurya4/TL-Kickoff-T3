/*
 * tof_hcsr04.c - helper functions to pull distance values off an HC-SR04.
 *
 * How it works: drive TRIG high for 10 us, then time the ECHO pulse. Both
 * edges of ECHO raise a GPIO interrupt that timestamps with esp_timer, and
 * the falling edge unblocks the waiting task, so no busy-waiting.
 *
 *   distance_mm = echo_us * speed_of_sound_mm_per_us / 2
 */

#include "tof_hcsr04.h"

#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "hcsr04";

#define TRIG_PULSE_US       10
#define TRIG_SETTLE_US      4

/* Speed of sound in mm/us: (331.3 + 0.606 * T_celsius) m/s / 1000. */
#define SPEED_MM_PER_US(t)  ((331.3f + 0.606f * (t)) / 1000.0f)

/* ---- Module state ------------------------------------------------- */
static SemaphoreHandle_t s_lock;
static TaskHandle_t      s_waiter;
static volatile int64_t  s_rise_us;
static volatile int64_t  s_fall_us;
static volatile bool     s_got_rise;
static int64_t           s_last_trigger_us;
static float             s_speed_mm_per_us = SPEED_MM_PER_US(20.0f);
static bool              s_ready;

/* ---- ECHO edge interrupt ------------------------------------------ */

static void IRAM_ATTR echo_isr(void *arg)
{
    const int64_t now = esp_timer_get_time();
    BaseType_t higher_prio_woken = pdFALSE;

    (void)arg;

    if (gpio_get_level(TOF_ECHO_GPIO)) {
        s_rise_us = now;
        s_got_rise = true;
    } else if (s_got_rise) {
        s_fall_us = now;
        if (s_waiter != NULL) {
            vTaskNotifyGiveFromISR(s_waiter, &higher_prio_woken);
        }
    }

    portYIELD_FROM_ISR(higher_prio_woken);
}

/* ---- Helpers ------------------------------------------------------ */

/* The datasheet wants >= 60 ms between bursts so the previous one has died
 * away; sleep off whatever is left of that window. */
static void wait_out_min_period(void)
{
    if (s_last_trigger_us == 0) {
        return;
    }

    const int64_t elapsed_ms = (esp_timer_get_time() - s_last_trigger_us) / 1000;
    if (elapsed_ms < TOF_MIN_PERIOD_MS) {
        vTaskDelay(pdMS_TO_TICKS(TOF_MIN_PERIOD_MS - (uint32_t)elapsed_ms));
    }
}

static void send_trigger_pulse(void)
{
    gpio_set_level(TOF_TRIG_GPIO, 0);
    esp_rom_delay_us(TRIG_SETTLE_US);
    gpio_set_level(TOF_TRIG_GPIO, 1);
    esp_rom_delay_us(TRIG_PULSE_US);
    gpio_set_level(TOF_TRIG_GPIO, 0);
}

static int cmp_u16(const void *a, const void *b)
{
    const uint16_t x = *(const uint16_t *)a;
    const uint16_t y = *(const uint16_t *)b;
    return (x > y) - (x < y);
}

/* ---- Public API ---------------------------------------------------- */

esp_err_t tof_init(void)
{
    ESP_RETURN_ON_FALSE(!s_ready, ESP_ERR_INVALID_STATE, TAG,
                        "already initialised");

    const gpio_config_t trig_cfg = {
        .pin_bit_mask = 1ULL << TOF_TRIG_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const gpio_config_t echo_cfg = {
        .pin_bit_mask = 1ULL << TOF_ECHO_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&trig_cfg), TAG, "TRIG config failed");
    ESP_RETURN_ON_ERROR(gpio_config(&echo_cfg), TAG, "ECHO config failed");
    gpio_set_level(TOF_TRIG_GPIO, 0);

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "no memory");

    /* The app may already own the ISR service; that is not an error. */
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        ESP_LOGE(TAG, "gpio_install_isr_service failed");
        return err;
    }

    err = gpio_isr_handler_add(TOF_ECHO_GPIO, echo_isr, NULL);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        ESP_LOGE(TAG, "gpio_isr_handler_add failed");
        return err;
    }

    s_last_trigger_us = 0;
    s_ready = true;
    ESP_LOGI(TAG, "HC-SR04 ready on TRIG=%d ECHO=%d", (int)TOF_TRIG_GPIO,
             (int)TOF_ECHO_GPIO);
    return ESP_OK;
}

esp_err_t tof_read(tof_result_t *out, uint32_t timeout_ms)
{
    uint32_t notified;
    uint32_t echo_us;
    uint32_t distance_mm;

    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "null arg");
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG,
                        "call tof_init() first");

    if (timeout_ms == 0) {
        timeout_ms = TOF_DEFAULT_TIMEOUT_MS;
    }

    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_lock, portMAX_DELAY);

    wait_out_min_period();

    /* Arm: drop any stale notification, then let the ISR find us. */
    ulTaskNotifyTake(pdTRUE, 0);
    s_got_rise = false;
    s_waiter = xTaskGetCurrentTaskHandle();

    send_trigger_pulse();
    s_last_trigger_us = esp_timer_get_time();

    notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms));
    s_waiter = NULL;

    if (notified == 0) {
        /* Nothing came back inside the window. */
        out->status = s_got_rise ? TOF_STATUS_TIMEOUT : TOF_STATUS_NO_ECHO;
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    echo_us = (uint32_t)(s_fall_us - s_rise_us);
    xSemaphoreGive(s_lock);

    out->echo_us = echo_us;
    distance_mm = (uint32_t)(((float)echo_us * s_speed_mm_per_us) / 2.0f);

    if (distance_mm > TOF_MAX_DISTANCE_MM) {
        out->status = TOF_STATUS_TOO_FAR;
        return ESP_OK;
    }
    if (distance_mm < TOF_MIN_DISTANCE_MM) {
        out->status = TOF_STATUS_TOO_CLOSE;
        return ESP_OK;
    }

    out->distance_mm = (uint16_t)distance_mm;
    out->status = TOF_STATUS_OK;
    out->valid = true;
    return ESP_OK;
}

esp_err_t tof_read_distance_mm(uint16_t *distance_mm, uint32_t timeout_ms)
{
    tof_result_t result;

    ESP_RETURN_ON_FALSE(distance_mm != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "null arg");
    ESP_RETURN_ON_ERROR(tof_read(&result, timeout_ms), TAG, "read failed");

    if (!result.valid) {
        ESP_LOGW(TAG, "measurement rejected: %s", tof_status_str(result.status));
        return ESP_ERR_INVALID_RESPONSE;
    }

    *distance_mm = result.distance_mm;
    return ESP_OK;
}

esp_err_t tof_read_distance_mm_median(uint16_t *distance_mm, uint8_t samples)
{
    uint16_t good[15];
    uint8_t n = 0;

    ESP_RETURN_ON_FALSE(distance_mm != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "null arg");
    ESP_RETURN_ON_FALSE(samples >= 1 && samples <= sizeof(good) / sizeof(good[0]),
                        ESP_ERR_INVALID_ARG, TAG, "samples must be 1..15");

    for (uint8_t i = 0; i < samples; i++) {
        tof_result_t result;
        ESP_RETURN_ON_ERROR(tof_read(&result, 0), TAG, "read failed");
        if (result.valid) {
            good[n++] = result.distance_mm;
        }
    }

    if (n == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    qsort(good, n, sizeof(good[0]), cmp_u16);
    *distance_mm = good[n / 2];
    return ESP_OK;
}

void tof_set_temperature_c(float temperature_c)
{
    s_speed_mm_per_us = SPEED_MM_PER_US(temperature_c);
}

const char *tof_status_str(tof_status_t status)
{
    switch (status) {
    case TOF_STATUS_OK:        return "valid";
    case TOF_STATUS_NO_ECHO:   return "no echo (check 5 V supply and wiring)";
    case TOF_STATUS_TIMEOUT:   return "echo never returned";
    case TOF_STATUS_TOO_FAR:   return "target beyond sensor range";
    case TOF_STATUS_TOO_CLOSE: return "target inside dead zone";
    default:                   return "unknown error";
    }
}

esp_err_t tof_deinit(void)
{
    if (!s_ready) {
        return ESP_OK;
    }

    s_ready = false;
    gpio_isr_handler_remove(TOF_ECHO_GPIO);
    gpio_set_level(TOF_TRIG_GPIO, 0);
    gpio_reset_pin(TOF_TRIG_GPIO);
    gpio_reset_pin(TOF_ECHO_GPIO);

    if (s_lock != NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
    s_waiter = NULL;
    s_last_trigger_us = 0;
    return ESP_OK;
}
