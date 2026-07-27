/**
 * @file steribox_uart.cpp
 * SteriBox - slave-side (ESP32-S3) UART link to the GPIO master.
 * See steribox_uart.h and sbx_uart_protocol.h for the protocol.
 */
#include "steribox_uart.h"
#include "sbx_uart_protocol.h"
#include <Arduino.h>
#include <string.h>

#define SLAVE_RX 44
#define SLAVE_TX 43

static uint8_t  s_relay_state  = 0;      /* bit0=relay1 bit1=relay2 */
static bool     s_door_open    = false;
static bool     s_env_valid    = false;
static float    s_temp         = 0.0f;
static float    s_hum          = 0.0f;
static uint32_t s_last_rx_ms   = 0;

static uint8_t  rxBuf[sizeof(sbx_packet_t)];
static uint8_t  rxIdx = 0;

void sbx_uart_init(void)
{
    Serial1.begin(SBX_UART_BAUD, SERIAL_8N1, SLAVE_RX, SLAVE_TX);
}

static void handle_packet(const sbx_packet_t *p)
{
    if (p->header != SBX_HDR_MASTER) return;
    if (sbx_checksum(p) != p->checksum) return;

    if (p->type == SBX_MSG_TELEMETRY) {
        uint8_t flags = p->data[0];
        int16_t traw;
        memcpy(&traw, &p->data[1], sizeof(int16_t));

        s_door_open   = flags & SBX_FLAG_DOOR_OPEN;
        s_relay_state = (flags >> 1) & 0x03;
        s_env_valid   = flags & SBX_FLAG_ENV_VALID;
        s_temp        = traw / 10.0f;
        s_hum         = p->data[3];
        s_last_rx_ms  = millis();
    }
}

void sbx_uart_task(void)
{
    while (Serial1.available()) {
        uint8_t b = Serial1.read();
        if (rxIdx == 0 && b != SBX_HDR_MASTER) continue; /* resync on header */
        rxBuf[rxIdx++] = b;
        if (rxIdx >= sizeof(sbx_packet_t)) {
            sbx_packet_t p;
            memcpy(&p, rxBuf, sizeof(p));
            handle_packet(&p);
            rxIdx = 0;
        }
    }
}

static void send_packet(sbx_packet_t *p)
{
    p->header   = SBX_HDR_SLAVE;
    p->checksum = sbx_checksum(p);
    Serial1.write((uint8_t*)p, sizeof(*p));
}

void sbx_uart_send_relay(uint8_t relay_id, bool on)
{
    sbx_packet_t p = {0};
    p.type    = SBX_CMD_SET_RELAY;
    p.data[0] = relay_id;
    p.data[1] = on ? 1 : 0;
    send_packet(&p);

    /* optimistic local update: instant UI feedback, corrected by the next
     * telemetry packet if the master disagrees (e.g. link was down) */
    if (on) s_relay_state |= (1 << relay_id);
    else    s_relay_state &= ~(1 << relay_id);
}

void sbx_uart_send_buzzer(uint8_t pattern)
{
    sbx_packet_t p = {0};
    p.type    = SBX_CMD_SET_BUZZER;
    p.data[0] = pattern;
    send_packet(&p);
}

bool sbx_uart_is_linked(void)    { return (millis() - s_last_rx_ms) < 2000; }
bool sbx_uart_get_door_open(void){ return s_door_open; }
bool sbx_uart_get_relay(uint8_t relay_id) { return s_relay_state & (1 << relay_id); }

bool sbx_uart_get_env(float *t, float *h)
{
    if (t) *t = s_temp;
    if (h) *h = s_hum;
    return s_env_valid;
}
