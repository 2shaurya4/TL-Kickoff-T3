/*
 * strip_test.h - Diagnostic patterns for bringing up the WS2812B strip.
 *
 * Set APP_MODE to APP_MODE_STRIP_TEST in main.c to run this.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Walk through a sequence of deliberately low-current patterns designed to
 * separate the ways a NeoPixel strip fails: no data at all, data that dies
 * after the first pixel, and a wrong colour order. Announces each step on
 * the console so you can match what you see to what was sent. Never returns.
 */
void strip_test_run(void);

#ifdef __cplusplus
}
#endif
