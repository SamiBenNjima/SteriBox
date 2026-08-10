/**
 * SteriBox_S3_Master.ino
 * ESP32-S3 DevKit — GPIO controller for the SteriBox UV sterilizer.
 *
 * UPGRADE from ESP32-S:  This board replaces the old ESP32-S DevKit master.
 * The slave (CrowPanel ESP32-S3 5") remains UNCHANGED — same UART protocol,
 * same RX=44/TX=43 pins on the slave side.
 *
 * What this board owns:
 *   RELAY1/RELAY2  UV lamp ballasts   (active LOW — pull to GND to energize)
 *   BUZZER         piezo buzzer       (LEDC PWM, ~2.7 kHz)
 *   DOOR / PIR     door or presence sensor (HIGH = open / motion)
 *   DHT22          temperature + humidity (Adafruit DHT library)
 *   RTC (DS3231)   real-time clock via I2C  (sync ESP32 system clock at boot)
 *   FAN            cooling fan (HIGH = ON when temp > 26 °C)
 *
 * USB ports (ESP32-S3 DevKit has two USB-C):
 *   USB-UART (CH340)  → GPIO 43/44 UART0 → Arduino Serial Monitor / flashing
 *   USB-OTG (native)  → GPIO 19/20 D+/D-  → USB devices (printer, USB drive)
 *     IMPORTANT: do NOT wire anything to GPIO 19 / 20 — they are reserved for
 *     the USB-OTG peripheral.  The OTG port is brought up as a USB-Host CDC /
 *     MSC stub in this firmware (full driver in a future sprint).
 *
 * UART link to the CrowPanel slave:
 *   This board  Serial1 : RX=18  TX=17
 *   Slave board Serial1 : RX=44  TX=43
 *   Wire: TX(here,17) → RX(slave,44), TX(slave,43) → RX(here,18), GND↔GND
 *   Baud: 115200 8N1
 *
 * I2C (DS3231 RTC):
 *   SDA = GPIO 8     SCL = GPIO 9
 *
 * Pin map — change only here, nothing else to touch:
 */

/* ---- GPIO assignments ---- */
#define PIN_RELAY1   1    /* active LOW: HIGH = OFF, LOW = ON  */
#define PIN_RELAY2   2
#define PIN_BUZZER   3
#define PIN_DOOR     4    /* digital input — no INPUT_PULLDOWN needed if external pull */
#define PIN_DHT      5
#define PIN_FAN      6    /* active HIGH — HIGH = fan ON when temp > 26 °C */
#define LED_PIN     48    /* ESP32-S3 DevKit onboard RGB NeoPixel (WS2812) — used as
                           * plain digital blink; LOW = off, HIGH = faint white */

#define MASTER_RX   18
#define MASTER_TX   17

/* I2C for DS3231 RTC */
#define RTC_SDA      8
#define RTC_SCL      9
#define RTC_ADDR  0x68

/* ---- LEDC (buzzer PWM) ---- */
/* Arduino-ESP32 ≥ 3.x uses ledcAttach() / ledcWriteTone().
 * For older 2.x boards package uncomment the #define below:
 * #define SBX_LEDC_LEGACY   */
#define BUZZER_CHANNEL   0
#define BUZZER_FREQ_HZ   2700

/* ---- USB-OTG stub switch ---- */
/* Set to 1 once TinyUSB host + MSC / CDC drivers are wired in.
 * At 0 the USB-OTG pins are left untouched by this firmware.       */
#define SBX_USB_OTG_ENABLED  0

/* ------------------------------------------------------------------ */
#include <Arduino.h>
#include <Wire.h>
#include <time.h>
#include <sys/time.h>
#include <DHT.h>
#include <string.h>
#include "sbx_uart_protocol.h"

#if SBX_USB_OTG_ENABLED
/* TinyUSB host — requires ESP32 Arduino core ≥ 3.0 with TinyUSB selected
 * in Tools → USB Mode → "TinyUSB" and a USB Host shield / MAX3421E.      */
// #include <Adafruit_TinyUSB.h>
// TODO: full USB-Host MSC / CDC implementation in follow-up sprint.
#endif

DHT dht(PIN_DHT, DHT22);

static bool     relay_state[2] = { false, false };
static float    last_temp = 0, last_hum = 0;
static bool     env_valid = false;
static uint32_t lastDhtRead   = 0;
static uint32_t lastTelemetry = 0;

static uint8_t  rxBuf[sizeof(sbx_packet_t)];
static uint8_t  rxIdx = 0;

/* ================================================================
 * LEDC helpers — compatible with both ESP32 Arduino 2.x and 3.x
 * ================================================================ */
static void buzzer_init(void)
{
#ifdef SBX_LEDC_LEGACY
    ledcSetup(BUZZER_CHANNEL, BUZZER_FREQ_HZ, 10);
    ledcAttachPin(PIN_BUZZER, BUZZER_CHANNEL);
    ledcWriteTone(BUZZER_CHANNEL, 0);
#else
    /* Arduino-ESP32 3.x: ledcAttach replaces ledcSetup + ledcAttachPin */
    ledcAttachChannel(PIN_BUZZER, BUZZER_FREQ_HZ, 10, BUZZER_CHANNEL);
    ledcWriteTone(PIN_BUZZER, 0);
#endif
}

static void buzzer_tone(uint32_t freq)
{
#ifdef SBX_LEDC_LEGACY
    ledcWriteTone(BUZZER_CHANNEL, freq);
#else
    ledcWriteTone(PIN_BUZZER, freq);
#endif
}

/* ================================================================
 * Helpers
 * ================================================================ */
void blink(int times, int onMs = 40, int offMs = 60)
{
    for (int i = 0; i < times; i++) {
        digitalWrite(LED_PIN, HIGH);
        delay(onMs);
        digitalWrite(LED_PIN, LOW);
        if (i < times - 1) delay(offMs);
    }
}

void send_packet(sbx_packet_t *p)
{
    p->header   = SBX_HDR_MASTER;
    p->checksum = sbx_checksum(p);
    Serial1.write((uint8_t*)p, sizeof(*p));
}

void send_telemetry(void)
{
    sbx_packet_t p = {0};
    p.type = SBX_MSG_TELEMETRY;

    uint8_t flags = 0;
    if (digitalRead(PIN_DOOR) == HIGH) flags |= SBX_FLAG_DOOR_OPEN;
    if (relay_state[0])                flags |= SBX_FLAG_RELAY1_ON;
    if (relay_state[1])                flags |= SBX_FLAG_RELAY2_ON;
    if (env_valid)                     flags |= SBX_FLAG_ENV_VALID;
    p.data[0] = flags;

    int16_t traw = (int16_t)(last_temp * 10.0f);
    memcpy(&p.data[1], &traw, sizeof(int16_t));
    p.data[3] = (uint8_t)constrain((int)last_hum, 0, 100);

    send_packet(&p);
}

/* ================================================================
 * Buzzer patterns
 * ================================================================ */
void apply_buzzer(uint8_t pattern)
{
    switch (pattern) {
        case 0: /* SBX_BEEP_KEY: single short click */
            buzzer_tone(BUZZER_FREQ_HZ);
            delay(60);
            buzzer_tone(0);
            Serial.println("[BUZZER] KEY");
            break;

        case 1: /* SBX_BEEP_OK: double beep */
            for (int i = 0; i < 2; i++) {
                buzzer_tone(BUZZER_FREQ_HZ);
                delay(80);
                buzzer_tone(0);
                delay(60);
            }
            Serial.println("[BUZZER] OK");
            break;

        case 2: /* SBX_BEEP_WARN: long low beep */
            buzzer_tone(BUZZER_FREQ_HZ / 2);
            delay(300);
            buzzer_tone(0);
            Serial.println("[BUZZER] WARN");
            break;

        case 3: /* SBX_BEEP_DONE: triple beep */
            for (int i = 0; i < 3; i++) {
                buzzer_tone(BUZZER_FREQ_HZ);
                delay(100);
                buzzer_tone(0);
                delay(80);
            }
            Serial.println("[BUZZER] DONE");
            break;

        case 4: /* SBX_BEEP_ALARM: rapid repeated beeps */
            for (int i = 0; i < 6; i++) {
                buzzer_tone(BUZZER_FREQ_HZ);
                delay(100);
                buzzer_tone(0);
                delay(80);
            }
            Serial.println("[BUZZER] ALARM");
            break;

        default:
            Serial.printf("[BUZZER] UNKNOWN pattern=%d\n", pattern);
            break;
    }
}

/* ================================================================
 * Packet handler (commands from the slave CrowPanel)
 * ================================================================ */
void handle_packet(const sbx_packet_t *p)
{
    if (p->header != SBX_HDR_SLAVE) return;
    if (sbx_checksum(p) != p->checksum) {
        Serial.println("[RX] checksum FAIL");
        return;
    }
    blink(1);
    Serial.printf("[RX] type=0x%02X (checksum OK)\n", p->type);

    switch (p->type) {
        case SBX_CMD_SET_RELAY: {
            uint8_t id = p->data[0];
            bool    on = p->data[1] != 0;
            if (id < 2) {
                relay_state[id] = on;
                uint8_t pin = (id == 0) ? PIN_RELAY1 : PIN_RELAY2;
                digitalWrite(pin, on ? LOW : HIGH);   /* active LOW relay */
                Serial.printf("  [RELAY%d] %s\n", id + 1,
                              on ? "ON (grounded)" : "OFF (released)");
            }
            break;
        }
        case SBX_CMD_SET_BUZZER:
            Serial.printf("  [BUZZER] pattern=%d\n", p->data[0]);
            apply_buzzer(p->data[0]);
            break;

        case SBX_CMD_PING: {
            Serial.println("  [PING] -> sending PONG");
            sbx_packet_t pong = {0};
            pong.type = SBX_MSG_PONG;
            send_packet(&pong);
            break;
        }
        default:
            Serial.printf("  [UNKNOWN] type=0x%02X\n", p->type);
            break;
    }
}

/* ================================================================
 * DS3231 RTC — minimal I2C driver (no extra library needed)
 * ================================================================ */
static uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
static uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

static bool rtc_read(struct tm *t)
{
    Wire.beginTransmission(RTC_ADDR);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) return false;
    if (Wire.requestFrom(RTC_ADDR, 7) != 7) return false;

    uint8_t s  = Wire.read();
    uint8_t mi = Wire.read();
    uint8_t hr = Wire.read();
    Wire.read(); /* day-of-week, unused */
    uint8_t d  = Wire.read();
    uint8_t mo = Wire.read();
    uint8_t yr = Wire.read();

    if (s == 0xFF && mi == 0xFF && hr == 0xFF) return false; /* floating bus */

    t->tm_sec  = bcd2dec(s  & 0x7F);
    t->tm_min  = bcd2dec(mi & 0x7F);
    t->tm_hour = bcd2dec(hr & 0x3F);
    t->tm_mday = bcd2dec(d  & 0x3F);
    t->tm_mon  = bcd2dec(mo & 0x1F) - 1;
    t->tm_year = bcd2dec(yr) + 100;
    t->tm_isdst = -1;
    return true;
}

static void rtc_init(void)
{
    Wire.begin(RTC_SDA, RTC_SCL);
    Wire.setClock(100000);
    Wire.setTimeOut(50);
    delay(100);

    Wire.beginTransmission(RTC_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.println("[RTC] DS3231 not found on I2C");
        return;
    }

    struct tm t;
    if (!rtc_read(&t)) {
        Serial.println("[RTC] read failed");
        return;
    }
    time_t epoch = mktime(&t);
    if (epoch == (time_t)-1) return;
    struct timeval tv = { epoch, 0 };
    settimeofday(&tv, NULL);
    Serial.printf("[RTC] System clock set: %04d-%02d-%02d %02d:%02d\n",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
}

/* ================================================================
 * USB-OTG stub
 * ================================================================ */
#if SBX_USB_OTG_ENABLED
static void usb_host_task(void)
{
    /* Placeholder — full TinyUSB host implementation (MSC + CDC) here.
     * GPIO 19 = D-, GPIO 20 = D+  (do not use for anything else)       */
}
#endif

/* ================================================================
 * setup()
 * ================================================================ */
void setup(void)
{
    /* Onboard LED */
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    /* Relays — active LOW (HIGH = OFF at startup) */
    pinMode(PIN_RELAY1, OUTPUT); digitalWrite(PIN_RELAY1, HIGH);
    pinMode(PIN_RELAY2, OUTPUT); digitalWrite(PIN_RELAY2, HIGH);

    /* Cooling fan — active HIGH */
    pinMode(PIN_FAN, OUTPUT); digitalWrite(PIN_FAN, LOW);

    /* Door / PIR sensor */
    pinMode(PIN_DOOR, INPUT);

    /* Buzzer LEDC */
    buzzer_init();

    /* USB-UART serial (CH340 / debug port) */
    Serial.begin(115200);
    delay(200);
    Serial.println("\n========================================");
    Serial.println("=== SteriBox S3 Master Boot ===");
    Serial.println("========================================");
    Serial.printf("[INIT] Board: ESP32-S3 DevKit — firmware built %s %s\n",
                  __DATE__, __TIME__);

    /* I2C + DS3231 RTC */
    rtc_init();

    /* UART1 link to the CrowPanel slave */
    Serial1.begin(SBX_UART_BAUD, SERIAL_8N1, MASTER_RX, MASTER_TX);
    Serial.printf("[INIT] UART1 slave link: RX=%d TX=%d @ %u baud\n",
                  MASTER_RX, MASTER_TX, SBX_UART_BAUD);

    /* DHT22 */
    dht.begin();
    delay(100);
    Serial.printf("[INIT] DHT22 on GPIO %d\n", PIN_DHT);

    Serial.println("[INIT] Hardware ready:");
    Serial.printf("  Relay1  GPIO %d  (active LOW)\n",  PIN_RELAY1);
    Serial.printf("  Relay2  GPIO %d  (active LOW)\n",  PIN_RELAY2);
    Serial.printf("  Fan     GPIO %d  (active HIGH)\n", PIN_FAN);
    Serial.printf("  Buzzer  GPIO %d  (LEDC %d Hz)\n",  PIN_BUZZER, BUZZER_FREQ_HZ);
    Serial.printf("  Door    GPIO %d  (input)\n",       PIN_DOOR);
    Serial.printf("  DHT22   GPIO %d\n",                PIN_DHT);
    Serial.printf("  I2C RTC SDA=%d SCL=%d\n",         RTC_SDA, RTC_SCL);
    Serial.println("  USB-OTG GPIO 19/20 (D-/D+) — reserved, do not connect");
#if SBX_USB_OTG_ENABLED
    Serial.println("  [USB-OTG] Host stub ENABLED");
#else
    Serial.println("  [USB-OTG] stub DISABLED (enable SBX_USB_OTG_ENABLED when wired)");
#endif
    Serial.println("[INIT] Waiting for slave (CrowPanel) link...\n");
}

/* ================================================================
 * loop()
 * ================================================================ */
void loop(void)
{
    /* ---- receive commands from the slave (non-blocking) ---- */
    while (Serial1.available()) {
        uint8_t b = Serial1.read();
        if (rxIdx == 0 && b != SBX_HDR_SLAVE) continue; /* resync on header */
        rxBuf[rxIdx++] = b;
        if (rxIdx >= sizeof(sbx_packet_t)) {
            sbx_packet_t p;
            memcpy(&p, rxBuf, sizeof(p));
            handle_packet(&p);
            rxIdx = 0;
        }
    }

    /* ---- DHT22 (min ~2 s between reads) ---- */
    if (millis() - lastDhtRead >= 2000) {
        lastDhtRead = millis();
        float t = dht.readTemperature();
        float h = dht.readHumidity();
        if (!isnan(t) && !isnan(h)) {
            last_temp = t;
            last_hum  = h;
            env_valid = true;
            Serial.printf("[DHT22] OK: %.1f°C, %.1f%% RH\n", t, h);

            /* Fan control */
            if (t > 26.0f) {
                digitalWrite(PIN_FAN, HIGH);
                Serial.println("  [FAN] ON (temp > 26.0°C)");
            } else {
                digitalWrite(PIN_FAN, LOW);
                Serial.println("  [FAN] OFF (temp <= 26.0°C)");
            }
        } else {
            env_valid = false;
            Serial.println("[DHT22] FAIL (no data or CRC error)");
        }
    }

    /* ---- telemetry heartbeat every 500 ms ---- */
    if (millis() - lastTelemetry >= 500) {
        lastTelemetry = millis();
        send_telemetry();

        /* debug state dump every 2 s */
        static uint32_t lastDebug = 0;
        if (millis() - lastDebug >= 2000) {
            lastDebug = millis();
            Serial.printf("[STATE] Door=%s R1=%s R2=%s Temp=%.1f°C Hum=%.0f%% Valid=%d\n",
                          (digitalRead(PIN_DOOR) == HIGH) ? "OPEN" : "closed",
                          relay_state[0] ? "ON" : "off",
                          relay_state[1] ? "ON" : "off",
                          last_temp, last_hum, env_valid);
        }
    }

#if SBX_USB_OTG_ENABLED
    usb_host_task();
#endif
}
