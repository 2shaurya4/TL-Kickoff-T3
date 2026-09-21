#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tof_hcsr04.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_ERROR_CHECK(tof_init());

    while (1) {
        tof_result_t m;
        esp_err_t err = tof_read(&m, 0);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "read failed: %s", esp_err_to_name(err));
        } else if (m.valid) {
            ESP_LOGI(TAG, "distance: %u mm (echo %lu us)", m.distance_mm,
                     (unsigned long)m.echo_us);
        } else {
            ESP_LOGW(TAG, "no reading: %s", tof_status_str(m.status));
        }

        /* Or, for something that acts on the value:
         *   uint16_t mm;
         *   if (tof_read_distance_mm_median(&mm, 5) == ESP_OK) { ... }
         */

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
