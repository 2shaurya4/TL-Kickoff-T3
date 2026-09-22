# Wiring guide — proximity lamp

Board: **ESP32-S3-DevKitC-1**. Pin names below are the silkscreen labels on the
board (`5V`, `GND`, `IO5`, `IO6`, `IO7`). This matches
`KiCad/TL Kickoff T3.kicad_sch` and the `#define`s in the firmware.

## Parts

| Qty | Part |
|-----|------|
| 1 | ESP32-S3-DevKitC-1 |
| 1 | HC-SR04 ultrasonic sensor |
| 1 | WS2812B / NeoPixel strip (firmware assumes 8 pixels) |
| 1 | 1 kΩ resistor |
| 1 | 2 kΩ resistor |
| 1 | 470 Ω resistor |
| 1 | 1000 µF electrolytic capacitor, 6.3 V or higher |

## Connection list

Every connection, one row each. Nothing else gets wired.

### Power

| From | To |
|------|----|
| DevKitC `5V` | HC-SR04 `VCC` |
| DevKitC `5V` | NeoPixel `5V` / `VCC` / `+5V` |
| DevKitC `GND` | HC-SR04 `GND` |
| DevKitC `GND` | NeoPixel `GND` |

### HC-SR04 trigger — direct

| From | To |
|------|----|
| DevKitC `IO6` | HC-SR04 `TRIG` |

### HC-SR04 echo — through the divider (do NOT connect ECHO straight to the board)

ECHO swings to 5 V. The ESP32-S3 pins are 3.3 V only. Three wires, one junction:

| From | To |
|------|----|
| HC-SR04 `ECHO` | one leg of the **1 kΩ** resistor |
| other leg of the **1 kΩ** | junction point **J** |
| junction **J** | one leg of the **2 kΩ** resistor |
| other leg of the **2 kΩ** | DevKitC `GND` |
| junction **J** | DevKitC `IO5` |

Junction **J** sits at 5 V × 2 kΩ / (1 kΩ + 2 kΩ) = **3.3 V**. That is the point
that touches `IO5`.

### NeoPixel data

| From | To |
|------|----|
| DevKitC `IO7` | one leg of the **470 Ω** resistor |
| other leg of the **470 Ω** | NeoPixel `DIN` (the input end of the strip — check the arrows) |

### Capacitor

| From | To |
|------|----|
| 1000 µF **+** leg (long leg) | NeoPixel `5V` |
| 1000 µF **−** leg (stripe side) | NeoPixel `GND` |

Put it physically at the strip, not at the board.

## ASCII summary

```
  ESP32-S3-DevKitC-1                        HC-SR04
  ┌──────────────┐                        ┌──────────┐
  │          5V  ├────────────────────────┤ VCC      │
  │          IO6 ├────────────────────────┤ TRIG     │
  │                                       │          │
  │          IO5 ├──────┬───[1k]──────────┤ ECHO     │
  │                     │                 │          │
  │                   [2k]                │          │
  │                     │                 │          │
  │          GND ├──────┴─────────────────┤ GND      │
  └──────┬───┬───┘                        └──────────┘
         │   │
         │   │            NeoPixel strip (input end)
         │   │           ┌──────────────┐
    IO7 ─┼───┼──[470R]───┤ DIN          │
     5V ─┤   └───────────┤ 5V      ──┐  │
    GND ─┴───────────────┤ GND     ──┴─ 1000uF across 5V/GND
                         └──────────────┘
```

## Three things that will bite you

1. **Don't skip the ECHO divider.** 5 V straight into `IO5` damages the chip.

2. **3.3 V data into a 5 V strip is out of spec.** WS2812B wants a logic high of
   0.7 × 5 V = 3.5 V; the ESP32-S3 gives 3.3 V. It very often works, and it is
   fine for a bench draft. If the first pixel flickers or shows wrong colours,
   pick one fix:
   - a 74AHCT125 level shifter between `IO7` and `DIN`, powered from 5 V, **or**
   - a 1N4001 diode in series with the strip's 5 V, dropping it to ~4.3 V, which
     lowers the strip's logic threshold to ~3.0 V.

3. **Watch the current.** A WS2812B pulls ~60 mA at full white, so 8 pixels can
   ask for ~0.5 A — more than a USB port is happy to give through the DevKitC's
   5 V pin. The firmware ships at brightness 40/255 with an amber colour, which
   lands around 30 mA total. If you raise `NEO_DEFAULT_BRIGHTNESS` or add
   pixels, feed the strip from its own 5 V supply and **tie that supply's GND to
   the DevKitC GND** — the data line needs a shared reference.

## Powering the strip from its own 5 V supply

Thirty-six pixels at full white is roughly 2.2 A, which no USB port will
deliver through the board's 5 V pin. Once you go past a bench test, give the
strip its own supply. The data line does not care where the strip's power
comes from, so the ESP32 still drives it exactly as before.

| From | To |
|------|----|
| PSU **+** | strip `5V` |
| PSU **-** | strip `GND` |
| PSU **-** | ESP32 `GND` |
| ESP32 `IO7` -> 470 ohm | strip `DIN` |
| 1000 uF **+** / **-** | strip `5V` / `GND` |

```
   5V DC supply
   +----------+
   |      (+) |----------------------+----------> strip 5V
   |          |                      |
   |          |                   [1000uF]
   |          |                      |
   |      (-) |----------+-----------+----------> strip GND
   +----------+          |
                         |  <- the critical wire
   ESP32 (USB powered)   |
   +----------+          |
   |      GND |----------+
   |      IO7 |---[470R]-------------------------> strip DIN
   +----------+
```

Two rules:

1. **Tie the grounds together.** A data line is a voltage measured against
   ground. With two separate supplies and no shared reference the strip sees
   noise rather than data and stays dark. This is the most common reason an
   externally powered strip does nothing.

2. **Do not feed the PSU into `5VIN` while USB is connected.** Power the board
   from one source. Keep the ESP32 on USB, the strip on the PSU, and join only
   the grounds.

## If the strip stays dark

Work down this list; each step rules out one cause.

1. **Is the strip actually getting 5 V?** A WS2812B needs at least ~3.5 V and
   realistically 4 V to light at all, so a strip wired to `3V3` will often show
   nothing whatsoever. A dark strip on 3.3 V is not evidence of a data fault.

2. **Count the pads.** Three (`5V`/`DIN`/`GND`) is a WS2812B and this driver
   is correct. Four, with a `CI` or `CLK` pad, is an APA102 / DotStar, which
   needs a separate clock line this driver does not generate.

3. **Check the voltage marking.** A strip marked `12V` is a WS2811 with LEDs
   in groups of three and will never light from 5 V.

4. **Confirm which end is the input.** The arrows printed on the strip point
   away from `DIN`. Feeding the output end does nothing at all.

5. **Then, and only then, suspect the logic level.** See the level-shifting
   note above.

## Build and flash

```sh
idf.py set-target esp32s3
idf.py build flash monitor
```

The `espressif/led_strip` dependency in `main/idf_component.yml` downloads
automatically on the first build.

At boot the strip flashes amber once for 300 ms. That is the self-test — if you
don't see it, the problem is the strip wiring, not the sensor.
