# SteriBox — UV Sterilization System (Arduino/ESP32)

Medical-grade UV sterilizer controller built on two ESP32 boards communicating over UART.

---

## System Overview

```
┌─────────────────────────────┐        UART (115200 baud)       ┌──────────────────────────────┐
│   ESP32-S (Master)          │ ◄──────────────────────────────► │  ESP32-S3 CrowPanel 5"       │
│   SteriBox_Master.ino       │   TX(17)→RX(44) / TX(43)→RX(16) │  UI-2/UI/UI.ino              │
│                             │                                  │  + steribox_app.c (LVGL UI)  │
│  • 2× UV lamp relays        │                                  │   Home / Config / Info screens│
│  • Piezo buzzer (PWM)       │                                  │                              │
│  • DHT22 (temp + humidity)  │                                  │                              │
│  • DS3231 RTC (I²C)         │                                  │                              │
│  • Door / PIR sensor        │                                  │                              │
└─────────────────────────────┘                                  └──────────────────────────────┘
```

### Folders

| Path | Description |
|---|---|
| `master/` | Simple demo sketch (UART ping-pong, no sensors) |
| `slave/` | Simple demo sketch (UART reply, CrowPanel side) |
| `UI-2/SteriBox_Master/` | **Production** ESP32-S master firmware |
| `UI-2/UI/` | **Production** ESP32-S3 CrowPanel UI firmware (LVGL) |

> Use the `UI-2/` sketches for a real deployment.  
> The `master/` and `slave/` sketches are standalone communication demos only.

---

## Hardware Requirements

| Component | Model |
|---|---|
| Master MCU | ESP32-S DevKit |
| Display/Slave MCU | Elecrow CrowPanel 5" (ESP32-S3) |
| UV lamp relays | 2-channel relay module (active LOW) |
| Temperature sensor | DHT22 |
| Real-time clock | DS3231 (I²C) |
| Buzzer | Piezo buzzer |
| Sensor | Door or PIR sensor |

### Wiring (UART link)

| Master (ESP32-S) | Slave (CrowPanel S3) |
|---|---|
| TX → GPIO 17 | RX → GPIO 44 |
| RX → GPIO 16 | TX → GPIO 43 |
| GND | GND |

---

## Software Requirements

- [Arduino IDE 2.x](https://www.arduino.cc/en/software) or PlatformIO
- **ESP32 board package** — add to Board Manager URL:  
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
```

### 2 — Install the ESP32 board package

1. Open Arduino IDE → **File → Preferences**
2. Paste the URL above into *Additional boards manager URLs*
3. Open **Tools → Board → Boards Manager**, search `esp32`, install **esp32 by Espressif**

### 3 — Flash the Master (ESP32-S)

1. Open `UI-2/SteriBox_Master/SteriBox_Master.ino`
2. Select **Board**: `ESP32 Dev Module`
3. Select the correct COM port
4. Click **Upload**

### 4 — Flash the Slave UI (CrowPanel ESP32-S3)

1. Open `UI-2/UI/UI.ino`
2. Select **Board**: `ESP32S3 Dev Module` (or your CrowPanel variant)
3. Select the correct COM port
4. Click **Upload**

### 5 — Power up

Connect both boards, wire UART + GND, then power on. The CrowPanel will show the Home screen; the master will begin sending telemetry (temp / humidity / relay states) every 500 ms.

---

## UART Protocol

Defined in `UI-2/SteriBox_Master/sbx_uart_protocol.h` (shared with the UI firmware).

| Field | Size | Notes |
|---|---|---|
| `header` | 1 byte | `0xAA` Master → Slave, `0xBB` Slave → Master |
| `type` | 1 byte | Message/command identifier |
| `data` | 8 bytes | Payload |
| `checksum` | 1 byte | XOR of all preceding bytes |

Key message types: `TELEMETRY`, `CMD_SET_RELAY`, `CMD_SET_BUZZER`, `CMD_PING` / `MSG_PONG`.

---

## License

MIT — see [LICENSE](LICENSE) if present, otherwise free to use and modify.
