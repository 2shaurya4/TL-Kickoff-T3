# Flipper Zero as the RFID reader

The Flipper is a handheld with its own firmware, not a module the ESP32 can
drive. So the split is: **the Flipper reads cards and reports what it sees;
the ESP32 listens and acts.** They talk over the Flipper's GPIO UART.

```
   card ))))  [ Flipper Zero ]  --- pin 13 (TX) ---->  RX  [ ESP32-S3 ]
                UID Stream app        GND ------------  GND    rfid_flipper
                                  115200 8N1, ASCII lines
```

```
flipper/
  app/                     Flipper Zero app (built with ufbt, runs on the Flipper)
    application.fam
    uid_stream.c
  esp32/rfid_flipper/      ESP-IDF component (built with the rest of the firmware)
    rfid_flipper.h
    rfid_flipper.c
```

The ESP32 half is registered through `EXTRA_COMPONENT_DIRS` in the project's
top-level `CMakeLists.txt`, so it builds automatically and `main` can include
`rfid_flipper.h` without any change to `main/CMakeLists.txt`.

## Wiring

Both sides are 3.3 V, so the pins connect directly - no level shifting.

| Flipper pin | Signal   | ESP32-S3            | Notes                                   |
|-------------|----------|---------------------|-----------------------------------------|
| 13          | USART TX | `RFID_UART_RX_GPIO` (GPIO17, placeholder) | the one that matters |
| 14          | USART RX | `RFID_UART_TX_GPIO` (GPIO18, placeholder) | optional, unused today |
| 11 or 18    | GND      | GND                 | **required** - no common ground, no link |
| 9           | +3.3 V   | (leave alone)       | see below                               |

Powering the ESP32 from the Flipper's 3V3 pin is possible (the rail is good
for about 1.2 A), but the LED strip alone can exceed that, so keep the board
on its own USB supply and share only ground.

## Line protocol

ASCII, CRLF-terminated, 115200 8N1. The ESP32 ignores lines it does not
recognise, so the protocol can grow without breaking an older build.

| Line                      | Meaning                                    |
|---------------------------|--------------------------------------------|
| `RDY uid_stream 1`        | app started (version 1)                    |
| `CARD <uid> <sak> <atqa>` | card arrived, e.g. `CARD 04A23B1C 08 0004` |
| `GONE`                    | card left the field                        |
| `PING`                    | heartbeat, every 2 s                       |
| `BYE`                     | app exiting                                |

`<uid>` is 8, 14 or 20 hex digits (4-, 7- or 10-byte UIDs). `CARD` is sent
once per arrival, not once per poll, so a card left resting on the Flipper
does not flood the link. `sak` and `atqa` are optional - a `CARD <uid>` line
alone parses fine.

The heartbeat is what makes `rfid_link_ok()` meaningful: with the reader
being a separate handheld that can be switched off, walk away or drop into
another app, "no taps" and "no reader" are different states and the firmware
should be able to tell them apart.

## Building the Flipper app

```
pip install --upgrade ufbt      # once
cd flipper/app
ufbt                            # builds uid_stream.fap
ufbt launch                     # builds, uploads over USB and runs it
```

`ufbt` pulls the SDK matching the firmware on the connected Flipper. If the
app fails to build after a firmware update, that is the usual cause - see
"If it does not compile" below.

**Before running it: Settings -> Expansion Modules -> off.** That service
holds the USART for detecting add-on boards, and while it does,
`furi_hal_serial_control_acquire()` returns NULL and the app says "UART
busy!" on screen.

The app must be running and the Flipper awake for taps to reach the ESP32.
That is the real cost of this approach versus a soldered-down reader.

## Using it from the firmware

```c
#include "rfid_flipper.h"

ESP_ERROR_CHECK(rfid_init());          /* succeeds even with no Flipper attached */

rfid_uid_t uid;
if (rfid_read_new_uid(&uid, 0) == ESP_OK) {     /* 0 = poll, don't block */
    char str[32];
    rfid_uid_to_str(&uid, str, sizeof(str));
    ESP_LOGI("app", "tap: %s (%s)", str,
             rfid_card_type_str(rfid_card_type(uid.sak)));
}

if (!rfid_link_ok()) {
    /* Flipper is off, asleep, or the app is not running. */
}
```

Also available: `rfid_card_present()`, `rfid_get_last_uid()`, `rfid_flush()`,
`rfid_uid_equal()`, `rfid_deinit()`.

## If it does not compile

The app is written against the post-rework NFC API (`nfc_poller_*`,
`Iso14443_3aPollerEvent`) and the `furi_hal_serial` API. Both have moved
before. The likely spots, in order:

1. `data->sak` / `data->atqa[]` - if `Iso14443_3aData` became opaque, swap in
   `iso14443_3a_get_sak(data)` and `iso14443_3a_get_atqa(data)`.
2. `nfc_poller_get_data()` - check the signature in
   `lib/nfc/nfc_poller.h` of the SDK `ufbt` downloaded.
3. `furi_hal_serial_control_acquire(FuriHalSerialIdUsart)` - older firmware
   used `furi_hal_uart_init(FuriHalUartIdUSART1, baud)` instead.

`ufbt` keeps the SDK under `~/.ufbt/current/` - the headers there are the
authoritative reference for whatever firmware is on the Flipper.

## Testing without a Flipper

The ESP32 half only cares about the text. A USB-serial adapter on the same
pin works:

```
echo -e 'CARD 04A23B1C 08 0004\r' > /dev/ttyUSB0
```

That should produce a tap event, exactly as a real card would.
