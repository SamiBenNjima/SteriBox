/*
 * ESP32-S3 CrowPanel 5" UART Slave (Debug only, no LED)
 * 
 * Serial   = USB CDC (moniteur serie Arduino IDE) -> DEBUG
 * Serial1  = UART1 hardware sur GPIO 44(RX) / 43(TX) -> Liaison Master
 */

#define SLAVE_RX  44
#define SLAVE_TX  43

#define HDR_MASTER  0xAA
#define HDR_SLAVE   0xBB

struct __attribute__((packed)) Packet {
  uint8_t header;
  uint8_t counter;
  uint8_t checksum;
};

uint8_t makeChecksum(uint8_t h, uint8_t c) {
  return h ^ c;
}

bool validatePacket(Packet* p, uint8_t expectedHeader) {
  if (p->header != expectedHeader) return false;
  return (p->checksum == makeChecksum(p->header, p->counter));
}

void sendPacket(Packet* p) {
  p->checksum = makeChecksum(p->header, p->counter);
  Serial1.write((uint8_t*)p, sizeof(Packet));
}

// ---------- UART parsing ----------
uint8_t rxBuf[3];
int rxState = 0;

void setup() {
  // USB CDC debug
  Serial.begin(115200);
  while (!Serial && millis() < 3000) { ; }
  Serial.println("\n========================================");
  Serial.println("=== ESP32-S3 Slave Boot ===");
  Serial.println("========================================");

  // UART1 sur pins 44/43
  Serial1.begin(115200, SERIAL_8N1, SLAVE_RX, SLAVE_TX);
  Serial.println("[INIT] UART1 started on RX=44 TX=43");
  Serial.println("[INIT] Boot complete. Waiting for Master packets...\n");
}

void loop() {
  while (Serial1.available()) {
    uint8_t b = Serial1.read();

    Serial.printf("[DEBUG] Raw byte: 0x%02X | state=%d\n", b, rxState);

    switch (rxState) {
      case 0:
        if (b == HDR_MASTER) {
          rxBuf[0] = b;
          rxState = 1;
          Serial.println("[DEBUG] -> Header 0xAA OK, waiting counter");
        } else {
          Serial.printf("[DEBUG] -> Unexpected 0x%02X (expected 0x%02X), resync\n", b, HDR_MASTER);
        }
        break;

      case 1:
        rxBuf[1] = b;
        rxState = 2;
        Serial.printf("[DEBUG] -> Counter byte: %d, waiting checksum\n", b);
        break;

      case 2:
        rxBuf[2] = b;
        {
          Packet rx;
          rx.header   = rxBuf[0];
          rx.counter  = rxBuf[1];
          rx.checksum = rxBuf[2];

          uint8_t expectedCS = makeChecksum(rx.header, rx.counter);
          Serial.printf("[DEBUG] -> Packet: H=0x%02X C=%d CS=0x%02X (expected=0x%02X)\n",
                        rx.header, rx.counter, rx.checksum, expectedCS);

          if (validatePacket(&rx, HDR_MASTER)) {
            Serial.printf("[RX] VALID packet from Master | counter=%d\n", rx.counter);

            Packet tx;
            tx.header  = HDR_SLAVE;
            tx.counter = rx.counter;

            sendPacket(&tx);
            Serial.printf("[TX] ACK sent back to Master | counter=%d\n\n", tx.counter);

          } else {
            Serial.println("[DEBUG] CHECKSUM MISMATCH -> packet dropped");
          }
        }
        rxState = 0;
        break;
    }
  }
}