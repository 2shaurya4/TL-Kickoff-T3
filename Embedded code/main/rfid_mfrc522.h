/*
 * rfid_mfrc522.h - Minimal ESP-IDF helper driver for an MFRC522 13.56 MHz
 *                  RFID reader ("RC522" breakout) over SPI on an
 *                  ESP32-S3-DevKitC.
 *
 * Reads the UID of any ISO 14443A card/tag (4-, 7- or 10-byte UIDs), and can
 * read/write data blocks on MIFARE Classic 1K/4K cards - the white card and
 * blue key fob that ship with most RC522 kits are MIFARE Classic 1K.
 *
 * Wiring note: the RC522 is a 3.3 V part. Power it from 3V3, never 5 V; its
 * I/O then connects straight to the ESP32-S3 with no level shifting. The pin
 * the board silk calls "SDA" is SPI chip-select when the module is in SPI
 * mode (which the common blue breakout is, hard-wired). IRQ can stay
 * unconnected - this driver polls.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Wiring - PLACEHOLDERS. Change these to the pins you actually used,  */
/* or -D them from CMake. The defaults are SPI2's native IO_MUX pins   */
/* on the S3 and stay clear of GPIO5/6/7 (HC-SR04 and the strip).      */
/* ------------------------------------------------------------------ */
#ifndef RFID_SPI_HOST
#define RFID_SPI_HOST       SPI2_HOST
#endif

#ifndef RFID_SCK_GPIO
#define RFID_SCK_GPIO       GPIO_NUM_12     /* <<< PLACEHOLDER (board: SCK) */
#endif

#ifndef RFID_MOSI_GPIO
#define RFID_MOSI_GPIO      GPIO_NUM_11     /* <<< PLACEHOLDER (board: MOSI) */
#endif

#ifndef RFID_MISO_GPIO
#define RFID_MISO_GPIO      GPIO_NUM_13     /* <<< PLACEHOLDER (board: MISO) */
#endif

#ifndef RFID_CS_GPIO
#define RFID_CS_GPIO        GPIO_NUM_10     /* <<< PLACEHOLDER (board: SDA) */
#endif

/* Reset / power-down pin (board: RST). Plain number so it can be -1:
 * set to -1 if it is not wired and the driver falls back to a soft reset
 * (then tie RST to 3V3 on the board, or the chip stays powered down). */
#ifndef RFID_RST_GPIO
#define RFID_RST_GPIO       14              /* <<< PLACEHOLDER (board: RST) */
#endif

/* The MFRC522 is rated to 10 MHz SPI; 5 MHz leaves margin for jumper wires. */
#ifndef RFID_SPI_CLOCK_HZ
#define RFID_SPI_CLOCK_HZ   (5 * 1000 * 1000)
#endif

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

#define RFID_UID_MAX_LEN    10

/* One card's identity, as returned by the select procedure. */
typedef struct {
    uint8_t  bytes[RFID_UID_MAX_LEN];
    uint8_t  len;       /* 4, 7 or 10                                    */
    uint8_t  sak;       /* Select Acknowledge - tells you the card type  */
    uint16_t atqa;      /* Answer To Request, as received (LSB first)    */
} rfid_uid_t;

typedef enum {
    RFID_TYPE_UNKNOWN = 0,
    RFID_TYPE_MIFARE_MINI,      /* 320 bytes                                */
    RFID_TYPE_MIFARE_1K,        /* the usual kit card / fob                 */
    RFID_TYPE_MIFARE_4K,
    RFID_TYPE_MIFARE_UL,        /* Ultralight / NTAG21x stickers            */
    RFID_TYPE_MIFARE_PLUS,
    RFID_TYPE_ISO_14443_4,      /* smart cards, phones, DESFire, bank cards */
    RFID_TYPE_ISO_18092,        /* NFC peer-to-peer                          */
} rfid_card_type_t;

/* Factory transport key on every blank MIFARE Classic card: FF FF FF FF FF FF. */
extern const uint8_t RFID_MIFARE_DEFAULT_KEY[6];

#define RFID_MIFARE_BLOCK_SIZE 16

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

/*
 * Bring up the SPI bus on the pins above (or join it if the application
 * already initialised RFID_SPI_HOST), reset the MFRC522, check it answers,
 * configure its timer and switch the antenna on.
 */
esp_err_t rfid_init(void);

/*
 * Chip version register. 0x91 = MFRC522 v1.0, 0x92 = v2.0. Clone boards
 * often report 0x88, 0x12 or 0xB2 and work fine. 0x00 or 0xFF means SPI is
 * not reaching the chip - check wiring and that RST is high.
 */
esp_err_t rfid_get_version(uint8_t *version);

/* Release the SPI device (and the bus, if rfid_init() created it). */
esp_err_t rfid_deinit(void);

/* ------------------------------------------------------------------ */
/* Reading cards                                                       */
/* ------------------------------------------------------------------ */

/* Is any card in the field right now? Cheap enough to poll every ~50 ms. */
bool rfid_card_present(void);

/*
 * Read the UID of the card in the field. Returns ESP_ERR_NOT_FOUND when no
 * card is there. Succeeds every time it is called while a card stays on the
 * reader - use rfid_read_new_uid() if you want one event per tap.
 */
esp_err_t rfid_read_uid(rfid_uid_t *uid);

/*
 * One event per tap: returns ESP_OK only the first time a card is seen after
 * the reader has been empty (or a different card is presented), and
 * ESP_ERR_NOT_FOUND on every other call. Poll it from a loop.
 */
esp_err_t rfid_read_new_uid(rfid_uid_t *uid);

/*
 * Poll until a card shows up or timeout_ms passes (ESP_ERR_TIMEOUT).
 * Pass UINT32_MAX to wait forever.
 */
esp_err_t rfid_wait_for_card(rfid_uid_t *uid, uint32_t timeout_ms);

/* ------------------------------------------------------------------ */
/* UID utilities                                                       */
/* ------------------------------------------------------------------ */

/* True if both UIDs have the same length and bytes. */
bool rfid_uid_equal(const rfid_uid_t *a, const rfid_uid_t *b);

/* Format as "DE:AD:BE:EF". Needs 3 * len bytes; 30 always suffices. */
void rfid_uid_to_str(const rfid_uid_t *uid, char *buf, size_t buf_len);

/* Decode the SAK byte into a card family. */
rfid_card_type_t rfid_card_type(uint8_t sak);
const char *rfid_card_type_str(rfid_card_type_t type);

/* ------------------------------------------------------------------ */
/* MIFARE Classic data blocks                                          */
/* ------------------------------------------------------------------ */

/*
 * Read one 16-byte block from the card in the field, authenticating with
 * key A (pass RFID_MIFARE_DEFAULT_KEY for a blank card). Each call is a full
 * select -> authenticate -> read -> halt cycle, so no other call is needed
 * around it. `uid` may be NULL; if given it receives the card's UID.
 *
 * Returns ESP_ERR_NOT_FOUND with no card, ESP_ERR_NOT_SUPPORTED if the card
 * is not MIFARE Classic, ESP_ERR_INVALID_RESPONSE if the key was wrong.
 */
esp_err_t rfid_mifare_read_block(uint8_t block, const uint8_t key_a[6],
                                 uint8_t out[RFID_MIFARE_BLOCK_SIZE],
                                 rfid_uid_t *uid);

/*
 * Write one 16-byte data block, same cycle and errors as the read.
 *
 * Refuses (ESP_ERR_INVALID_ARG) block 0 and every sector trailer (blocks
 * 3, 7, 11, ... and 15-of-16 in the 4K upper sectors): writing a bad trailer
 * permanently locks that sector, so that is left to deliberate code.
 */
esp_err_t rfid_mifare_write_block(uint8_t block, const uint8_t key_a[6],
                                  const uint8_t data[RFID_MIFARE_BLOCK_SIZE],
                                  rfid_uid_t *uid);

#ifdef __cplusplus
}
#endif
