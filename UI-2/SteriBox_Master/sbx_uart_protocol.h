/**
 * @file sbx_uart_protocol.h
 * SteriBox - UART protocol between:
 *   MASTER = ESP32-S DevKit         (relays, buzzer, door/PIR, DHT22)
 *   SLAVE  = ESP32-S3 CrowPanel 5"  (display + touch, runs the LVGL UI)
 *
 * IMPORTANT: keep this file byte-identical in both sketch folders
 * (UI-2/UI and SteriBox_Master) - it is NOT a shared library on purpose,
 * to match the existing project convention (see master.ino / slave.ino demo).
 *
 * Wiring:
 *   Master  Serial2 : RX=16  TX=17
 *   Slave   Serial1 : RX=44  TX=43
 *   TX(master) -> RX(slave), TX(slave) -> RX(master), GND <-> GND
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

    /* master -> slave (telemetry/replies) */
    SBX_MSG_TELEMETRY   = 0x10,  /* data0=flags, data1..2=temp*10 (i16 LE), data3=hum% */
    SBX_MSG_PONG        = 0x11,
};

/* telemetry flags bitfield (data[0]) */
#define SBX_FLAG_DOOR_OPEN    (1 << 0)
#define SBX_FLAG_RELAY1_ON    (1 << 1)
#define SBX_FLAG_RELAY2_ON    (1 << 2)
#define SBX_FLAG_ENV_VALID    (1 << 3)  /* DHT22 read OK this cycle */

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
