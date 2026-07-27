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

/* cached telemetry <- master */
bool sbx_uart_is_linked(void);        /* true if telemetry seen in last 2s */
bool sbx_uart_get_door_open(void);
bool sbx_uart_get_relay(uint8_t relay_id);
bool sbx_uart_get_env(float *temp_c, float *hum_pct); /* false if never valid */

#ifdef __cplusplus
}
#endif
#endif /* STERIBOX_UART_H */
