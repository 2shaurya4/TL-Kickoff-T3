/*
 * strip_test.c - Patterns for working out why a WS2812B strip is dark.
 *
 * Everything here runs at low brightness on purpose. A dark strip has three
 * common causes and current is not one of them, so there is no reason to
 * pull an amp while diagnosing:
 *
 *   nothing ever lights      -> DIN is on the wrong pin or the wrong end of
 *                               the strip, or grounds are not common
 *   pixel 0 lights, rest do not
 *                            -> the 3.3 V data high is too low for a strip
 *                               running on 5 V; the first LED latches it by
 *                               luck and then re-times garbage downstream
 *   lights, but wrong colours-> colour component order is not GRB
 */

#include "strip_test.h"

#include <inttypes.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "neo_strip.h"

/* Dim enough that 36 lit pixels stay under ~100 mA. */
#define DIAG_BRIGHTNESS     24

static void all(uint8_t r, uint8_t g, uint8_t b)
{
    for (uint32_t i = 0; i < NEO_LED_COUNT; i++) {
        neo_set_pixel(i, r, g, b);
    }
    neo_show();
}

/* Step 1: one pixel, the nearest one to the controller. If data reaches the
 * strip at all, this lights. ~20 mA, so nothing about power can mask it. */
static void step_first_pixel_only(void)
{
    printf("[1/5] pixel 0 only, RED, for 3 s\n");
    printf("      lit  -> data is getting to the strip\n");
    printf("      dark -> DIN is not on GPIO%d, or you are feeding the\n",
           (int)NEO_DIN_GPIO);
    printf("              output end of the strip, or GND is not shared\n");

    all(0, 0, 0);
    neo_set_pixel(0, 255, 0, 0);
    neo_show();
    vTaskDelay(pdMS_TO_TICKS(3000));
}

/* Step 2: walk one pixel down the strip. Where it stops is where the signal
 * dies, which is the single most useful number for a level-shift problem. */
static void step_chase(void)
{
    printf("[2/5] single RED pixel walking 0 -> %d\n", NEO_LED_COUNT - 1);
    printf("      watch for the position where it stops or goes wrong\n");

    for (uint32_t i = 0; i < NEO_LED_COUNT; i++) {
        all(0, 0, 0);
        neo_set_pixel(i, 255, 0, 0);
        neo_show();
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

/* Steps 3-5: solid colours. If these come out swapped the pixel format is
 * wrong, not the wiring - an easy and very common misdiagnosis. */
static void step_colours(void)
{
    printf("[3/5] whole strip RED\n");
    all(255, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(1500));

    printf("[4/5] whole strip GREEN\n");
    all(0, 255, 0);
    vTaskDelay(pdMS_TO_TICKS(1500));

    printf("[5/5] whole strip BLUE\n");
    all(0, 0, 255);
    vTaskDelay(pdMS_TO_TICKS(1500));

    printf("      if these three came out in a different order than\n");
    printf("      red/green/blue, the strip is not GRB - tell me the order\n");
    printf("      you actually saw and I will change the pixel format\n");
}

void strip_test_run(void)
{
    printf("\n");
    printf("=== WS2812B strip diagnostic ===\n");
    printf("  DIN -> GPIO%d, %d pixels, brightness %d/255\n",
           (int)NEO_DIN_GPIO, NEO_LED_COUNT, DIAG_BRIGHTNESS);
    printf("  deliberately dim: current draw is not a variable here\n");
    printf("\n");

    ESP_ERROR_CHECK(neo_init());
    neo_set_brightness(DIAG_BRIGHTNESS);

    uint32_t pass = 0;

    while (1) {
        printf("--- pass %" PRIu32 " ---\n", ++pass);
        step_first_pixel_only();
        step_chase();
        step_colours();
        all(0, 0, 0);
        printf("      all off for 2 s, then repeating\n\n");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
