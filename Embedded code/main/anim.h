/*
 * anim.h - Animation engine for the NeoPixel strip.
 *
 * Runs on its own FreeRTOS task at a fixed frame rate. This matters: a
 * proximity decision costs ~180 ms because the HC-SR04 needs 60 ms between
 * bursts and we take three samples, and an animation redrawn only that often
 * looks broken. Sensing and rendering therefore run independently, with a
 * single flag passed between them.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start the render task. Call after neo_init(). The strip begins idle
 * (dark); nothing else needs to be done to keep it running.
 */
esp_err_t anim_start(void);

/*
 * Tell the animation whether something is currently close. Cheap and safe to
 * call from another task at whatever rate the sensor produces answers; the
 * renderer picks the change up on its next frame.
 *
 * false -> true starts a white comet down the strip, which settles into a
 *          cycling rainbow with sparkles for as long as the target stays.
 * true -> false fades the strip out rather than cutting it dead.
 */
void anim_set_triggered(bool triggered);

#ifdef __cplusplus
}
#endif
