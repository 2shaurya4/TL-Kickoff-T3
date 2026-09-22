/*
 * tof_hcsr04.h - Minimal ESP-IDF helper driver for an HC-SR04 ultrasonic
 *                distance sensor on an ESP32-S3-DevKitC.
 *
 * Wiring note: the HC-SR04 needs 5 V on VCC and its ECHO pin idles/drives at
 * 5 V, which is NOT safe for the ESP32-S3 GPIOs. Put a level shifter or a
 * divider (e.g. 1k in series + 2k to GND) between ECHO and the MCU. TRIG can
 * be driven directly from a 3.3 V GPIO.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Wiring - matches KiCad/TL Kickoff T3.kicad_sch.                     */
/* ------------------------------------------------------------------ */
#ifndef TOF_TRIG_GPIO
#define TOF_TRIG_GPIO       GPIO_NUM_6      /* output, straight to sensor TRIG */
#endif

#ifndef TOF_ECHO_GPIO
#define TOF_ECHO_GPIO       GPIO_NUM_5      /* input, via the R1/R2 divider */
#endif

/* Datasheet range of the HC-SR04: roughly 2 cm .. 4 m. */
#ifndef TOF_MIN_DISTANCE_MM
#define TOF_MIN_DISTANCE_MM 20
#endif

#ifndef TOF_MAX_DISTANCE_MM
#define TOF_MAX_DISTANCE_MM 4000
#endif

/* A no-echo burst lasts ~38 ms; 60 ms covers it with margin. */
#ifndef TOF_DEFAULT_TIMEOUT_MS
#define TOF_DEFAULT_TIMEOUT_MS 60
#endif

/* The datasheet asks for >=60 ms between triggers to avoid hearing the
 * previous burst's echoes. tof_read() enforces this for you. */
#ifndef TOF_MIN_PERIOD_MS
#define TOF_MIN_PERIOD_MS   60
#endif

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    TOF_STATUS_OK = 0,          /* distance_mm is good                      */
    TOF_STATUS_NO_ECHO,         /* echo never went high - check wiring/5 V  */
    TOF_STATUS_TIMEOUT,         /* echo started but never came back         */
    TOF_STATUS_TOO_FAR,         /* beyond TOF_MAX_DISTANCE_MM               */
    TOF_STATUS_TOO_CLOSE,       /* inside the sensor's dead zone            */
} tof_status_t;

/* One measurement. Only trust distance_mm when `valid` is true. */
typedef struct {
    uint16_t     distance_mm;   /* one-way distance to the target, mm       */
    uint32_t     echo_us;       /* raw round-trip echo pulse width, us      */
    tof_status_t status;
    bool         valid;         /* shorthand for status == TOF_STATUS_OK    */
} tof_result_t;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/*
 * Configure TRIG as an output and ECHO as an interrupt-driven input.
 * Installs the shared GPIO ISR service if the application has not already.
 */
esp_err_t tof_init(void);

/*
 * Fire one burst and wait up to timeout_ms for the echo (pass 0 for
 * TOF_DEFAULT_TIMEOUT_MS). Returns ESP_OK whenever the attempt completed -
 * check `out->valid` / `out->status` to see whether a target was found.
 * Other esp_err_t values mean the driver itself failed (not initialised,
 * bad argument, ...). Safe to call from several tasks; calls are serialised
 * and spaced at least TOF_MIN_PERIOD_MS apart.
 */
esp_err_t tof_read(tof_result_t *out, uint32_t timeout_ms);

/* Convenience wrapper: distance only. Returns ESP_ERR_INVALID_RESPONSE when
 * the measurement was not valid. */
esp_err_t tof_read_distance_mm(uint16_t *distance_mm, uint32_t timeout_ms);

/*
 * Take `samples` readings (1..15) and return the median of the valid ones.
 * Ultrasonic readings are noisy and occasionally miss entirely, so this is
 * the one to use for anything that acts on the value.
 * Returns ESP_ERR_INVALID_RESPONSE if no sample came back valid.
 */
esp_err_t tof_read_distance_mm_median(uint16_t *distance_mm, uint8_t samples);

/*
 * Speed of sound depends on air temperature (~0.17 % per degC). Default is
 * 20 degC; set this if you have a thermometer and want the extra accuracy.
 */
void tof_set_temperature_c(float temperature_c);

/* Human-readable text for tof_result_t.status. */
const char *tof_status_str(tof_status_t status);

/* Release the ECHO interrupt handler and reset the pins. */
esp_err_t tof_deinit(void);

#ifdef __cplusplus
}
#endif
