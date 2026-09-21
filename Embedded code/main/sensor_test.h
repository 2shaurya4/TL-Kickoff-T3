/*
 * sensor_test.h - Bare HC-SR04 bring-up test.
 *
 * Set APP_MODE_SENSOR_TEST to 1 in main.c to run this instead of the lamp.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialise only the ultrasonic sensor (the LED strip is deliberately left
 * alone, so a miswired strip cannot mask a working sensor) and stream one
 * reading per line to the console forever. Never returns.
 */
void sensor_test_run(void);

#ifdef __cplusplus
}
#endif
