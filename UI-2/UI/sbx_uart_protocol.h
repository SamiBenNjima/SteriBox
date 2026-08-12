/**
 * @file sbx_uart_protocol.h
 * SteriBox — UART protocol between:
 *   MASTER = ESP32-S3 DevKit         (relays, buzzer, door/PIR, DHT22, RTC, fan)
 *   SLAVE  = ESP32-S3 CrowPanel 5"   (display + touch, runs the LVGL UI)
 *
 * IMPORTANT: keep this file byte-identical in both sketch folders
 * (UI-2/UI and SteriBox_S3_Master) — it is NOT a shared library on purpose,
 * to match the existing project convention.
 *
 * Wiring:
 *   Master  Serial1 : RX=18  TX=17    (ESP32-S3 DevKit)
 *   Slave   Serial1 : RX=44  TX=43    (CrowPanel — this board)
 *   TX(master,17) -> RX(slave,44)
 *   TX(slave,43)  -> RX(master,18)
 *   GND <-> GND
 *   Baud: 115200 8N1
 */
#ifndef SBX_UART_PROTOCOL_H
#define SBX_UART_PROTOCOL_H

#include <stdint.h>

#define SBX_UART_BAUD     115200

#define SBX_HDR_MASTER    0xAA   /* packet sent BY the master (telemetry) */
#define SBX_HDR_SLAVE     0xBB   /* packet sent BY the slave  (commands)  */

/* ---- message types --------------------------------------------------- */
enum {
    /* slave -> master (commands) */
    SBX_CMD_SET_RELAY   = 0x01,  /* data0=relay id (0/1), data1=on (0/1) */
    SBX_CMD_SET_BUZZER  = 0x02,  /* data0=pattern (sbx_beep_t)           */
    SBX_CMD_PING        = 0x03,
    SBX_CMD_PRINT       = 0x04,  /* data0..3=4 chars of text; zero-payload=end */
    SBX_CMD_SET_STATE   = 0x05,  /* data0=system state (sbx_state_t)     */

    /* --- file transfer to a USB mass-storage drive on the OTG port ---
     * A document is streamed as:
     *   FILE_OPEN x N   name chars, then FILE_OPEN with data0=0 to commit
     *   FILE_DATA x M   data0 = 1..3 payload bytes in data1..3
     *   FILE_CLOSE      data0 = 0 to commit, 1 to abort and delete
     * data0 doubles as the length so the transfer is binary-safe (a PDF
     * may legitimately contain 0x00). 3 payload bytes per 8-byte packet
     * is ~4.3 kB/s at 115200 baud - a one-page report lands in ~2 s. */
    SBX_CMD_FILE_OPEN   = 0x06,  /* data0=n chars in data1..3, 0 = commit name */
    SBX_CMD_FILE_DATA   = 0x07,  /* data0=n bytes (1..3) in data1..3           */
    SBX_CMD_FILE_CLOSE  = 0x08,  /* data0: 0=commit, 1=abort                   */

    /* master -> slave (telemetry/replies) */
    SBX_MSG_TELEMETRY   = 0x10,  /* data0=flags, data1..2=temp*10 (i16 LE), data3=hum% */
    SBX_MSG_PONG        = 0x11,
    SBX_MSG_FILE_ACK    = 0x12,  /* data0: 1=file written OK, 0=failed         */
};

/* telemetry flags bitfield (data[0]) */
#define SBX_FLAG_DOOR_OPEN      (1 << 0)
#define SBX_FLAG_RELAY1_ON      (1 << 1)
#define SBX_FLAG_RELAY2_ON      (1 << 2)
#define SBX_FLAG_ENV_VALID      (1 << 3)  /* DHT22 read OK this cycle */
#define SBX_FLAG_PRINTER_READY  (1 << 4)  /* USB-OTG printer detected */
#define SBX_FLAG_USB_DRIVE      (1 << 5)  /* USB-OTG mass-storage drive mounted */

typedef struct __attribute__((packed)) {
    uint8_t header;     /* SBX_HDR_MASTER or SBX_HDR_SLAVE */
    uint8_t type;       /* SBX_CMD_* or SBX_MSG_*          */
    uint8_t data[4];
    uint8_t checksum;   /* XOR of header,type,data[0..3]    */
} sbx_packet_t;

static inline uint8_t sbx_checksum(const sbx_packet_t *p) {
    uint8_t c = p->header ^ p->type;
    for (uint8_t i = 0; i < 4; i++) c ^= p->data[i];
    return c;
}

#endif /* SBX_UART_PROTOCOL_H */
