/*
 * anim.c - The light show.
 *
 * Four states, driven by one flag from the sensor task:
 *
 *   IDLE   dark, waiting
 *   BURST  a white comet races the length of the strip, laying down rainbow
 *          behind it - the "something just arrived" moment
 *   ACTIVE cycling rainbow with random white sparkles, held while the target
 *          is still there
 *   FADE   the rainbow dims to black over ~400 ms, so leaving does not just
 *          cut the strip off
 */

#include "anim.h"

#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "neo_strip.h"

static const char *TAG = "anim";

/* ---- Tuning -------------------------------------------------------- */

#define ANIM_FPS            50
#define ANIM_FRAME_MS       (1000 / ANIM_FPS)

#define BURST_FRAMES        16          /* ~320 ms for the comet to land   */
#define FADE_FRAMES         20          /* ~400 ms to go dark              */
#define HUE_STEP_PER_FRAME  6           /* rainbow scroll speed, degrees   */
#define HUE_SPREAD_DEG      300         /* colour spread across the strip  */
#define SPARKLES_PER_FRAME  2

/*
 * Master brightness for the show, 0..255.
 *
 * Current: a WS2812B pulls ~60 mA per pixel at full white. A rainbow lights
 * roughly half of that on average, so 36 pixels here land near
 * 36 * 60 * (128/255) * 0.5 = ~540 mA. That is already more than a USB port
 * wants to give through the board's 5 V pin. It is set where it is because
 * the ask was for something bright; if the strip browns out or the ESP32
 * reboots when it triggers, either drop this or give the strip its own 5 V
 * supply (see WIRING.md).
 */
#define ANIM_BRIGHTNESS     128

/* The comet is deliberately full white - it is the loudest part of the
 * effect and only ever lights one pixel at a time. */
#define COMET_LEVEL         255

/* ---- State --------------------------------------------------------- */

typedef enum {
    ANIM_IDLE = 0,
    ANIM_BURST,
    ANIM_ACTIVE,
    ANIM_FADE,
} anim_state_t;

static volatile bool s_triggered;       /* written by the sensor task */
static TaskHandle_t  s_task;

/* ---- Colour -------------------------------------------------------- */

/*
 * Fully saturated HSV to RGB, integer only. `h` is degrees 0..359, `v` is
 * 0..255. Saturation is pinned at maximum because washed-out pastels are not
 * what is wanted here.
 */
static void hsv_to_rgb(uint16_t h, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (h >= 360) {
        h %= 360;
    }

    const uint8_t region = (uint8_t)(h / 60);
    const uint8_t rem = (uint8_t)(((h % 60) * 255) / 60);
    const uint8_t q = (uint8_t)(((uint16_t)v * (255 - rem)) / 255);
    const uint8_t t = (uint8_t)(((uint16_t)v * rem) / 255);

    switch (region) {
    case 0:  *r = v; *g = t; *b = 0; break;
    case 1:  *r = q; *g = v; *b = 0; break;
    case 2:  *r = 0; *g = v; *b = t; break;
    case 3:  *r = 0; *g = q; *b = v; break;
    case 4:  *r = t; *g = 0; *b = v; break;
    default: *r = v; *g = 0; *b = q; break;
    }
}

/* Hue for pixel `i` at scroll position `phase`. */
static uint16_t pixel_hue(uint32_t i, uint16_t phase)
{
    return (uint16_t)(((i * HUE_SPREAD_DEG) / NEO_LED_COUNT + phase) % 360);
}

/* ---- Frames -------------------------------------------------------- */

static void draw_rainbow(uint16_t phase, uint8_t level)
{
    for (uint32_t i = 0; i < NEO_LED_COUNT; i++) {
        uint8_t r, g, b;
        hsv_to_rgb(pixel_hue(i, phase), level, &r, &g, &b);
        neo_set_pixel(i, r, g, b);
    }
}

/* A few pixels punched to white each frame. Cheap, and it is what makes the
 * rainbow read as energetic rather than as a slow gradient. */
static void add_sparkles(void)
{
    for (int n = 0; n < SPARKLES_PER_FRAME; n++) {
        const uint32_t i = esp_random() % NEO_LED_COUNT;
        neo_set_pixel(i, 255, 255, 255);
    }
}

/* White head racing away from the controller, rainbow filling in behind it,
 * darkness ahead of it. */
static void draw_burst(uint32_t frame, uint16_t phase)
{
    const uint32_t head = (frame * NEO_LED_COUNT) / BURST_FRAMES;

    for (uint32_t i = 0; i < NEO_LED_COUNT; i++) {
        if (i < head) {
            uint8_t r, g, b;
            hsv_to_rgb(pixel_hue(i, phase), 255, &r, &g, &b);
            neo_set_pixel(i, r, g, b);
        } else if (i == head) {
            neo_set_pixel(i, COMET_LEVEL, COMET_LEVEL, COMET_LEVEL);
        } else {
            neo_set_pixel(i, 0, 0, 0);
        }
    }
}

/* ---- Render task ---------------------------------------------------- */

static void anim_task(void *arg)
{
    anim_state_t state = ANIM_IDLE;
    uint32_t     frame = 0;
    uint16_t     phase = 0;
    TickType_t   last_wake = xTaskGetTickCount();

    (void)arg;

    neo_set_brightness(ANIM_BRIGHTNESS);
    neo_off();

    while (1) {
        const bool triggered = s_triggered;

        /* Transitions that can happen from any state. */
        if (triggered && (state == ANIM_IDLE || state == ANIM_FADE)) {
            state = ANIM_BURST;
            frame = 0;
        } else if (!triggered && (state == ANIM_BURST || state == ANIM_ACTIVE)) {
            state = ANIM_FADE;
            frame = 0;
        }

        switch (state) {
        case ANIM_IDLE:
            /* Nothing to push - the strip already holds black. */
            break;

        case ANIM_BURST:
            draw_burst(frame, phase);
            neo_show();
            if (++frame >= BURST_FRAMES) {
                state = ANIM_ACTIVE;
                frame = 0;
            }
            break;

        case ANIM_ACTIVE:
            draw_rainbow(phase, 255);
            add_sparkles();
            neo_show();
            break;

        case ANIM_FADE: {
            /* Linear ramp to black. */
            const uint8_t level =
                (uint8_t)((255U * (FADE_FRAMES - frame)) / FADE_FRAMES);
            draw_rainbow(phase, level);
            neo_show();
            if (++frame >= FADE_FRAMES) {
                neo_off();
                state = ANIM_IDLE;
                frame = 0;
            }
            break;
        }
        }

        phase = (uint16_t)((phase + HUE_STEP_PER_FRAME) % 360);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(ANIM_FRAME_MS));
    }
}

/* ---- Public API ------------------------------------------------------ */

esp_err_t anim_start(void)
{
    ESP_RETURN_ON_FALSE(s_task == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "already started");

    const BaseType_t ok = xTaskCreate(anim_task, "anim", 3072, NULL, 4, &s_task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "could not create render task");

    ESP_LOGI(TAG, "render task up at %d fps, brightness %d/255",
             ANIM_FPS, ANIM_BRIGHTNESS);
    return ESP_OK;
}

void anim_set_triggered(bool triggered)
{
    s_triggered = triggered;
}
