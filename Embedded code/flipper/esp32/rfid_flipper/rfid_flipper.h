/*
 * rfid_flipper.h - ESP32 side of the Flipper Zero RFID link.
 *
 * The Flipper runs the "UID Stream" app (flipper/app/) and prints one line
 * per event out its GPIO UART; this component reads those lines on a
 * background task and hands them over as card events.
 *
 * Wiring (3.3 V both sides, no level shifting):
 *   Flipper pin 13 (TX)  -> ESP32 RFID_UART_RX_GPIO
 *   Flipper pin 14 (RX)  <- ESP32 RFID_UART_TX_GPIO   (optional, unused today)
 *   Flipper pin 11 (GND) -- ESP32 GND                 (required)
 *
 * Do not power the board from the Flipper's 3V3 pin if the LED strip is
 * attached: that rail is good for ~1.2 A and the strip alone can exceed it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Wiring - PLACEHOLDERS. Change to the pins you actually used. These  */
/* stay clear of GPIO5/6/7 (HC-SR04, LED strip).                       */
/* ------------------------------------------------------------------ */
#ifndef RFID_UART_PORT
#define RFID_UART_PORT      UART_NUM_1      /* UART0 is the console - leave it */
#endif

#ifndef RFID_UART_RX_GPIO
#define RFID_UART_RX_GPIO   GPIO_NUM_17     /* <<< PLACEHOLDER (from pin 13) */
#endif

/* Set to -1 if you only wire the one direction. Nothing is sent today; the
 * pin is here so the Flipper app can grow a command channel later. */
#ifndef RFID_UART_TX_GPIO
#define RFID_UART_TX_GPIO   18              /* <<< PLACEHOLDER (to pin 14) */
#endif

#ifndef RFID_UART_BAUD
#define RFID_UART_BAUD      115200          /* must match the Flipper app */
#endif

/*
 * The app pings every 2 s. If nothing at all arrives for this long, treat the
 * link as down - the Flipper is off, asleep, running a different app, or the
 * jumper fell off.
 */
#ifndef RFID_LINK_TIMEOUT_MS
#define RFID_LINK_TIMEOUT_MS 6000
#endif

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

#define RFID_UID_MAX_LEN    10

typedef struct {
    uint8_t  bytes[RFID_UID_MAX_LEN];
    uint8_t  len;       /* 4, 7 or 10                                   */
    uint8_t  sak;       /* Select Acknowledge - card family             */
    uint16_t atqa;      /* Answer To Request                            */
} rfid_uid_t;

typedef enum {
    RFID_TYPE_UNKNOWN = 0,
    RFID_TYPE_MIFARE_MINI,
    RFID_TYPE_MIFARE_1K,
    RFID_TYPE_MIFARE_4K,
    RFID_TYPE_MIFARE_UL,        /* Ultralight / NTAG                    */
    RFID_TYPE_MIFARE_PLUS,
    RFID_TYPE_ISO_14443_4,      /* smart cards, phones                  */
    RFID_TYPE_ISO_18092,        /* NFC peer-to-peer                     */
} rfid_card_type_t;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* Configure the UART and start the receive task. Succeeds even if the
 * Flipper is not connected yet - check rfid_link_ok() for that. */
esp_err_t rfid_init(void);

/*
 * Wait up to timeout_ms for the next tap (0 = poll, UINT32_MAX = forever)
 * and return the card's UID. One event per tap: a card left resting on the
 * Flipper does not repeat. Returns ESP_ERR_TIMEOUT when nothing arrived.
 */
esp_err_t rfid_read_new_uid(rfid_uid_t *uid, uint32_t timeout_ms);

/* Is a card sitting on the Flipper right now? */
bool rfid_card_present(void);

/* The most recent card seen, tap or not. ESP_ERR_NOT_FOUND if none yet. */
esp_err_t rfid_get_last_uid(rfid_uid_t *uid);

/*
 * Has the Flipper said anything within RFID_LINK_TIMEOUT_MS? False means the
 * reader is not there - worth showing on the strip, since with a separate
 * handheld doing the reading there is no other way to notice.
 */
bool rfid_link_ok(void);

/* Throw away any queued taps (e.g. after a mode change). */
void rfid_flush(void);

/* Stop the task and release the UART. */
esp_err_t rfid_deinit(void);

/* ---- UID utilities ------------------------------------------------- */

bool rfid_uid_equal(const rfid_uid_t *a, const rfid_uid_t *b);

/* Format as "04:A2:3B:1C". Needs 3 * len bytes; 32 always suffices. */
void rfid_uid_to_str(const rfid_uid_t *uid, char *buf, size_t buf_len);

rfid_card_type_t rfid_card_type(uint8_t sak);
const char *rfid_card_type_str(rfid_card_type_t type);

#ifdef __cplusplus
}
#endif
