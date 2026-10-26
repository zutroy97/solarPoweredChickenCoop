# Solar Powered Chicken Coop – Wiring and Pin Map

Coop sensor node: a **Raspberry Pi Pico (RP2040)** reads the temperature probes and
sends JSON over UART to a **RAK4631** (on a RAK19007 base with a RAK13002 IO adapter)
running Meshtastic. A **Heltec WiFi LoRa 32 V3** gateway relays the mesh to Home
Assistant over Wi-Fi/MQTT.

```
DS18B20 probes ──1-Wire──► Pico (+ OLED on I2C) ──UART 38400──► RAK4631 ~~LoRa 915 MHz~~► Heltec V3 ──Wi-Fi/MQTT──► Home Assistant
```

> **Pico pin numbers** below are the physical pins (1–40, counted counter-clockwise
> from pin 1 next to the USB connector). **GPxx** is the GPIO number used in code.

---

## 1. Pico ↔ RAK4631 (UART link) — wired

| Signal | Pico GPIO | Pico pin | → / ← | RAK13002 terminal | RAK4631 (nRF52) | Notes |
|---|---|---|---|---|---|---|
| Pico TX → RAK RX | GP0 (`Serial1` TX) | 1 | → | **RXD1** | P0.15 (Meshtastic "RX pin 15") | Pico sends JSON reports |
| Pico RX ← RAK TX | GP1 (`Serial1` RX) | 2 | ← | **TXD1** | P0.16 (Meshtastic "TX pin 16") | RAK forwards mesh text (`GWND: {...}`) |
| Ground | GND | 3 (any GND) | — | **GND** | GND | Common ground is required |

Both sides are 3.3 V logic, so they connect directly with no level shifter.

**RAK Meshtastic serial module settings:** enabled, mode `TEXTMSG`, 38400 baud,
RX 15, TX 16, echo off. **Position → GPS mode = `Not_Present`**, or Meshtastic's GPS
probing takes over this UART.

---

## 2. Pico ↔ DS18B20 temperature probes — wired

Both probes share one 1-Wire bus.

| Probe wire | Connects to | Pico pin | Notes |
|---|---|---|---|
| Data (yellow or white) | GP10 | 14 | Both probes on the same pin |
| Red (+) | 3V3(OUT) | 36 | |
| Black (−) | GND | 38 (any GND) | |
| — | **4.7 kΩ pull-up** between GP10 and 3V3 | 14 ↔ 36 | One resistor for the whole bus |

| Probe | 1-Wire address (in `src/main.cpp`) |
|---|---|
| **Water** | `{0x28, 0xFF, 0xB6, 0xAB, 0x03, 0x15, 0x02, 0xED}` |
| **Air** | `{0x28, 0xFF, 0x4B, 0x77, 0x03, 0x15, 0x02, 0xB4}` |

Addresses were confirmed on 2026-10-15 by warming the probe labelled *water*.

---

## 2a. Pico ↔ OLED display (I2C) — firmware ready, being wired

1.3" **SH1106 128×64** module (looks like an SSD1306, but the SSD1306 driver shows garbage on it) at I2C address `0x3C`. It wakes for 60 s at boot and after
every report (water/air in °C with °F, report reason, uptime), then blanks to save power
and avoid burn-in. Without the OLED connected, the firmware runs as before.

| OLED pin | Connects to | Pico pin | Notes |
|---|---|---|---|
| SDA | GP4 (I2C0 SDA) | 6 | Shared I2C bus (see planned devices below) |
| SCL | GP5 (I2C0 SCL) | 7 | |
| VCC | 3V3(OUT) | 36 | |
| GND | GND | 8 (any GND) | |

The module has its own I2C pull-ups, so no extra resistors are needed.

---

## 3. Pico power and on-board pins

| Function | Pico GPIO / pin | Pin # | Notes |
|---|---|---|---|
| USB power / programming | Micro-USB | — | Use a USB wall charger at the coop. If it's powered from a Mac, it stops when the Mac sleeps. |
| On-board LED | GP25 (`LED_BUILTIN`) | — (on board) | Blinks for 100 ms on each report sent |
| Reset | RUN | 30 | Pull to GND to reset. Not wired yet; see the proposed RAK-controlled reset below. |
| 3.3 V out | 3V3(OUT) | 36 | Powers the probes |

---

## 4. Planned connections — **not wired yet**

These pins are reserved in the build plan. The current firmware does not use them.
A KiCad carrier board (`coop_node`) for these circuits is planned but not started yet.

| Function | Pico GPIO | Pico pin(s) | Connects to | Notes |
|---|---|---|---|---|
| Door A contacts | GP2, GP3 | 4, 5 | Door A position switches | |
| I2C bus (SDA, SCL) | GP4, GP5 | 6, 7 | MCP23017 `0x20`, INA3221 `0x40`, SHT41 `0x44` | Shared with the OLED (section 2a) |
| Door B contacts | GP6, GP7 | 9, 10 | Door B position switches | |
| Status LED | GP13 | 17 | External LED | |
| IR LED, door A | GP14 | 19 | IR emitter aimed at door A | Drives a commercial IR-remote door |
| IR LED, door B | GP15 | 20 | IR emitter aimed at door B | |
| IR receiver | GP16 | 21 | IR receiver module | For learning the remote codes |
| Light PWM | GP17 | 22 | 2N3904 → IRF520 MOSFET | **Inverted** logic |
| HX711 feeder / waterer | GP18–GP21 | 24, 25, 26, 27 | Two HX711 load-cell amps | DOUT/SCK pairs |
| MCP23017 interrupt | GP22 | 29 | MCP23017 INTA | |
| Heater current clamp | GP26 (ADC0) | 31 | Current clamp | Analog input |
| 12 V battery voltage | GP27 (ADC1) | 32 | 100 kΩ / 22 kΩ divider | 12 V reads as ~2.16 V at the pin |

### Proposed: RAK-controlled Pico reset (not wired)

This would let the RAK hard-reset a frozen Pico. Use a transistor so that a
rebooting RAK can never hold the Pico in reset.

| From | Through | To |
|---|---|---|
| A free RAK13002 IO pin (not IO2) | 10 kΩ → 2N3904 base (100 kΩ base → GND) | — |
| 2N3904 collector | — | Pico **RUN** (pin 30) |
| 2N3904 emitter | — | GND |

RAK pin high → Pico resets. Low or floating → Pico runs.

---

## 5. Radio and network (no wires)

| Item | RAK4631 (coop) | Heltec V3 (gateway) |
|---|---|---|
| Meshtastic node ID | `!b5754eb8` (3044363960) | `!9e9d93b0` (2661127088), short name `GWND` |
| Role | `CLIENT_MUTE`, rebroadcast `LOCAL_ONLY` | `CLIENT` |
| Region / preset | US / LONG_FAST | US / LONG_FAST |
| Frequency | Slot 0 (default) → **915.625 MHz** | Slot 0 (default) → **915.625 MHz** — must match |
| Channel 0 | `Coop` (private key) | `Coop`, uplink + downlink on |
| Channel 1 | — | `mqtt`, downlink on |
| Network | — | Wi-Fi `192.168.1.175`, MQTT to `mqtt.home.lan:1883`, JSON on |
| Power notes | — | `power.on_battery_shutdown_after_secs = 31536000` (setting 0 does **not** disable it) |

MQTT topics:

| Direction | Topic | Payload |
|---|---|---|
| Coop → HA | `msh/US/2/json/Coop/!9e9d93b0` | Pico JSON, e.g. `{"fr":"coop1","t":"env","wt":20.5,"at":20.6,"r":"timer"}` |
| HA → coop | `msh/US/2/json/mqtt/` | `{"from":2661127088,"channel":0,"type":"sendtext","payload":"{\"to\":\"coop1\",\"c\":\"status\"}"}` |

Commands: `"c":"status"` sends a reading now; `"c":"reboot"` restarts the Pico.

---

## Build note

The board is an **original Pico (RP2040)**, and `platformio.ini` defaults to the
`pico` environment, so a plain build and upload targets it:

```bash
pio run -t upload
```

The `pico2` environment (Pico 2 / RP2350) is kept for a future board swap:
`pio run -e pico2 -t upload`.
