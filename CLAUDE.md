# Solar Powered Chicken Coop – project context

Coop sensor/control node: Raspberry Pi Pico (RP2040) reads sensors and talks JSON
over UART to a RAK4631 running Meshtastic. A Heltec V3 Meshtastic gateway bridges
to Home Assistant via MQTT (Mosquitto on HA).

## Hardware
- Controller: Raspberry Pi **Pico (RP2040)** – boots as RPI-RP2. PlatformIO env `pico`
  (`default_envs = pico`; `pico2` env kept for a future RP2350 swap). Earle Philhower Arduino core.
- Full pin/wiring tables (current and planned): README.md – keep it in sync when wiring changes.
- Coop radio: RAK4631 on RAK19007 base + RAK13002 IO adapter. Meshtastic node `!b5754eb8`
  (decimal 3044363960). Role CLIENT_MUTE, rebroadcast LOCAL_ONLY.
- Gateway: Heltec WiFi LoRa 32 V3 (HF, 915 MHz) "Gateway Node"/GWND, `!9e9d93b0`
  (decimal 2661127088). WiFi + MQTT, JSON enabled.
- Probes: 2x DS18B20 on GP10 (pin 14), one 4.7k pull-up to 3V3.
  - Water: {0x28,0xFF,0xB6,0xAB,0x03,0x15,0x02,0xED}
  - Air:   {0x28,0xFF,0x4B,0x77,0x03,0x15,0x02,0xB4}
  (Confirmed 2026-10-15 by warming the probe labeled water; matches WATER_ADDR/AIR_ADDR in src/main.cpp.)
- OLED: 1.3" SH1106 128x64 at 0x3C on I2C0, GP4 SDA / GP5 SCL (Adafruit SH110X + GFX). NOT an SSD1306:
  that driver gives a noise-filled screen with the text wrapped/shifted.
  On 60 s at boot and after each report, then DISPLAYOFF. Probed at boot; absent OLED = skipped.
  Boot log prints every I2C address found ("I2C devices on GP4/GP5: 0x3C"); falls back to 0x3D.
  Verified working 2026-10-25. An empty scan with GP4/GP5 idling high meant SDA/SCL were swapped.

## Wiring Pico <-> RAK13002
- Pico GP0 (UART TX) -> RAK RXD1; Pico GP1 (UART RX) <- RAK TXD1; GND common.
- RAK serial module: enabled, TEXTMSG, 38400, RX 15, TX 16, echo off.
- RAK Position -> GPS mode **Not_Present** (otherwise Meshtastic GPS probing takes the UART).

## Meshtastic / MQTT
- Primary channel `Coop` (private key), US, LONG_FAST. Frequency slot left at 0 (default) on both
  nodes, so it is derived from the channel name: slot 55, 915.625 MHz. Both nodes must match.
  Hop limit: Heltec 2, RAK 3.
- Heltec has secondary channel `mqtt` with downlink on (required for JSON downlink).
- Uplink topic: `msh/US/2/json/Coop/!9e9d93b0` – coop messages have `"type":"text"`,
  `"from":3044363960`; payload is the Pico JSON (object or string under payload.text).
- Send to coop: publish to `msh/US/2/json/mqtt/` with
  `{"from":2661127088,"channel":0,"type":"sendtext","payload":"<text>"}` (add `"to":3044363960` for a DM).

## Pico message protocol (src/main.cpp)
- Report: `{"fr":"coop1","t":"env","wt":<water C>,"at":<air C>,"r":"boot|timer|alert|req"}`
  every 10 min, immediately when water crosses 2 C (max one alert per 5 min).
- Boot report adds `"rs"`: last reset cause (power, run_pin, soft, wdt, debug, glitch, brownout, unknown).
  `wdt` means the watchdog recovered a hang. A firmware upload also shows as `soft`.
- Command: `{"to":"coop1","c":"status"}` -> immediate report. Enter in USB serial monitor does the same.
- Command: `{"to":"coop1","c":"reboot"}` -> Pico restarts. Typing `reboot` in USB serial monitor does the same.
- Hardware watchdog (8 s, started after the 10 s USB wait in setup) resets the Pico if loop() stalls.
- Temperatures in Celsius on the wire; Home Assistant converts for display.

## Home Assistant
- Trigger-based template sensors `sensor.coop_water_temperature`, `sensor.coop_air_temperature`
  from the MQTT topic above (filter type == text and from == 3044363960).
- Alerts planned: water < 2 C for 10 min; no reading for 40 min.

## Gotchas
- VS Code Auto Save overwrote files edited outside the editor – close a file before external edits.
- With both boards on USB, the RAK enumerates as "WiFi/WisCore RAK4631"; the Pico is the other usbmodem port.
- Boards powered from the Mac stop when the Mac sleeps – use a USB wall charger for long runs.
- Heltec V3 misdetects USB power ("PowerFSM: Loss of power" at boot). With the default
  `power.on_battery_shutdown_after_secs` = 60 it deep-sleeps 60 s after every boot (screen shows
  "Resuming" on button wake). Set to 31536000 on 2026-10-16; 0 does NOT disable it (reverts to 60).
- Heltec Wi-Fi API (192.168.1.175:4403) drops connections (low heap): the meshtastic CLI's `--set`
  can print "Writing…" without the change landing. Hold the connection a few seconds after
  `writeConfig()` and confirm with a read-back. Opening the Heltec's USB port resets it.
- If the Pico is unpowered, RAK TX leaks back into RAK RX: commands come back over the mesh as
  coop text like "GWND: {...}".

## Plan / future pins (see build plan doc)
Door names: door A = **Hen house**, door B = **Coop run**. Use these names (not A/B) on the control-box
labels, the OLED and Home Assistant messages (e.g. "Coop run door opened locally").
Planned control panel: 2 IP67 momentary door buttons on the outside under the OLED, 2 tare buttons
inside the box. No screen/page button. Each button: one leg to an MCP23017 input, other leg to GND,
internal pull-ups on (pressed = 0); avoid GPA7/GPB7 (output-only on newer datasheets).
- GPA0 HEN HOUSE, GPA1 COOP RUN (outside): if the screen is dark, a press only wakes it; if lit, a
  press opens/closes that door (ignored while moving). Panel text: "Press once to wake, again to move".
- GPA2 TARE FEEDER, GPA3 TARE WATER (inside, PCB-mounted tactile switches on the carrier board,
  silkscreen-labeled "hold 3 s"): hold 3 s; release early cancels.
  The screen can't be seen from inside, so the result stays on the OLED for 60 s ("Feeder zeroed" /
  "Tare failed") and the status LED (GP13) blinks: 3 short = OK, 1 long = failed.
- All status must fit one OLED page (no paging).
- Every local action is reported to HA. Must work with the gateway down. An HA tare button is an
  optional extra only.
MCP23017: VDD 3V3, VSS GND, A0-A2 GND (0x20), RESET to 3V3, INTA -> GP22, 100 nF at VDD.
GP2/GP3 door A contacts, GP6/GP7 door B, GP4/GP5 I2C shared with the OLED (MCP23017 0x20,
INA3221 0x40, SHT41 0x44), GP13 status LED, GP14/GP15 IR LEDs (doors A/B, commercial IR-remote doors),
GP16 IR receiver, GP17 light PWM (inverted, via 2N3904 -> IRF520), GP18-21 HX711 feeder/waterer,
GP22 MCP23017 INTA, GP26 heater clamp, GP27 12V battery divider (100k/22k).
KiCad carrier board (`coop_node`, socketed Pico): NOT started yet (no files exist as of 2026-10-18).
Per the build plan it is Phase 3 (weeks 9–12), drawn only from circuits proven on the breadboard.
Build plan doc: https://claude.ai/code/artifact/8fcd9386-4051-48c5-9cdd-96f92910ac93
