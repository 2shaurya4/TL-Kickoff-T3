/*
 * sensor_test.c - Bring-up test for the HC-SR04. Prints one reading per
 *                 line so you can watch the numbers move as you wave your
 *                 hand at the sensor.
 *
 * What good output looks like: mostly "ok" lines whose mm value tracks your
 * hand, with the occasional dropped reading. What a wiring fault looks like
 * is spelled out in the legend printed at startup.
 */

#include "sensor_test.h"

#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tof_hcsr04.h"

/* Slow enough to read by eye; the driver's 60 ms floor allows much faster. */
#define TEST_PERIOD_MS      250

/* Column of '#' proportional to distance, as a crude visual scale. */
#define BAR_FULL_SCALE_MM   500
#define BAR_WIDTH           40

static void print_bar(uint16_t distance_mm)
{
    char bar[BAR_WIDTH + 1];
    uint32_t filled = ((uint32_t)distance_mm * BAR_WIDTH) / BAR_FULL_SCALE_MM;

    if (filled > BAR_WIDTH) {
        filled = BAR_WIDTH;
    }

    for (uint32_t i = 0; i < BAR_WIDTH; i++) {
        bar[i] = (i < filled) ? '#' : '.';
    }
    bar[BAR_WIDTH] = '\0';

    printf("  [%s]\n", bar);
}

static void print_legend(void)
{
    printf("\n");
    printf("=== HC-SR04 bring-up test ===\n");
    printf("  TRIG -> GPIO%d   ECHO -> GPIO%d\n",
           (int)TOF_TRIG_GPIO, (int)TOF_ECHO_GPIO);
    printf("  valid range %d..%d mm, one reading every %d ms\n",
           TOF_MIN_DISTANCE_MM, TOF_MAX_DISTANCE_MM, TEST_PERIOD_MS);
    printf("\n");
}

/*
 * Is anything actually out there? Tug the ECHO line up, then down, with the
 * internal pulls and see whether it follows. A powered HC-SR04 holds its ECHO
 * output low when idle and will win against the ~45k internal pull-up; a pin
 * with nothing on it just follows whichever pull is enabled.
 */
static void probe_echo_line(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << TOF_ECHO_GPIO,
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };

    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&cfg);
    vTaskDelay(pdMS_TO_TICKS(20));
    const int up = gpio_get_level(TOF_ECHO_GPIO);

    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&cfg);
    vTaskDelay(pdMS_TO_TICKS(20));
    const int down = gpio_get_level(TOF_ECHO_GPIO);

    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&cfg);

    printf("  ECHO probe on GPIO%d: pull-up reads %d, pull-down reads %d\n",
           (int)TOF_ECHO_GPIO, up, down);

    if (up == 1 && down == 0) {
        printf("    -> FLOATING. Nothing is driving this pin at all.\n");
        printf("       Either the sensor has no power (is 5VIN really\n");
        printf("       putting out 5 V?), or ECHO is not on this pin.\n");
    } else if (up == 0 && down == 0) {
        printf("    -> held LOW. Something is actively pulling this pin\n");
        printf("       down, which is what a powered idle HC-SR04 does.\n");
        printf("       If so the sensor has power, and TRIG is the thing\n");
        printf("       to suspect. A 2k divider to GND also reads this way.\n");
    } else {
        printf("    -> held HIGH. ECHO is stuck asserted - look for a\n");
        printf("       short to 5 V, or a swapped TRIG/ECHO pair.\n");
    }
    printf("\n");
}

void sensor_test_run(void)
{
    uint32_t total = 0;
    uint32_t good = 0;

    print_legend();
    probe_echo_line();

    ESP_ERROR_CHECK(tof_init());

    while (1) {
        tof_result_t m;
        esp_err_t err = tof_read(&m, 0);

        total++;

        if (err != ESP_OK) {
            printf("#%-5" PRIu32 "  DRIVER ERROR: %s\n", total,
                   esp_err_to_name(err));
        } else if (m.valid) {
            good++;
            /* 254 tenths of a mm per inch; keeps this in integer maths. */
            const uint32_t tenth_inches = ((uint32_t)m.distance_mm * 100U) / 254U;

            printf("#%-5" PRIu32 "  %4u mm  (%2" PRIu32 ".%" PRIu32 " in)"
                   "  echo %5" PRIu32 " us  ok   [%" PRIu32 "/%" PRIu32 " good]\n",
                   total, (unsigned)m.distance_mm,
                   tenth_inches / 10U, tenth_inches % 10U,
                   m.echo_us, good, total);
            print_bar(m.distance_mm);
        } else {
            printf("#%-5" PRIu32 "     --      echo %5" PRIu32 " us  %s"
                   "   [%" PRIu32 "/%" PRIu32 " good]\n",
                   total, m.echo_us, tof_status_str(m.status), good, total);
        }

        vTaskDelay(pdMS_TO_TICKS(TEST_PERIOD_MS));
    }
}
