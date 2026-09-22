/*
 * neo_strip.c - WS2812B strip helper built on the espressif/led_strip
 *               component (RMT backend).
 */

#include "neo_strip.h"

#include "esp_check.h"
#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "neo";

/* 10 MHz -> 0.1 us per RMT tick, the resolution the WS2812 encoder wants. */
#define NEO_RMT_RESOLUTION_HZ (10 * 1000 * 1000)

static led_strip_handle_t s_strip;
static uint8_t            s_brightness = NEO_DEFAULT_BRIGHTNESS;

static inline uint8_t scale(uint8_t value)
{
    return (uint8_t)(((uint16_t)value * s_brightness) / 255U);
}

esp_err_t neo_init(void)
{
    ESP_RETURN_ON_FALSE(s_strip == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "already initialised");

    const led_strip_config_t strip_cfg = {
        .strip_gpio_num = NEO_DIN_GPIO,
        .max_leds = NEO_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = NEO_RMT_RESOLUTION_HZ,
        .mem_block_symbols = 0,     /* let the driver pick */
        .flags.with_dma = false,
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip),
                        TAG, "led_strip_new_rmt_device failed");

    ESP_RETURN_ON_ERROR(led_strip_clear(s_strip), TAG, "clear failed");

    ESP_LOGI(TAG, "%d pixels ready on DIN=%d (brightness %u/255)",
             NEO_LED_COUNT, (int)NEO_DIN_GPIO, (unsigned)s_brightness);
    return ESP_OK;
}

esp_err_t neo_set_all(uint8_t r, uint8_t g, uint8_t b)
{
    ESP_RETURN_ON_FALSE(s_strip != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "call neo_init() first");

    const uint8_t sr = scale(r);
    const uint8_t sg = scale(g);
    const uint8_t sb = scale(b);

    for (uint32_t i = 0; i < NEO_LED_COUNT; i++) {
        ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_strip, i, sr, sg, sb), TAG,
                            "set_pixel %lu failed", (unsigned long)i);
    }

    return led_strip_refresh(s_strip);
}

esp_err_t neo_set_pixel(uint32_t index, uint8_t r, uint8_t g, uint8_t b)
{
    ESP_RETURN_ON_FALSE(s_strip != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "call neo_init() first");
    ESP_RETURN_ON_FALSE(index < NEO_LED_COUNT, ESP_ERR_INVALID_ARG, TAG,
                        "pixel %lu is past the end of the strip",
                        (unsigned long)index);

    return led_strip_set_pixel(s_strip, index, scale(r), scale(g), scale(b));
}

esp_err_t neo_show(void)
{
    ESP_RETURN_ON_FALSE(s_strip != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "call neo_init() first");
    return led_strip_refresh(s_strip);
}

esp_err_t neo_off(void)
{
    ESP_RETURN_ON_FALSE(s_strip != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "call neo_init() first");
    return led_strip_clear(s_strip);
}

void neo_set_brightness(uint8_t brightness)
{
    s_brightness = brightness;
}

esp_err_t neo_deinit(void)
{
    if (s_strip == NULL) {
        return ESP_OK;
    }

    led_strip_clear(s_strip);
    esp_err_t err = led_strip_del(s_strip);
    s_strip = NULL;
    return err;
}
