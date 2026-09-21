/*
 * neo_strip.h - Thin wrapper over espressif/led_strip for a WS2812B
 *               ("Adafruit NeoPixel") strip on an ESP32-S3-DevKitC.
 *
 * Wiring note: WS2812B wants a data high of ~0.7 * VDD. Driven from 5 V that
 * is 3.5 V, and an ESP32-S3 GPIO only reaches 3.3 V. It usually works anyway,
 * but see the wiring guide for the two reliable fixes (level shifter, or drop
 * strip VCC to ~4.3 V with a series diode).
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
/* Wiring / strip geometry - matches KiCad/TL Kickoff T3.kicad_sch.    */
/* ------------------------------------------------------------------ */
#ifndef NEO_DIN_GPIO
#define NEO_DIN_GPIO        GPIO_NUM_7      /* the LEDOUT net */
#endif

/* How many pixels are actually on the strip you soldered. */
#ifndef NEO_LED_COUNT
#define NEO_LED_COUNT       36
#endif

/*
 * 0..255, applied to every colour before it reaches the strip. Keep this low
 * unless the strip has its own 5 V supply: a WS2812B pulls ~60 mA at full
 * white, so 8 of them can ask for ~0.5 A on their own.
 */
#ifndef NEO_DEFAULT_BRIGHTNESS
#define NEO_DEFAULT_BRIGHTNESS 40
#endif

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* Claim an RMT channel and drive the strip. Leaves every pixel off. */
esp_err_t neo_init(void);

/*
 * Set every pixel to one colour and push it out. Values are 0..255 at full
 * brightness and are scaled by the current brightness before transmission.
 */
esp_err_t neo_set_all(uint8_t r, uint8_t g, uint8_t b);

/* Blank the strip. */
esp_err_t neo_off(void);

/*
 * Global scale, 0..255, applied on the next write. Does not repaint on its
 * own - call neo_set_all() again to see the change.
 */
void neo_set_brightness(uint8_t brightness);

/* Release the RMT channel and reset the pin. */
esp_err_t neo_deinit(void);

#ifdef __cplusplus
}
#endif
