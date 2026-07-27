/**
 * SteriBox_Master.ino
 * ESP32-S DevKit - GPIO controller for the SteriBox UV sterilizer.
 *
 * Owns everything the ESP32-S3 CrowPanel has no free pins left for:
 *   RELAY1/RELAY2  UV lamp ballasts   (active LOW - pull to GND to energize)
 *   BUZZER         piezo buzzer       (LEDC PWM, ~2.7 kHz)
 *   DOOR / PIR     door or presence sensor (HIGH = open/motion)
 *   DHT22          temperature + humidity (Adafruit DHT library)
 *   RTC (DS3231)   real-time clock via I2C (sync ESP32 system clock at boot)
 *
 * Talks to the CrowPanel (display/UI) over UART2 <-> its UART1:
 *   here   Serial2 : RX=16 TX=17
 *   slave  Serial1 : RX=44 TX=43
 *   Wire TX(here)->RX(slave), TX(slave)->RX(here), GND<->GND.
 *
 * Protocol: see sbx_uart_protocol.h (keep identical to the copy in UI-2/UI).
 *
 * Pin map - change here if your wiring differs, nothing else to touch:
 */
#define PIN_RELAY1   25   /* active LOW: HIGH = OFF, LOW = ON */
#define PIN_RELAY2   26
#define PIN_BUZZER   27
#define PIN_DOOR     34   /* input-only pin, fine for a sensor */
#define PIN_DHT      33
#define LED_PIN       2   /* onboard LED: blinks on each valid packet in */

#define MASTER_RX    16
#define MASTER_TX    17

/* I2C pins for RTC (DS3231) */
#define RTC_SDA      21   /* I2C data (configurable, e.g., 21, 22, etc.) */
#define RTC_SCL      22   /* I2C clock (configurable) */
#define RTC_ADDR     0x68 /* DS3231 I2C address (standard: 0x68) */

#include <Arduino.h>
#include <Wire.h>
#include <time.h>
#include <sys/time.h>
#include <DHT.h>
#include <string.h>
#include "sbx_uart_protocol.h"

#define BUZZER_CHANNEL   0
#define BUZZER_FREQ_HZ   2700

DHT dht(PIN_DHT, DHT22);

static bool     relay_state[2] = { false, false };
static float    last_temp = 0, last_hum = 0;
static bool     env_valid = false;
static uint32_t lastDhtRead   = 0;
static uint32_t lastTelemetry = 0;

static uint8_t  rxBuf[sizeof(sbx_packet_t)];
static uint8_t  rxIdx = 0;

/* ---------- helpers ---------- */
void blink(int times, int onMs = 40, int offMs = 60) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(onMs);
    digitalWrite(LED_PIN, LOW);
    if (i < times - 1) delay(offMs);
  }
}

void send_packet(sbx_packet_t *p) {
  p->header   = SBX_HDR_MASTER;
  p->checksum = sbx_checksum(p);
  Serial2.write((uint8_t*)p, sizeof(*p));
  // Debug: log major packets (not every telemetry, just commands from slave)
  // Serial.printf("[TX] type=0x%02X checksum=0x%02X\n", p->type, p->checksum);
}

void send_telemetry() {
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

/* Blocking buzzer patterns: short and rare, fine to block the main loop
 * for ~0.1-0.5s. LEDC PWM control via ledcWriteTone().
 * All patterns use BUZZER_FREQ_HZ (~2700 Hz) unless noted.
 */
void apply_buzzer(uint8_t pattern) {
  switch (pattern) {
    case 0:   /* SBX_BEEP_KEY: single short click for UI feedback */
      ledcWriteTone(BUZZER_CHANNEL, BUZZER_FREQ_HZ);
      delay(60);
      ledcWriteTone(BUZZER_CHANNEL, 0);
      Serial.println("[BUZZER] KEY");
      break;
      
    case 1:   /* SBX_BEEP_OK: double beep (action confirmed) */
      for (int i = 0; i < 2; i++) {
        ledcWriteTone(BUZZER_CHANNEL, BUZZER_FREQ_HZ);
        delay(80);
        ledcWriteTone(BUZZER_CHANNEL, 0);
        delay(60);
      }
      Serial.println("[BUZZER] OK");
      break;
      
    case 2:   /* SBX_BEEP_WARN: long low beep (refused action) */
      ledcWriteTone(BUZZER_CHANNEL, BUZZER_FREQ_HZ / 2);  // lower pitch
      delay(300);
      ledcWriteTone(BUZZER_CHANNEL, 0);
      Serial.println("[BUZZER] WARN");
      break;
      
    case 3:   /* SBX_BEEP_DONE: triple beep (cycle finished) */
      for (int i = 0; i < 3; i++) {
        ledcWriteTone(BUZZER_CHANNEL, BUZZER_FREQ_HZ);
        delay(100);
        ledcWriteTone(BUZZER_CHANNEL, 0);
        delay(80);
      }
      Serial.println("[BUZZER] DONE");
      break;
      
    case 4:   /* SBX_BEEP_ALARM: rapid beeping (safety abort) */
      for (int i = 0; i < 6; i++) {
        ledcWriteTone(BUZZER_CHANNEL, BUZZER_FREQ_HZ);
        delay(100);
        ledcWriteTone(BUZZER_CHANNEL, 0);
        delay(80);
      }
      Serial.println("[BUZZER] ALARM");
      break;
      
    default:
      Serial.printf("[BUZZER] UNKNOWN pattern=%d\n", pattern);
      break;
  }
}

void handle_packet(const sbx_packet_t *p) {
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
        // Active LOW: on=true -> pull to GND (LOW), on=false -> release (HIGH)
        uint8_t pin = (id == 0) ? PIN_RELAY1 : PIN_RELAY2;
        digitalWrite(pin, on ? LOW : HIGH);
        Serial.printf("  [RELAY%d] %s\n", id + 1, on ? "ON (grounded)" : "OFF (released)");
      }
      break;
    }
    case SBX_CMD_SET_BUZZER: {
      Serial.printf("  [BUZZER] pattern=%d\n", p->data[0]);
      apply_buzzer(p->data[0]);
      break;
    }
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

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  
  // Relays: active LOW (HIGH = OFF, LOW = ON)
  pinMode(PIN_RELAY1, OUTPUT); 
  digitalWrite(PIN_RELAY1, HIGH);  // start OFF (not grounded)
  pinMode(PIN_RELAY2, OUTPUT); 
  digitalWrite(PIN_RELAY2, HIGH);  // start OFF (not grounded)
  
  pinMode(PIN_DOOR, INPUT);

  // Buzzer: LEDC PWM setup (channel 0, 2.7kHz, 10-bit resolution)
  ledcSetup(BUZZER_CHANNEL, BUZZER_FREQ_HZ, 10);
  ledcAttachPin(PIN_BUZZER, BUZZER_CHANNEL);
  ledcWriteTone(BUZZER_CHANNEL, 0);  // start silent

  // Serial
  Serial.begin(115200);
  delay(100);
  Serial.println("\n========================================");
  Serial.println("=== SteriBox Master Boot ===");
  Serial.println("========================================");
  
  // UART2 to slave (CrowPanel)
  Serial2.begin(SBX_UART_BAUD, SERIAL_8N1, MASTER_RX, MASTER_TX);
  Serial.printf("[INIT] UART2 started RX=%d TX=%d @ %u baud\n", 
                MASTER_RX, MASTER_TX, SBX_UART_BAUD);
  
  // DHT22
  dht.begin();
  delay(100);
  Serial.printf("[INIT] DHT22 on GPIO %d\n", PIN_DHT);
  
  Serial.println("[INIT] Hardware ready:");
  Serial.println("  Relay1 (25) = active LOW");
  Serial.println("  Relay2 (26) = active LOW");
  Serial.println("  Buzzer (27) = LEDC PWM @ 2700 Hz");
  Serial.println("  Door/PIR (34) = input");
  Serial.println("  DHT22 (33) = OneWire");
  Serial.println("[INIT] Waiting for slave (CrowPanel) link...\n");
}

void loop() {
  // ---- receive commands from the slave (non-blocking) ----
  while (Serial2.available()) {
    uint8_t b = Serial2.read();
    if (rxIdx == 0 && b != SBX_HDR_SLAVE) continue;  // resync on header byte
    rxBuf[rxIdx++] = b;
    if (rxIdx >= sizeof(sbx_packet_t)) {
      sbx_packet_t p;
      memcpy(&p, rxBuf, sizeof(p));
      handle_packet(&p);
      rxIdx = 0;
    }
  }

  // ---- DHT22 (min ~2s between reads; poor reading cadence = high failure rate) ----
  if (millis() - lastDhtRead >= 2000) {
    lastDhtRead = millis();
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    
    if (!isnan(t) && !isnan(h)) { 
      last_temp = t; 
      last_hum = h; 
      env_valid = true;
      Serial.printf("[DHT22] OK: %.1f°C, %.1f%% RH\n", t, h);
    } else {
      env_valid = false;
      Serial.println("[DHT22] FAIL (no data or CRC error)");
    }
  }

  // ---- telemetry heartbeat every 500ms ----
  if (millis() - lastTelemetry >= 500) {
    lastTelemetry = millis();
    send_telemetry();
    
    // debug: print state every 2s (4 telemetry cycles)
    static uint32_t lastDebug = 0;
    if (millis() - lastDebug >= 2000) {
      lastDebug = millis();
      uint8_t flags = 0;
      if (digitalRead(PIN_DOOR) == HIGH) flags |= SBX_FLAG_DOOR_OPEN;
      if (relay_state[0])                flags |= SBX_FLAG_RELAY1_ON;
      if (relay_state[1])                flags |= SBX_FLAG_RELAY2_ON;
      if (env_valid)                     flags |= SBX_FLAG_ENV_VALID;
      
      Serial.printf("[STATE] Door=%s R1=%s R2=%s Temp=%.1f°C Hum=%.0f%% Valid=%d\n",
                    (flags & SBX_FLAG_DOOR_OPEN) ? "OPEN" : "closed",
                    (flags & SBX_FLAG_RELAY1_ON)  ? "ON" : "off",
                    (flags & SBX_FLAG_RELAY2_ON)  ? "ON" : "off",
                    last_temp, last_hum, env_valid);
    }
  }
}
