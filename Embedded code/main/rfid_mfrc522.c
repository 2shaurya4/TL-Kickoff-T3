/*
 * rfid_mfrc522.c - helper functions to read cards off an MFRC522 over SPI.
 *
 * Register map and command sequences follow the NXP MFRC522 datasheet
 * (rev 3.9) and ISO/IEC 14443-3 type A. The card-side CRC_A is computed in
 * software rather than on the chip, which saves a poll loop per frame.
 */

#include "rfid_mfrc522.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "mfrc522";

/* ---- MFRC522 registers ------------------------------------------- */
#define REG_COMMAND         0x01
#define REG_COM_IRQ         0x04
#define REG_DIV_IRQ         0x05
#define REG_ERROR           0x06
#define REG_STATUS2         0x08
#define REG_FIFO_DATA       0x09
#define REG_FIFO_LEVEL      0x0A
#define REG_CONTROL         0x0C
#define REG_BIT_FRAMING     0x0D
#define REG_COLL            0x0E
#define REG_MODE            0x11
#define REG_TX_MODE         0x12
#define REG_RX_MODE         0x13
#define REG_TX_CONTROL      0x14
#define REG_TX_ASK          0x15
#define REG_MOD_WIDTH       0x24
#define REG_T_MODE          0x2A
#define REG_T_PRESCALER     0x2B
#define REG_T_RELOAD_H      0x2C
#define REG_T_RELOAD_L      0x2D
#define REG_VERSION         0x37

/* ---- MFRC522 commands -------------------------------------------- */
#define CMD_IDLE            0x00
#define CMD_TRANSCEIVE      0x0C
#define CMD_MF_AUTHENT      0x0E
#define CMD_SOFT_RESET      0x0F

/* ---- Register bits ------------------------------------------------ */
#define COMMAND_POWER_DOWN  0x10
#define IRQ_TIMER           0x01
#define IRQ_ERR             0x02
#define IRQ_IDLE            0x10
#define IRQ_RX              0x20
#define ERR_PROTOCOL        0x01
#define ERR_PARITY          0x02
#define ERR_COLL            0x08
#define ERR_BUFFER_OVFL     0x10
#define STATUS2_CRYPTO1_ON  0x08
#define BIT_FRAMING_START   0x80
#define FIFO_FLUSH          0x80
#define COLL_VALUES_AFTER   0x80

/* ---- ISO 14443A / MIFARE commands --------------------------------- */
#define PICC_WUPA           0x52
#define PICC_CASCADE_TAG    0x88
#define PICC_HLTA           0x50
#define PICC_MF_AUTH_KEY_A  0x60
#define PICC_MF_READ        0x30
#define PICC_MF_WRITE       0xA0
#define PICC_MF_ACK         0x0A

static const uint8_t SEL_CMD[3] = { 0x93, 0x95, 0x97 };

const uint8_t RFID_MIFARE_DEFAULT_KEY[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ESP32-S3 without DMA moves at most 64 bytes per SPI transaction, so FIFO
 * traffic is split into chunks (the FIFO itself is 64 bytes deep). */
#define SPI_CHUNK           32

/* The chip's own timer gives up after 25 ms; this is the wall-clock backstop
 * in case the IRQ bits never move at all (chip unplugged mid-transfer). */
#define XFER_TIMEOUT_US     40000

/* rfid_read_new_uid(): polls in a row without a card before it counts as
 * removed. A card resting on the reader occasionally misses one poll. */
#define ABSENT_POLLS        2

/* ---- Module state -------------------------------------------------- */
static spi_device_handle_t s_dev;
static SemaphoreHandle_t   s_lock;
static bool                s_bus_is_ours;
static rfid_uid_t          s_last_uid;
static bool                s_last_valid;
static uint8_t             s_absent_count;

/* ==================================================================== */
/* SPI register access                                                  */
/* ==================================================================== */

static inline uint8_t addr_write(uint8_t reg) { return (uint8_t)((reg << 1) & 0x7E); }
static inline uint8_t addr_read(uint8_t reg)  { return (uint8_t)(0x80 | ((reg << 1) & 0x7E)); }

static esp_err_t reg_write_n(uint8_t reg, const uint8_t *data, size_t n)
{
    uint8_t tx[SPI_CHUNK + 1];

    while (n > 0) {
        const size_t chunk = (n > SPI_CHUNK) ? SPI_CHUNK : n;
        spi_transaction_t t = {
            .length = (chunk + 1) * 8,
            .tx_buffer = tx,
        };

        tx[0] = addr_write(reg);
        memcpy(&tx[1], data, chunk);
        ESP_RETURN_ON_ERROR(spi_device_polling_transmit(s_dev, &t), TAG,
                            "spi write failed");
        data += chunk;
        n -= chunk;
    }
    return ESP_OK;
}

/* The MFRC522 reads back by clocking the address repeatedly; the byte on
 * MISO during address i is the data for address i-1. */
static esp_err_t reg_read_n(uint8_t reg, uint8_t *out, size_t n)
{
    uint8_t tx[SPI_CHUNK + 1];
    uint8_t rx[SPI_CHUNK + 1];

    while (n > 0) {
        const size_t chunk = (n > SPI_CHUNK) ? SPI_CHUNK : n;
        spi_transaction_t t = {
            .length = (chunk + 1) * 8,
            .tx_buffer = tx,
            .rx_buffer = rx,
        };

        memset(tx, addr_read(reg), chunk);
        tx[chunk] = 0x00;
        ESP_RETURN_ON_ERROR(spi_device_polling_transmit(s_dev, &t), TAG,
                            "spi read failed");
        memcpy(out, &rx[1], chunk);
        out += chunk;
        n -= chunk;
    }
    return ESP_OK;
}

static esp_err_t reg_write(uint8_t reg, uint8_t value)
{
    return reg_write_n(reg, &value, 1);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *value)
{
    return reg_read_n(reg, value, 1);
}

static esp_err_t reg_set_bits(uint8_t reg, uint8_t mask)
{
    uint8_t v;
    ESP_RETURN_ON_ERROR(reg_read(reg, &v), TAG, "read failed");
    return reg_write(reg, v | mask);
}

static esp_err_t reg_clear_bits(uint8_t reg, uint8_t mask)
{
    uint8_t v;
    ESP_RETURN_ON_ERROR(reg_read(reg, &v), TAG, "read failed");
    return reg_write(reg, v & (uint8_t)~mask);
}

/* ==================================================================== */
/* ISO 14443A CRC_A (poly 0x8408 reflected, preset 0x6363)              */
/* ==================================================================== */

static uint16_t crc_a(const uint8_t *data, size_t len)
{
    uint16_t crc = 0x6363;

    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i] ^ (uint8_t)(crc & 0xFF);
        b ^= (uint8_t)(b << 4);
        crc = (uint16_t)((crc >> 8) ^ ((uint16_t)b << 8) ^ ((uint16_t)b << 3) ^
                         (b >> 4));
    }
    return crc;
}

static void append_crc(uint8_t *frame, size_t len)
{
    const uint16_t crc = crc_a(frame, len);
    frame[len] = (uint8_t)(crc & 0xFF);
    frame[len + 1] = (uint8_t)(crc >> 8);
}

static bool check_crc(const uint8_t *frame, size_t len_with_crc)
{
    if (len_with_crc < 3) {
        return false;
    }
    const uint16_t crc = crc_a(frame, len_with_crc - 2);
    return frame[len_with_crc - 2] == (uint8_t)(crc & 0xFF) &&
           frame[len_with_crc - 1] == (uint8_t)(crc >> 8);
}

/* ==================================================================== */
/* Talking to the card                                                  */
/* ==================================================================== */

/*
 * Run one chip command that exchanges a frame with the card.
 *
 *  tx_last_bits  number of valid bits in the last TX byte (0 = all 8);
 *                only REQA/WUPA short frames use 7.
 *  rx / rx_len   in: capacity, out: bytes received. rx may be NULL.
 *  rx_last_bits  out (optional): valid bits in the last RX byte, 0 = all 8.
 *
 * ESP_ERR_TIMEOUT means the card never answered, which for most callers
 * simply means "no card".
 */
static esp_err_t pcd_communicate(uint8_t command, uint8_t wait_irq,
                                 const uint8_t *tx, size_t tx_len,
                                 uint8_t tx_last_bits,
                                 uint8_t *rx, size_t *rx_len,
                                 uint8_t *rx_last_bits)
{
    uint8_t irq = 0;
    uint8_t err_reg = 0;
    uint8_t level = 0;
    uint8_t control = 0;

    ESP_RETURN_ON_ERROR(reg_write(REG_COMMAND, CMD_IDLE), TAG, "idle failed");
    ESP_RETURN_ON_ERROR(reg_write(REG_COM_IRQ, 0x7F), TAG, "irq clear failed");
    ESP_RETURN_ON_ERROR(reg_write(REG_FIFO_LEVEL, FIFO_FLUSH), TAG, "flush failed");
    ESP_RETURN_ON_ERROR(reg_write_n(REG_FIFO_DATA, tx, tx_len), TAG, "fifo failed");
    ESP_RETURN_ON_ERROR(reg_write(REG_BIT_FRAMING, tx_last_bits & 0x07), TAG,
                        "framing failed");
    ESP_RETURN_ON_ERROR(reg_write(REG_COMMAND, command), TAG, "command failed");

    if (command == CMD_TRANSCEIVE) {
        ESP_RETURN_ON_ERROR(reg_set_bits(REG_BIT_FRAMING, BIT_FRAMING_START), TAG,
                            "start send failed");
    }

    const int64_t deadline = esp_timer_get_time() + XFER_TIMEOUT_US;
    for (;;) {
        ESP_RETURN_ON_ERROR(reg_read(REG_COM_IRQ, &irq), TAG, "irq read failed");
        if (irq & wait_irq) {
            break;
        }
        if (irq & IRQ_TIMER) {
            return ESP_ERR_TIMEOUT;         /* chip timer: nobody answered */
        }
        if (esp_timer_get_time() > deadline) {
            ESP_LOGW(TAG, "transfer hung - is the reader still connected?");
            return ESP_ERR_TIMEOUT;
        }
    }

    ESP_RETURN_ON_ERROR(reg_read(REG_ERROR, &err_reg), TAG, "error read failed");
    if (err_reg & (ERR_BUFFER_OVFL | ERR_PARITY | ERR_PROTOCOL)) {
        return ESP_FAIL;
    }
    if (err_reg & ERR_COLL) {
        return ESP_ERR_INVALID_STATE;       /* two cards answered at once */
    }

    if (rx != NULL && rx_len != NULL) {
        ESP_RETURN_ON_ERROR(reg_read(REG_FIFO_LEVEL, &level), TAG, "level failed");
        level &= 0x7F;
        if (level > *rx_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        *rx_len = level;
        if (level > 0) {
            ESP_RETURN_ON_ERROR(reg_read_n(REG_FIFO_DATA, rx, level), TAG,
                                "fifo read failed");
        }
        ESP_RETURN_ON_ERROR(reg_read(REG_CONTROL, &control), TAG, "ctrl failed");
        if (rx_last_bits != NULL) {
            *rx_last_bits = control & 0x07;
        }
    }
    return ESP_OK;
}

static esp_err_t transceive(const uint8_t *tx, size_t tx_len, uint8_t tx_last_bits,
                            uint8_t *rx, size_t *rx_len, uint8_t *rx_last_bits)
{
    return pcd_communicate(CMD_TRANSCEIVE, IRQ_RX | IRQ_IDLE, tx, tx_len,
                           tx_last_bits, rx, rx_len, rx_last_bits);
}

/* WUPA wakes cards in IDLE *and* HALT, so a card left on the reader can be
 * read again on every call. Fills in the ATQA. */
static esp_err_t picc_wakeup(uint16_t *atqa)
{
    const uint8_t cmd = PICC_WUPA;
    uint8_t rx[2];
    size_t rx_len = sizeof(rx);
    uint8_t last_bits = 0;

    ESP_RETURN_ON_ERROR(reg_clear_bits(REG_COLL, COLL_VALUES_AFTER), TAG,
                        "coll reg failed");

    esp_err_t err = transceive(&cmd, 1, 7, rx, &rx_len, &last_bits);
    if (err == ESP_ERR_INVALID_STATE) {
        /* Several cards answered - still "a card is present". Their ATQAs
         * collided, so there is nothing meaningful to report. */
        if (atqa) {
            *atqa = 0;
        }
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (rx_len != 2 || last_bits != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (atqa) {
        *atqa = (uint16_t)(rx[0] | (rx[1] << 8));
    }
    return ESP_OK;
}

/* Walk the cascade levels (1 for 4-byte UIDs, 2 for 7, 3 for 10). Assumes
 * a single card: with two cards on the reader this reports a collision
 * rather than arbitrating between them. */
static esp_err_t picc_select(rfid_uid_t *uid)
{
    uid->len = 0;

    for (int level = 0; level < 3; level++) {
        uint8_t anticoll[2] = { SEL_CMD[level], 0x20 };
        uint8_t cl[5];
        size_t cl_len = sizeof(cl);

        esp_err_t err = transceive(anticoll, sizeof(anticoll), 0, cl, &cl_len, NULL);
        if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "more than one card in the field");
            return err;
        }
        ESP_RETURN_ON_ERROR(err, TAG, "anticollision failed at level %d", level + 1);
        if (cl_len != 5 || (cl[0] ^ cl[1] ^ cl[2] ^ cl[3]) != cl[4]) {
            return ESP_ERR_INVALID_CRC;     /* BCC mismatch */
        }

        uint8_t sel[9] = { SEL_CMD[level], 0x70, cl[0], cl[1], cl[2], cl[3], cl[4] };
        uint8_t sak[3];
        size_t sak_len = sizeof(sak);

        append_crc(sel, 7);
        ESP_RETURN_ON_ERROR(transceive(sel, sizeof(sel), 0, sak, &sak_len, NULL),
                            TAG, "select failed at level %d", level + 1);
        if (sak_len != 3 || !check_crc(sak, 3)) {
            return ESP_ERR_INVALID_CRC;
        }

        if (sak[0] & 0x04) {
            /* Cascade bit: UID continues at the next level. cl[0] is the
             * cascade tag 0x88, not part of the UID. */
            if (cl[0] != PICC_CASCADE_TAG) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            memcpy(&uid->bytes[uid->len], &cl[1], 3);
            uid->len += 3;
        } else {
            memcpy(&uid->bytes[uid->len], cl, 4);
            uid->len += 4;
            uid->sak = sak[0];
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_RESPONSE;
}

/* Put the card to sleep. Per ISO 14443-3 the card must NOT answer HLTA, so a
 * timeout is the success case. */
static void picc_halt(void)
{
    uint8_t hlta[4] = { PICC_HLTA, 0x00 };
    uint8_t rx[4];
    size_t rx_len = sizeof(rx);

    append_crc(hlta, 2);
    (void)transceive(hlta, sizeof(hlta), 0, rx, &rx_len, NULL);
}

static void stop_crypto(void)
{
    (void)reg_clear_bits(REG_STATUS2, STATUS2_CRYPTO1_ON);
}

/* Wake + select, with one retry: a card just entering the field sometimes
 * misses the first WUPA while its supply is still coming up. */
static esp_err_t activate_card(rfid_uid_t *uid)
{
    esp_err_t err = ESP_ERR_TIMEOUT;

    memset(uid, 0, sizeof(*uid));
    for (int attempt = 0; attempt < 2; attempt++) {
        err = picc_wakeup(&uid->atqa);
        if (err == ESP_OK) {
            err = picc_select(uid);
            if (err == ESP_OK) {
                return ESP_OK;
            }
        }
    }
    return (err == ESP_ERR_TIMEOUT) ? ESP_ERR_NOT_FOUND : err;
}

static esp_err_t mifare_auth(uint8_t block, const uint8_t key[6], const rfid_uid_t *uid)
{
    uint8_t frame[12] = { PICC_MF_AUTH_KEY_A, block };
    uint8_t status2 = 0;

    memcpy(&frame[2], key, 6);
    /* AN10927 3.2.5: authenticate with the last four UID bytes. */
    memcpy(&frame[8], &uid->bytes[uid->len - 4], 4);

    esp_err_t err = pcd_communicate(CMD_MF_AUTHENT, IRQ_IDLE, frame, sizeof(frame),
                                    0, NULL, NULL, NULL);
    if (err == ESP_ERR_TIMEOUT || err == ESP_FAIL) {
        return ESP_ERR_INVALID_RESPONSE;    /* wrong key: card goes silent */
    }
    ESP_RETURN_ON_ERROR(err, TAG, "auth command failed");

    ESP_RETURN_ON_ERROR(reg_read(REG_STATUS2, &status2), TAG, "status read failed");
    return (status2 & STATUS2_CRYPTO1_ON) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

/* MIFARE answers writes with a 4-bit ACK. */
static esp_err_t mifare_transceive_ack(const uint8_t *frame, size_t len)
{
    uint8_t rx[1];
    size_t rx_len = sizeof(rx);
    uint8_t last_bits = 0;

    ESP_RETURN_ON_ERROR(transceive(frame, len, 0, rx, &rx_len, &last_bits), TAG,
                        "no ack");
    if (rx_len != 1 || last_bits != 4 || (rx[0] & 0x0F) != PICC_MF_ACK) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static bool is_mifare_classic(uint8_t sak)
{
    const rfid_card_type_t t = rfid_card_type(sak);
    return t == RFID_TYPE_MIFARE_MINI || t == RFID_TYPE_MIFARE_1K ||
           t == RFID_TYPE_MIFARE_4K;
}

static bool is_protected_block(uint8_t block)
{
    if (block == 0) {
        return true;                                    /* manufacturer */
    }
    if (block < 128) {
        return (block % 4) == 3;                        /* 4-block sectors */
    }
    return (block % 16) == 15;                          /* 16-block sectors (4K) */
}

/* ==================================================================== */
/* Chip bring-up                                                        */
/* ==================================================================== */

static esp_err_t pcd_reset(void)
{
#if RFID_RST_GPIO >= 0
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << RFID_RST_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "RST config failed");
    gpio_set_level(RFID_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(RFID_RST_GPIO, 1);
#else
    ESP_RETURN_ON_ERROR(reg_write(REG_COMMAND, CMD_SOFT_RESET), TAG,
                        "soft reset failed");
#endif

    /* Oscillator start-up takes up to ~37.7 ms; wait for PowerDown to clear. */
    vTaskDelay(pdMS_TO_TICKS(50));
    for (int i = 0; i < 10; i++) {
        uint8_t cmd = 0;
        ESP_RETURN_ON_ERROR(reg_read(REG_COMMAND, &cmd), TAG, "read failed");
        if ((cmd & COMMAND_POWER_DOWN) == 0) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t pcd_configure(void)
{
    /* 106 kbit/s both ways, standard modulation width. */
    ESP_RETURN_ON_ERROR(reg_write(REG_TX_MODE, 0x00), TAG, "cfg failed");
    ESP_RETURN_ON_ERROR(reg_write(REG_RX_MODE, 0x00), TAG, "cfg failed");
    ESP_RETURN_ON_ERROR(reg_write(REG_MOD_WIDTH, 0x26), TAG, "cfg failed");

    /* Timer: auto-start after each transmission, 13.56 MHz / (2*169+1)
     * = 40 kHz tick, reload 1000 -> 25 ms "no card answered" timeout. */
    ESP_RETURN_ON_ERROR(reg_write(REG_T_MODE, 0x80), TAG, "cfg failed");
    ESP_RETURN_ON_ERROR(reg_write(REG_T_PRESCALER, 0xA9), TAG, "cfg failed");
    ESP_RETURN_ON_ERROR(reg_write(REG_T_RELOAD_H, 0x03), TAG, "cfg failed");
    ESP_RETURN_ON_ERROR(reg_write(REG_T_RELOAD_L, 0xE8), TAG, "cfg failed");

    /* 100 % ASK modulation (what type A needs), CRC preset 0x6363. */
    ESP_RETURN_ON_ERROR(reg_write(REG_TX_ASK, 0x40), TAG, "cfg failed");
    ESP_RETURN_ON_ERROR(reg_write(REG_MODE, 0x3D), TAG, "cfg failed");

    /* Antenna drivers TX1 and TX2 on. */
    return reg_set_bits(REG_TX_CONTROL, 0x03);
}

/* ==================================================================== */
/* Public API                                                           */
/* ==================================================================== */

#define LOCK()      xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK()    xSemaphoreGive(s_lock)

#define REQUIRE_INIT() \
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG, \
                        "call rfid_init() first")

esp_err_t rfid_init(void)
{
    uint8_t version = 0;

    ESP_RETURN_ON_FALSE(s_dev == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "already initialised");

    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = RFID_MOSI_GPIO,
        .miso_io_num = RFID_MISO_GPIO,
        .sclk_io_num = RFID_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_CHUNK + 1,
    };
    const spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = RFID_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = RFID_CS_GPIO,
        .queue_size = 1,
    };

    esp_err_t err = spi_bus_initialize(RFID_SPI_HOST, &bus_cfg, SPI_DMA_DISABLED);
    if (err == ESP_OK) {
        s_bus_is_ours = true;
    } else if (err == ESP_ERR_INVALID_STATE) {
        s_bus_is_ours = false;              /* app already owns the bus */
    } else {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    err = spi_bus_add_device(RFID_SPI_HOST, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        goto fail_bus;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail_dev;
    }

    err = pcd_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "chip did not come out of reset - check RST and 3V3");
        goto fail_lock;
    }

    err = reg_read(REG_VERSION, &version);
    if (err == ESP_OK && (version == 0x00 || version == 0xFF)) {
        ESP_LOGE(TAG, "version reads 0x%02X: SPI is not reaching the MFRC522",
                 version);
        err = ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        goto fail_lock;
    }

    err = pcd_configure();
    if (err != ESP_OK) {
        goto fail_lock;
    }

    s_last_valid = false;
    s_absent_count = 0;

    ESP_LOGI(TAG, "MFRC522 ready (version 0x%02X%s) on SCK=%d MOSI=%d MISO=%d CS=%d RST=%d",
             version,
             (version == 0x91 || version == 0x92) ? "" : ", likely a clone",
             (int)RFID_SCK_GPIO, (int)RFID_MOSI_GPIO, (int)RFID_MISO_GPIO,
             (int)RFID_CS_GPIO, (int)RFID_RST_GPIO);
    return ESP_OK;

fail_lock:
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
fail_dev:
    spi_bus_remove_device(s_dev);
    s_dev = NULL;
fail_bus:
    if (s_bus_is_ours) {
        spi_bus_free(RFID_SPI_HOST);
        s_bus_is_ours = false;
    }
    return err;
}

esp_err_t rfid_get_version(uint8_t *version)
{
    ESP_RETURN_ON_FALSE(version != NULL, ESP_ERR_INVALID_ARG, TAG, "null arg");
    REQUIRE_INIT();

    LOCK();
    esp_err_t err = reg_read(REG_VERSION, version);
    UNLOCK();
    return err;
}

bool rfid_card_present(void)
{
    uint16_t atqa;

    if (s_dev == NULL) {
        return false;
    }

    LOCK();
    esp_err_t err = picc_wakeup(&atqa);
    if (err == ESP_OK) {
        /* Card is now READY; any unexpected frame drops it back to IDLE so
         * the next WUPA finds it in a known state. */
        picc_halt();
    }
    UNLOCK();
    return err == ESP_OK;
}

esp_err_t rfid_read_uid(rfid_uid_t *uid)
{
    ESP_RETURN_ON_FALSE(uid != NULL, ESP_ERR_INVALID_ARG, TAG, "null arg");
    REQUIRE_INIT();

    LOCK();
    esp_err_t err = activate_card(uid);
    if (err == ESP_OK) {
        picc_halt();
    }
    UNLOCK();
    return err;
}

esp_err_t rfid_read_new_uid(rfid_uid_t *uid)
{
    rfid_uid_t seen;

    ESP_RETURN_ON_FALSE(uid != NULL, ESP_ERR_INVALID_ARG, TAG, "null arg");

    esp_err_t err = rfid_read_uid(&seen);
    if (err != ESP_OK) {
        if (s_absent_count < ABSENT_POLLS) {
            s_absent_count++;
        }
        if (s_absent_count >= ABSENT_POLLS) {
            s_last_valid = false;           /* reader has been empty */
        }
        return (err == ESP_ERR_NOT_FOUND) ? ESP_ERR_NOT_FOUND : err;
    }

    s_absent_count = 0;
    if (s_last_valid && rfid_uid_equal(&seen, &s_last_uid)) {
        return ESP_ERR_NOT_FOUND;           /* same card, still resting there */
    }

    s_last_uid = seen;
    s_last_valid = true;
    *uid = seen;
    return ESP_OK;
}

esp_err_t rfid_wait_for_card(rfid_uid_t *uid, uint32_t timeout_ms)
{
    const int64_t start = esp_timer_get_time();

    ESP_RETURN_ON_FALSE(uid != NULL, ESP_ERR_INVALID_ARG, TAG, "null arg");
    REQUIRE_INIT();

    for (;;) {
        esp_err_t err = rfid_read_uid(uid);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_INVALID_CRC &&
            err != ESP_ERR_INVALID_STATE && err != ESP_FAIL) {
            return err;                     /* driver-level fault, not a miss */
        }
        if (timeout_ms != UINT32_MAX &&
            (esp_timer_get_time() - start) / 1000 >= timeout_ms) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t rfid_mifare_read_block(uint8_t block, const uint8_t key_a[6],
                                 uint8_t out[RFID_MIFARE_BLOCK_SIZE],
                                 rfid_uid_t *uid)
{
    rfid_uid_t card;
    uint8_t cmd[4] = { PICC_MF_READ, block };
    uint8_t rx[RFID_MIFARE_BLOCK_SIZE + 2];
    size_t rx_len = sizeof(rx);

    ESP_RETURN_ON_FALSE(key_a != NULL && out != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "null arg");
    REQUIRE_INIT();

    LOCK();
    esp_err_t err = activate_card(&card);
    if (err != ESP_OK) {
        goto done;
    }
    if (uid) {
        *uid = card;
    }
    if (!is_mifare_classic(card.sak)) {
        err = ESP_ERR_NOT_SUPPORTED;
        goto halt;
    }

    err = mifare_auth(block, key_a, &card);
    if (err != ESP_OK) {
        goto halt;
    }

    append_crc(cmd, 2);
    err = transceive(cmd, sizeof(cmd), 0, rx, &rx_len, NULL);
    if (err == ESP_OK && (rx_len != sizeof(rx) || !check_crc(rx, rx_len))) {
        err = ESP_ERR_INVALID_CRC;
    }
    if (err == ESP_OK) {
        memcpy(out, rx, RFID_MIFARE_BLOCK_SIZE);
    }

halt:
    picc_halt();
    stop_crypto();
done:
    UNLOCK();
    return err;
}

esp_err_t rfid_mifare_write_block(uint8_t block, const uint8_t key_a[6],
                                  const uint8_t data[RFID_MIFARE_BLOCK_SIZE],
                                  rfid_uid_t *uid)
{
    rfid_uid_t card;
    uint8_t cmd[4] = { PICC_MF_WRITE, block };
    uint8_t payload[RFID_MIFARE_BLOCK_SIZE + 2];

    ESP_RETURN_ON_FALSE(key_a != NULL && data != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "null arg");
    ESP_RETURN_ON_FALSE(!is_protected_block(block), ESP_ERR_INVALID_ARG, TAG,
                        "block %u is block 0 or a sector trailer - refusing",
                        (unsigned)block);
    REQUIRE_INIT();

    LOCK();
    esp_err_t err = activate_card(&card);
    if (err != ESP_OK) {
        goto done;
    }
    if (uid) {
        *uid = card;
    }
    if (!is_mifare_classic(card.sak)) {
        err = ESP_ERR_NOT_SUPPORTED;
        goto halt;
    }

    err = mifare_auth(block, key_a, &card);
    if (err != ESP_OK) {
        goto halt;
    }

    /* Two-step write: command + address, then the 16 bytes; both ACKed. */
    append_crc(cmd, 2);
    err = mifare_transceive_ack(cmd, sizeof(cmd));
    if (err != ESP_OK) {
        goto halt;
    }
    memcpy(payload, data, RFID_MIFARE_BLOCK_SIZE);
    append_crc(payload, RFID_MIFARE_BLOCK_SIZE);
    err = mifare_transceive_ack(payload, sizeof(payload));

halt:
    picc_halt();
    stop_crypto();
done:
    UNLOCK();
    return err;
}

bool rfid_uid_equal(const rfid_uid_t *a, const rfid_uid_t *b)
{
    return a != NULL && b != NULL && a->len == b->len &&
           memcmp(a->bytes, b->bytes, a->len) == 0;
}

void rfid_uid_to_str(const rfid_uid_t *uid, char *buf, size_t buf_len)
{
    size_t pos = 0;

    if (buf == NULL || buf_len == 0) {
        return;
    }
    buf[0] = '\0';
    if (uid == NULL) {
        return;
    }

    for (uint8_t i = 0; i < uid->len && pos + 3 <= buf_len; i++) {
        pos += (size_t)snprintf(&buf[pos], buf_len - pos, i ? ":%02X" : "%02X",
                                uid->bytes[i]);
    }
}

rfid_card_type_t rfid_card_type(uint8_t sak)
{
    switch (sak & 0x7F) {
    case 0x09: return RFID_TYPE_MIFARE_MINI;
    case 0x08: return RFID_TYPE_MIFARE_1K;
    case 0x18: return RFID_TYPE_MIFARE_4K;
    case 0x00: return RFID_TYPE_MIFARE_UL;
    case 0x10:
    case 0x11: return RFID_TYPE_MIFARE_PLUS;
    case 0x20: return RFID_TYPE_ISO_14443_4;
    case 0x40: return RFID_TYPE_ISO_18092;
    default:   return RFID_TYPE_UNKNOWN;
    }
}

const char *rfid_card_type_str(rfid_card_type_t type)
{
    switch (type) {
    case RFID_TYPE_MIFARE_MINI:  return "MIFARE Mini";
    case RFID_TYPE_MIFARE_1K:    return "MIFARE Classic 1K";
    case RFID_TYPE_MIFARE_4K:    return "MIFARE Classic 4K";
    case RFID_TYPE_MIFARE_UL:    return "MIFARE Ultralight / NTAG";
    case RFID_TYPE_MIFARE_PLUS:  return "MIFARE Plus";
    case RFID_TYPE_ISO_14443_4:  return "ISO 14443-4 (smart card / phone)";
    case RFID_TYPE_ISO_18092:    return "ISO 18092 (NFC P2P)";
    default:                     return "unknown";
    }
}

esp_err_t rfid_deinit(void)
{
    if (s_dev == NULL) {
        return ESP_OK;
    }

    LOCK();
    (void)reg_clear_bits(REG_TX_CONTROL, 0x03);   /* antenna off */
    spi_bus_remove_device(s_dev);
    s_dev = NULL;
    if (s_bus_is_ours) {
        spi_bus_free(RFID_SPI_HOST);
        s_bus_is_ours = false;
    }
    UNLOCK();

    vSemaphoreDelete(s_lock);
    s_lock = NULL;
    s_last_valid = false;
    return ESP_OK;
}
