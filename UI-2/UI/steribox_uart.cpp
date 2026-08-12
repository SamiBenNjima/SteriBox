/**
 * @file steribox_uart.cpp
 * SteriBox — slave-side (ESP32-S3 CrowPanel) UART link to the GPIO master.
 *
 * Master board: ESP32-S3 DevKit  Serial1 RX=18 TX=17
 * Slave  board: CrowPanel S3 5"  Serial1 RX=44 TX=43  (THIS file)
 *
 * See steribox_uart.h and sbx_uart_protocol.h for the protocol details.
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
static bool     s_printer_ready = false;  /* USB-OTG printer present on master */
static bool     s_usb_drive    = false;   /* USB-OTG mass storage mounted      */
static float    s_temp         = 0.0f;
static float    s_hum          = 0.0f;
static uint32_t s_last_rx_ms   = 0;

/* File-transfer state (one document at a time) */
static bool     s_file_open     = false;
static bool     s_file_ack_seen = false;
static bool     s_file_ack_ok   = false;

static uint8_t  rxBuf[sizeof(sbx_packet_t)];
static uint8_t  rxIdx = 0;
static bool     s_started = false;   /* false when the link is compiled out */

void sbx_uart_init(void)
{
    Serial1.begin(SBX_UART_BAUD, SERIAL_8N1, SLAVE_RX, SLAVE_TX);
    s_started = true;
}

static void handle_packet(const sbx_packet_t *p)
{
    if (p->header != SBX_HDR_MASTER) return;
    if (sbx_checksum(p) != p->checksum) return;

    if (p->type == SBX_MSG_TELEMETRY) {
        uint8_t flags = p->data[0];
        int16_t traw;
        memcpy(&traw, &p->data[1], sizeof(int16_t));

        s_door_open    = flags & SBX_FLAG_DOOR_OPEN;
        s_relay_state  = (flags >> 1) & 0x03;
        s_env_valid    = flags & SBX_FLAG_ENV_VALID;
        s_printer_ready = flags & SBX_FLAG_PRINTER_READY;
        s_usb_drive    = flags & SBX_FLAG_USB_DRIVE;
        s_temp         = traw / 10.0f;
        s_hum          = p->data[3];
        s_last_rx_ms   = millis();
    }
    else if (p->type == SBX_MSG_FILE_ACK) {
        s_file_ack_seen = true;
        s_file_ack_ok   = (p->data[0] != 0);
    }
}

void sbx_uart_task(void)
{
    if (!s_started) return;
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
    if (!s_started) return;   /* link compiled out: never touch GPIO 43/44 */
    p->header   = SBX_HDR_SLAVE;
    p->checksum = sbx_checksum(p);
    Serial1.write((uint8_t*)p, sizeof(*p));
}

/* Serial1.write() blocks once the TX FIFO fills, which is what paces a
 * long stream. Between bursts, pump the RX side and hand the CPU back:
 * delay(1) blocks on the scheduler (unlike yield()), which is what
 * actually lets the idle task run and keeps its watchdog quiet across a
 * transfer of several thousand packets. */
static void stream_yield(void)
{
    sbx_uart_task();
    delay(1);
}

/* Packets sent since the current stream started. Module scope on purpose:
 * a document arrives as many small sbx_uart_file_write() calls, so a
 * per-call counter would almost never reach the yield threshold. */
static uint32_t s_stream_pkts = 0;

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

void sbx_uart_send_state(uint8_t state)
{
    sbx_packet_t p = {0};
    p.type    = SBX_CMD_SET_STATE;
    p.data[0] = state;
    send_packet(&p);
}

/* Send a text document to the master for printing on the USB-OTG printer.
 * Text is streamed in 4-byte payloads; a zero-payload packet terminates the
 * document and is what makes the master flush to the printer — it must be
 * sent even when the final chunk was shorter than 4 bytes.
 * SBX_CMD_PRINT is defined as 0x04 in sbx_uart_protocol.h.           */
void sbx_uart_send_print_text(const char * text)
{
    if (!text || !s_started) return;
    const uint8_t * p = (const uint8_t *)text;
    s_stream_pkts = 0;
    while (*p) {
        sbx_packet_t pkt = {0};
        pkt.type = SBX_CMD_PRINT;
        uint8_t n = 0;
        while (n < 4 && *p) { pkt.data[n++] = *p++; }
        send_packet(&pkt);
        if ((++s_stream_pkts & 0x3F) == 0) stream_yield();
    }
    /* end-of-document marker (all four data bytes zero) */
    sbx_packet_t end = {0};
    end.type = SBX_CMD_PRINT;
    send_packet(&end);
}

/*==================================================================
 * File transfer to a USB drive on the master's OTG port
 *
 * Streamed in 3-byte length-prefixed chunks so the payload is binary
 * safe (a PDF contains 0x00). Each packet carries 3 of 8 bytes, so the
 * effective rate at 115200 baud is ~4.3 kB/s.
 *=================================================================*/

bool sbx_uart_file_open(const char * filename)
{
    if (!s_started || !filename || !*filename) return false;
    if (s_file_open) sbx_uart_file_close(false);  /* never leave one dangling */

    s_file_ack_seen = false;
    s_file_ack_ok   = false;
    s_stream_pkts   = 0;

    const uint8_t * n = (const uint8_t *)filename;
    while (*n) {
        sbx_packet_t pkt = {0};
        pkt.type = SBX_CMD_FILE_OPEN;
        uint8_t k = 0;
        while (k < 3 && *n) { pkt.data[1 + k] = *n++; k++; }
        pkt.data[0] = k;
        send_packet(&pkt);
        stream_yield();
    }
    /* data0 = 0 commits the name and opens the file on the master */
    sbx_packet_t commit = {0};
    commit.type = SBX_CMD_FILE_OPEN;
    send_packet(&commit);

    s_file_open = true;
    return true;
}

bool sbx_uart_file_write(const void * data, uint32_t len)
{
    if (!s_file_open) return false;
    if (!data || len == 0) return true;

    const uint8_t * b = (const uint8_t *)data;
    uint32_t sent = 0;
    while (sent < len) {
        sbx_packet_t pkt = {0};
        pkt.type = SBX_CMD_FILE_DATA;
        uint8_t k = 0;
        while (k < 3 && sent < len) { pkt.data[1 + k] = b[sent++]; k++; }
        pkt.data[0] = k;
        send_packet(&pkt);

        if ((++s_stream_pkts & 0x3F) == 0) stream_yield();  /* every 64 */
    }
    return true;
}

bool sbx_uart_file_close(bool commit)
{
    if (!s_file_open) return false;

    sbx_packet_t pkt = {0};
    pkt.type    = SBX_CMD_FILE_CLOSE;
    pkt.data[0] = commit ? 0 : 1;   /* 1 = abort: master deletes the file */
    send_packet(&pkt);
    s_file_open = false;

    /* Wait for the master to confirm the file reached the drive. An abort
     * is acknowledged too, but never counts as success. */
    uint32_t t0 = millis();
    while (!s_file_ack_seen && (millis() - t0) < 2000) stream_yield();

    return commit && s_file_ack_seen && s_file_ack_ok;
}

bool sbx_uart_is_linked(void)    { return (millis() - s_last_rx_ms) < 2000; }
bool sbx_uart_get_door_open(void){ return s_door_open; }
bool sbx_uart_get_relay(uint8_t relay_id) { return s_relay_state & (1 << relay_id); }
bool sbx_uart_get_printer_present(void) { return s_printer_ready; }
bool sbx_uart_get_usb_drive_present(void) { return s_usb_drive; }

bool sbx_uart_get_env(float *t, float *h)
{
    if (t) *t = s_temp;
    if (h) *h = s_hum;
    return s_env_valid;
}
