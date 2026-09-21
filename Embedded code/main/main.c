/*
 * main.c - Proximity lamp: light the NeoPixel strip whenever the HC-SR04
 *          sees something within about an inch of it.
 *
 * Caveat worth knowing before you tune this: an inch (25 mm) is right on the
 * edge of what an HC-SR04 can do. Its published dead zone is 20 mm, and below
 * that it stops returning a usable echo entirely, so the whole "detected"
 * window here is 20..25 mm wide. Raise PROX_TRIGGER_MM to ~50-100 mm and the
 * system gets dramatically more dependable. The logic below is written so
 * that changing those two numbers is the only edit needed.
 */

#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "neo_strip.h"
#include "sensor_test.h"
#include "strip_test.h"
#include "tof_hcsr04.h"

/* ---- Which program to run. Change APP_MODE and reflash. ------------ */

#define MODE_LAMP           0   /* the actual proximity lamp              */
#define MODE_SENSOR_TEST    1   /* strip white + raw HC-SR04 readings     */
#define MODE_STRIP_TEST     2   /* dim diagnostic patterns, sensor idle   */

#define APP_MODE            MODE_STRIP_TEST

/*
 * Brightness for the bring-up strip test, 0..255.
 *
 * Mind the current. A WS2812B pulls ~60 mA at full white, so all 36 pixels at
 * 255 would ask for about 2.2 A - several times what a USB port will give you
 * through the board's 5 V rail. In practice the rail sags, the strip turns a
 * dirty yellow, and the ESP32 may brown out and reboot. 48/255 lands near
 * 400 mA, which is bright enough to prove every pixel works and survivable on
 * USB. Only wind this up if the strip has its own 5 V supply.
 */
#define STRIP_TEST_BRIGHTNESS 48

static const char *TAG = "main";

/* ---- Tuning ------------------------------------------------------- */

/* Light up at or inside this distance. 25 mm ~= 1 inch. */
#define PROX_TRIGGER_MM     25

/* Turn back off only past this distance, so a target hovering on the
 * boundary does not strobe the strip. Must be > PROX_TRIGGER_MM. */
#define PROX_RELEASE_MM     40

/* Readings per decision. Ultrasonic samples are noisy and the driver spaces
 * bursts 60 ms apart, so 3 costs ~180 ms per decision. */
#define PROX_SAMPLES        3

/* Colour shown while something is close (pre-brightness-scaling). */
#define PROX_COLOR_R        255
#define PROX_COLOR_G        60
#define PROX_COLOR_B        0

_Static_assert(PROX_RELEASE_MM > PROX_TRIGGER_MM,
               "release distance must exceed trigger distance");

/* ---- Detection ----------------------------------------------------- */

typedef enum {
    VOTE_FAR = 0,
    VOTE_NEAR,
    VOTE_UNKNOWN,       /* sensor had nothing to say - do not count it */
} vote_t;

static vote_t classify(const tof_result_t *m, uint16_t threshold_mm)
{
    switch (m->status) {
    case TOF_STATUS_OK:
        return (m->distance_mm <= threshold_mm) ? VOTE_NEAR : VOTE_FAR;

    /* Inside the sensor's dead zone means something is there and it is
     * nearer than our threshold, which is the whole question. */
    case TOF_STATUS_TOO_CLOSE:
        return VOTE_NEAR;

    case TOF_STATUS_TOO_FAR:
        return VOTE_FAR;

    /* No echo can mean an empty room or a target pressed flat against the
     * transducers. It is genuinely ambiguous, so it abstains and the strip
     * holds whatever state it was already in. */
    case TOF_STATUS_NO_ECHO:
    case TOF_STATUS_TIMEOUT:
    default:
        return VOTE_UNKNOWN;
    }
}

/*
 * Take PROX_SAMPLES readings and majority-vote them. `lit` is the current
 * strip state, which selects the hysteresis threshold. Returns the state the
 * strip should be in; on a fully inconclusive burst that is just `lit` again.
 */
static bool sense_is_near(bool lit)
{
    const uint16_t threshold_mm = lit ? PROX_RELEASE_MM : PROX_TRIGGER_MM;
    uint8_t near_votes = 0;
    uint8_t far_votes = 0;

    for (uint8_t i = 0; i < PROX_SAMPLES; i++) {
        tof_result_t m;
        esp_err_t err = tof_read(&m, 0);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "sensor read failed: %s", esp_err_to_name(err));
            continue;
        }

        switch (classify(&m, threshold_mm)) {
        case VOTE_NEAR: near_votes++; break;
        case VOTE_FAR:  far_votes++;  break;
        default:                      break;
        }
    }

    if (near_votes == 0 && far_votes == 0) {
        return lit;
    }
    return near_votes > far_votes;
}

/* ---- Startup self-test --------------------------------------------- */

/* Blink once at boot so a dead strip is obvious before you start debugging
 * the sensor instead. */
static void strip_selftest(void)
{
    ESP_ERROR_CHECK(neo_set_all(PROX_COLOR_R, PROX_COLOR_G, PROX_COLOR_B));
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_ERROR_CHECK(neo_off());
}

/* ---- Entry point ---------------------------------------------------- */

void app_main(void)
{
    bool lit = false;

#if APP_MODE == MODE_STRIP_TEST
    strip_test_run();           /* never returns */

#elif APP_MODE == MODE_SENSOR_TEST
    /* Whole strip on, full white, so any dead or mis-coloured pixel is
     * obvious while the sensor readings scroll past. */
    ESP_ERROR_CHECK(neo_init());
    neo_set_brightness(STRIP_TEST_BRIGHTNESS);
    ESP_ERROR_CHECK(neo_set_all(255, 255, 255));

    printf("strip: %d pixels white at brightness %d/255 (~%d mA)\n",
           NEO_LED_COUNT, STRIP_TEST_BRIGHTNESS,
           (NEO_LED_COUNT * 60 * STRIP_TEST_BRIGHTNESS) / 255);

    sensor_test_run();          /* never returns */
#endif

    ESP_ERROR_CHECK(neo_init());
    ESP_ERROR_CHECK(tof_init());

    strip_selftest();

    ESP_LOGI(TAG, "watching: on at <=%d mm, off above %d mm",
             PROX_TRIGGER_MM, PROX_RELEASE_MM);

    while (1) {
        const bool near = sense_is_near(lit);

        if (near != lit) {
            esp_err_t err = near
                ? neo_set_all(PROX_COLOR_R, PROX_COLOR_G, PROX_COLOR_B)
                : neo_off();

            if (err != ESP_OK) {
                ESP_LOGE(TAG, "strip update failed: %s", esp_err_to_name(err));
            } else {
                lit = near;
                ESP_LOGI(TAG, "%s", lit ? "target close - strip on"
                                        : "target gone - strip off");
            }
        }

        /* tof_read() already paces itself at 60 ms per burst, so this is
         * only a courtesy yield. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
