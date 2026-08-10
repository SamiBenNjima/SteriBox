# SteriBox — UV Sterilization System (Arduino/ESP32)

Medical-grade UV sterilizer controller built on two ESP32-S3 boards communicating over UART.

---

## System Overview

```
┌──────────────────────────────────┐       UART (115200 baud)       ┌──────────────────────────────┐
│   ESP32-S3 DevKit (Master)       │ ◄─────────────────────────────► │  ESP32-S3 CrowPanel 5"       │
│   SteriBox_S3_Master.ino         │  TX(17)→RX(44) / TX(43)→RX(18) │  UI-2/UI/UI.ino              │
│                                  │                                  │  + steribox_app.c (LVGL UI)  │
│  • 2× UV lamp relays (GPIO 1/2)  │                                  │   Home / Config / Info screens│
│  • Piezo buzzer  (GPIO 3, PWM)   │                                  │                              │
│  • Door / PIR sensor (GPIO 4)    │                                  │  • SD card logging           │
│  • DHT22 temp+hum (GPIO 5)       │                                  │  • NVS persistence           │
│  • Cooling fan   (GPIO 6)        │                                  │  • DS3231 RTC (slave-local)  │
│  • DS3231 RTC I2C (SDA=8 SCL=9) │                                  │                              │
│  • USB-OTG port  (GPIO 19/20)    │                                  │                              │
│    → USB printer / flash drive   │                                  │                              │
└──────────────────────────────────┘                                  └──────────────────────────────┘
```

### USB Ports — Master (ESP32-S3 DevKit)

| Port | Use |
|---|---|
| **USB-UART** (CH340 chip) | Arduino IDE upload · Serial Monitor · debug |
| **USB-OTG** (native S3) | USB devices — printer (CDC) · USB drive (MSC) |

> GPIO 19 / 20 are reserved for USB-OTG D−/D+. Do **not** wire anything else to them.

### Folders

| Path | Description |
|---|---|
| `UI-2/SteriBox_S3_Master/` | **New** ESP32-S3 DevKit master firmware |
| `UI-2/SteriBox_Master/` | *(Legacy — old ESP32-S master, kept for reference)* |
| `UI-2/UI/` | ESP32-S3 CrowPanel UI firmware (LVGL — Home / Config / Info screens) |

---

## Hardware Requirements

| Component | Model |
|---|---|
| Master MCU | ESP32-S3 DevKit |
| Display/Slave MCU | Elecrow CrowPanel 5" (ESP32-S3) |
| UV lamp relays | 2-channel relay module (active LOW) |
| Temperature sensor | DHT22 |
| Real-time clock | DS3231 (I²C) |
| Buzzer | Passive piezo buzzer |
| Sensor | Door switch or PIR sensor |
| USB devices | Printer (CDC) or USB drive (MSC) via OTG port |

### Wiring (UART link)

| Master (ESP32-S3 DevKit) | Slave (CrowPanel S3) |
|---|---|
| TX → GPIO 17 | RX → GPIO 44 |
| RX → GPIO 18 | TX → GPIO 43 |
| GND | GND |

### Master GPIO Map

| Function | GPIO | Notes |
|---|---|---|
| Relay 1 (UV lamp 1) | 1 | Active LOW |
| Relay 2 (UV lamp 2) | 2 | Active LOW |
| Buzzer | 3 | LEDC PWM 2700 Hz |
| Door / PIR sensor | 4 | HIGH = open / motion |
| DHT22 data | 5 | 10 kΩ pullup to 3.3 V |
| Cooling fan | 6 | Active HIGH |
| I²C SDA (DS3231) | 8 | 4.7 kΩ pullup |
| I²C SCL (DS3231) | 9 | 4.7 kΩ pullup |
| Onboard LED | 48 | RGB NeoPixel — blink on packet RX |
| USB-OTG D− | 19 | **Reserved** — OTG port |
| USB-OTG D+ | 20 | **Reserved** — OTG port |
| UART1 TX to slave | 17 | → CrowPanel RX44 |
| UART1 RX from slave | 18 | ← CrowPanel TX43 |

See `UI-2/WIRING_REFERENCE.txt` for full schematic guidance.

---

## Software Requirements

- [Arduino IDE 2.x](https://www.arduino.cc/en/software) or PlatformIO
- **ESP32 board package ≥ 3.0** — add to Board Manager URL:  
  `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
- Libraries (install via Library Manager):
  - `DHT sensor library` by Adafruit
  - `Adafruit Unified Sensor`
  - `LVGL` ≥ 8.x *(for the UI firmware)*

---

## Installation

### 1 — Clone the repository

```bash
git clone https://github.com/<your-username>/SteriBox.git
cd SteriBox/arduino
git checkout feature/esp32s3-master
```

### 2 — Install the ESP32 board package

1. Open Arduino IDE → **File → Preferences**
2. Paste the URL above into *Additional boards manager URLs*
3. Open **Tools → Board → Boards Manager**, search `esp32`, install **esp32 by Espressif ≥ 3.0**

### 3 — Flash the Master (ESP32-S3 DevKit)

1. Open `UI-2/SteriBox_S3_Master/SteriBox_S3_Master.ino`
2. Select **Board**: `ESP32S3 Dev Module`
3. Select **USB Mode**: `Hardware CDC and JTAG` (uses the CH340 debug port)
4. Select the correct COM port (the one that appears when you plug the **USB-UART** port)
5. Click **Upload**

### 4 — Flash the Slave UI (CrowPanel ESP32-S3)

1. Open `UI-2/UI/UI.ino`
2. Select **Board**: `ESP32S3 Dev Module` (or your CrowPanel variant)
3. Select the correct COM port
4. Click **Upload**

### 5 — Power up

Connect both boards, wire UART + GND (see wiring table above), then power on.
The CrowPanel will show the Home screen; the master begins sending telemetry
(temp / humidity / relay states) every 500 ms.

---

## UART Protocol

Defined in `sbx_uart_protocol.h` (kept byte-identical in both sketch folders).

| Field | Size | Notes |
|---|---|---|
| `header` | 1 byte | `0xAA` Master → Slave, `0xBB` Slave → Master |
| `type` | 1 byte | Message/command identifier |
| `data` | 4 bytes | Payload |
| `checksum` | 1 byte | XOR of all preceding bytes |

Key message types: `TELEMETRY`, `CMD_SET_RELAY`, `CMD_SET_BUZZER`, `CMD_PING` / `MSG_PONG`.

---

## USB-OTG (printer / flash drive)

The ESP32-S3 DevKit's native USB-OTG port (the second USB-C connector) is
reserved for USB devices. The current firmware includes a **stub**
(`SBX_USB_OTG_ENABLED = 0`). Full TinyUSB host MSC/CDC implementation is
planned for a follow-up branch. Enable the stub switch and add the
TinyUSB driver code when ready.

---

## License

MIT — see [LICENSE](LICENSE) if present, otherwise free to use and modify.
