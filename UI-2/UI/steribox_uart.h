/**
 * @file steribox_uart.h
 * SteriBox - link to the GPIO master (ESP32-S) over UART1 (RX=44 TX=43).
 *
 * Non-blocking: call sbx_uart_task() every loop() alongside lv_timer_handler().
 * All getters return the last telemetry received (cached) so they never
 * block the LVGL UI thread waiting on the master.
 */
#ifndef STERIBOX_UART_H
#define STERIBOX_UART_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void sbx_uart_init(void);
void sbx_uart_task(void);   /* call every loop(), non-blocking */

/* commands -> master (fire and forget) */
void sbx_uart_send_relay(uint8_t relay_id, bool on);
void sbx_uart_send_buzzer(uint8_t pattern);
void sbx_uart_send_state(uint8_t state);
void sbx_uart_send_print_text(const char *text);

/* ---- file transfer to a USB drive on the master's OTG port ----------
 * Streamed, so a multi-page PDF is never held in RAM on either board.
 * open -> write* -> close; close() blocks briefly (up to ~2 s) waiting
 * for the master's SBX_MSG_FILE_ACK and is the only call that can tell
 * the caller whether the file really landed on the drive.            */
bool sbx_uart_file_open(const char *filename);
bool sbx_uart_file_write(const void *data, uint32_t len);
/** commit=false aborts: the master deletes the partial file, so a failed
 *  export never leaves a truncated report on the operator's drive. */
bool sbx_uart_file_close(bool commit);

/* cached telemetry <- master */
bool sbx_uart_is_linked(void);        /* true if telemetry seen in last 2s */
bool sbx_uart_get_door_open(void);
bool sbx_uart_get_relay(uint8_t relay_id);
bool sbx_uart_get_env(float *temp_c, float *hum_pct); /* false if never valid */
bool sbx_uart_get_printer_present(void); /* true if USB-OTG printer on master */
bool sbx_uart_get_usb_drive_present(void); /* true if USB drive mounted      */

#ifdef __cplusplus
}
#endif
#endif /* STERIBOX_UART_H */
