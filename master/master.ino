/*
 * ESP32-S UART Master (Simple Demo)
 * LED GPIO 2 : blink 1x = send | blink 2x = receive
 * Cycle      : toutes les 2 secondes
 * UART       : Serial2 (RX=16, TX=17)
 */

#define LED_PIN     2
#define MASTER_RX   16
#define MASTER_TX   17

#define HDR_MASTER  0xAA
#define HDR_SLAVE   0xBB

struct __attribute__((packed)) Packet {
  uint8_t header;
  uint8_t counter;
  uint8_t checksum;
};

uint8_t txCounter = 0;
uint32_t lastCycle = 0;

// ---------- LED helpers ----------
void blink(int times, int onMs = 80, int offMs = 80) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(onMs);
    digitalWrite(LED_PIN, LOW);
    if (i < times - 1) delay(offMs);
  }
}

uint8_t makeChecksum(uint8_t h, uint8_t c) {
  return h ^ c;
}

bool validatePacket(Packet* p, uint8_t expectedHeader) {
  if (p->header != expectedHeader) return false;
  return (p->checksum == makeChecksum(p->header, p->counter));
}

// ---------- UART send ----------
void sendPacket(Packet* p) {
  p->checksum = makeChecksum(p->header, p->counter);
  Serial2.write((uint8_t*)p, sizeof(Packet));
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.begin(115200);   // USB debug
  Serial2.begin(115200, SERIAL_8N1, MASTER_RX, MASTER_TX);

  delay(500);
  Serial.println("=== Master ready ===");
  Serial.println("LED: 1 blink = TX | 2 blinks = RX");
}

void loop() {
  if (millis() - lastCycle >= 2000) {
    lastCycle = millis();

    // ----- BUILD & SEND -----
    Packet tx;
    tx.header  = HDR_MASTER;
    tx.counter = txCounter++;

    blink(1);                       // LED : 1 blink = SEND
    sendPacket(&tx);
    Serial.printf("\n[TX] counter=%d\n", tx.counter);

    // ----- WAIT REPLY (timeout 800 ms) -----
    unsigned long t0 = millis();
    bool gotIt = false;

    while (millis() - t0 < 800) {
      if (Serial2.available() >= sizeof(Packet)) {
        Packet rx;
        Serial2.readBytes((uint8_t*)&rx, sizeof(Packet));

        if (validatePacket(&rx, HDR_SLAVE) && rx.counter == tx.counter) {
          blink(2);                 // LED : 2 blinks = RECEIVE
          Serial.printf("[RX] ACK counter=%d\n", rx.counter);
          gotIt = true;
          break;
        }
      }
    }

    if (!gotIt) {
      Serial.println("[RX] TIMEOUT (check wiring GND + TX/RX crossed)");
    }
  }
}