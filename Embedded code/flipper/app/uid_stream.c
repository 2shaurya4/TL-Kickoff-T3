/*
 * uid_stream.c - Flipper Zero app: poll for 13.56 MHz ISO14443-3A cards and
 *                stream what it sees out the GPIO UART, so an ESP32 (or any
 *                MCU) can act on taps.
 *
 * Wire pin 13 (TX) to the ESP32's RX and pin 11/18 (GND) to its GND.
 * 115200 8N1. See ../README.md for the line protocol and the wiring table.
 *
 * Heads up: the Expansion Modules service also wants USART. Turn it off in
 * Settings -> Expansion Modules, or the serial handle here comes back NULL.
 */

#include <furi.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_types.h>

#include <gui/gui.h>
#include <input/input.h>

#include <nfc/nfc.h>
#include <nfc/nfc_poller.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a_poller.h>

#define TAG "UidStream"

#define UART_BAUD           115200
#define UID_MAX_LEN         10
#define UID_STR_LEN         (UID_MAX_LEN * 2 + 1)

/* No Ready event for this long means the card has left the field. The poller
 * re-detects a resting card every cycle (~100-300 ms), so this is generous. */
#define CARD_GONE_TIMEOUT_MS 750

/* Keeps the ESP32's link watchdog fed while nothing is being tapped. */
#define PING_PERIOD_MS      2000

typedef enum {
    AppEventCard,
    AppEventExit,
} AppEventType;

typedef struct {
    AppEventType type;
    uint8_t      uid[UID_MAX_LEN];
    uint8_t      uid_len;
    uint8_t      sak;
    uint16_t     atqa;
} AppEvent;

typedef struct {
    FuriMessageQueue*    queue;
    FuriHalSerialHandle* serial;
    Nfc*                 nfc;
    NfcPoller*           poller;
    Gui*                 gui;
    ViewPort*            view_port;
    FuriMutex*           mutex;

    /* Display state, guarded by mutex. */
    char     last_uid[UID_STR_LEN];
    uint32_t tap_count;
    bool     card_present;
    bool     serial_ok;
} App;

/* ------------------------------------------------------------------ */
/* UART                                                                */
/* ------------------------------------------------------------------ */

static void uart_print(App* app, const char* fmt, ...) {
    char buf[96];
    va_list args;

    if(!app->serial_ok) return;

    va_start(args, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if(n > 0) {
        furi_hal_serial_tx(app->serial, (const uint8_t*)buf, (size_t)n);
        furi_hal_serial_tx_wait_complete(app->serial);
    }
}

static void uid_to_hex(const uint8_t* uid, uint8_t len, char* out) {
    static const char hex[] = "0123456789ABCDEF";
    uint8_t i;

    for(i = 0; i < len && i < UID_MAX_LEN; i++) {
        out[i * 2] = hex[uid[i] >> 4];
        out[i * 2 + 1] = hex[uid[i] & 0x0F];
    }
    out[i * 2] = '\0';
}

/* ------------------------------------------------------------------ */
/* NFC                                                                 */
/* ------------------------------------------------------------------ */

/*
 * Runs on the NFC worker thread, so it does no I/O of its own - it just posts
 * the sighting to the main loop. Returning NfcCommandReset restarts the
 * poller, which makes a card left sitting on the Flipper produce a fresh
 * Ready event every cycle; that stream is what the removal timeout watches.
 */
static NfcCommand poller_callback(NfcGenericEvent event, void* context) {
    App* app = context;
    Iso14443_3aPollerEvent* poller_event = event.event_data;

    furi_assert(event.protocol == NfcProtocolIso14443_3a);

    if(poller_event->type == Iso14443_3aPollerEventTypeReady) {
        const Iso14443_3aData* data = nfc_poller_get_data(app->poller);
        size_t uid_len = 0;
        const uint8_t* uid = iso14443_3a_get_uid(data, &uid_len);

        AppEvent ev = {
            .type = AppEventCard,
            .uid_len = (uint8_t)MIN(uid_len, (size_t)UID_MAX_LEN),
            .sak = data->sak,
            .atqa = (uint16_t)(data->atqa[0] | (data->atqa[1] << 8)),
        };
        memcpy(ev.uid, uid, ev.uid_len);

        furi_message_queue_put(app->queue, &ev, 0);
    }

    return NfcCommandReset;
}

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */

static void draw_callback(Canvas* canvas, void* context) {
    App* app = context;
    char line[40];

    furi_mutex_acquire(app->mutex, FuriWaitForever);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "UID Stream");

    canvas_set_font(canvas, FontSecondary);
    if(!app->serial_ok) {
        canvas_draw_str(canvas, 2, 28, "UART busy!");
        canvas_draw_str(canvas, 2, 40, "Settings -> Expansion");
        canvas_draw_str(canvas, 2, 52, "Modules -> off, retry");
    } else {
        snprintf(line, sizeof(line), "TX pin 13 @ %d", UART_BAUD);
        canvas_draw_str(canvas, 2, 26, line);

        canvas_draw_str(canvas, 2, 40,
                        app->card_present ? app->last_uid : "present a card...");

        snprintf(line, sizeof(line), "taps sent: %lu", (unsigned long)app->tap_count);
        canvas_draw_str(canvas, 2, 52, line);
    }
    canvas_draw_str(canvas, 2, 62, "Back to exit");

    furi_mutex_release(app->mutex);
}

static void input_callback(InputEvent* input_event, void* context) {
    App* app = context;

    if(input_event->type == InputTypeShort && input_event->key == InputKeyBack) {
        AppEvent ev = { .type = AppEventExit };
        furi_message_queue_put(app->queue, &ev, FuriWaitForever);
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static App* app_alloc(void) {
    App* app = malloc(sizeof(App));

    memset(app, 0, sizeof(App));
    app->queue = furi_message_queue_alloc(8, sizeof(AppEvent));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    /* USART is the header UART: pin 13 = TX, pin 14 = RX. */
    app->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(app->serial) {
        furi_hal_serial_init(app->serial, UART_BAUD);
        app->serial_ok = true;
    } else {
        FURI_LOG_E(TAG, "could not acquire USART - expansion modules enabled?");
    }

    app->nfc = nfc_alloc();
    app->poller = nfc_poller_alloc(app->nfc, NfcProtocolIso14443_3a);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    return app;
}

static void app_free(App* app) {
    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(app->view_port);

    nfc_poller_free(app->poller);
    nfc_free(app->nfc);

    if(app->serial_ok) {
        furi_hal_serial_deinit(app->serial);
        furi_hal_serial_control_release(app->serial);
    }

    furi_mutex_free(app->mutex);
    furi_message_queue_free(app->queue);
    free(app);
}

int32_t uid_stream_app(void* p) {
    UNUSED(p);

    App* app = app_alloc();
    char hex[UID_STR_LEN];
    char last_sent[UID_STR_LEN] = {0};
    uint32_t last_seen_ms = 0;
    uint32_t last_ping_ms = 0;
    bool running = true;

    uart_print(app, "RDY uid_stream 1\r\n");
    nfc_poller_start(app->poller, poller_callback, app);

    while(running) {
        AppEvent ev;
        const FuriStatus status = furi_message_queue_get(app->queue, &ev, 100);
        const uint32_t now_ms = furi_get_tick();

        if(status == FuriStatusOk) {
            if(ev.type == AppEventExit) {
                running = false;
                continue;
            }

            uid_to_hex(ev.uid, ev.uid_len, hex);
            last_seen_ms = now_ms;

            /* Only announce arrivals: a card resting on the Flipper keeps
             * producing Ready events, and the ESP32 does not want a flood. */
            if(strcmp(hex, last_sent) != 0) {
                uart_print(app, "CARD %s %02X %04X\r\n", hex, ev.sak, ev.atqa);
                strncpy(last_sent, hex, sizeof(last_sent) - 1);

                furi_mutex_acquire(app->mutex, FuriWaitForever);
                strncpy(app->last_uid, hex, sizeof(app->last_uid) - 1);
                app->card_present = true;
                app->tap_count++;
                furi_mutex_release(app->mutex);

                view_port_update(app->view_port);
            }
        }

        /* Card removed? */
        if(last_sent[0] != '\0' &&
           (now_ms - last_seen_ms) > furi_ms_to_ticks(CARD_GONE_TIMEOUT_MS)) {
            uart_print(app, "GONE\r\n");
            last_sent[0] = '\0';

            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->card_present = false;
            furi_mutex_release(app->mutex);

            view_port_update(app->view_port);
        }

        /* Heartbeat, so the ESP32 can tell "no taps" from "cable fell out". */
        if((now_ms - last_ping_ms) > furi_ms_to_ticks(PING_PERIOD_MS)) {
            last_ping_ms = now_ms;
            uart_print(app, "PING\r\n");
        }
    }

    nfc_poller_stop(app->poller);
    uart_print(app, "BYE\r\n");
    app_free(app);
    return 0;
}
