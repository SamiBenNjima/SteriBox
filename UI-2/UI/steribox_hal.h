/**
 * @file steribox_hal.h
 * SteriBox UV Sterilizer - Hardware Abstraction Layer (portable API)
 *
 * One header, two implementations:
 *   - steribox_hal_sim.c    : PC simulator (SDL) - keyboard-driven fake IO
 *   - steribox_hal_esp32.cpp: Elecrow 5" panel (ESP32-S3) - real GPIO
 *
 * Hardware map (target):
 *   RELAY1  -> UV lamp 1 ballast        (active HIGH)
 *   RELAY2  -> UV lamp 2 ballast        (active HIGH)
 *   BUZZER  -> piezo buzzer             (PWM)
 *   PIR     -> door / presence sensor   (HIGH = door open / motion)
 *   DHT     -> temperature + humidity sensor
 *   USB     -> USB host port: mass-storage drive (export) or printer
 */
#ifndef STERIBOX_HAL_H
#define STERIBOX_HAL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*------------------------------------------------
 * Types
 *-----------------------------------------------*/
typedef enum {
    SBX_RELAY_LAMP1 = 0,
    SBX_RELAY_LAMP2 = 1,
} sbx_relay_t;

typedef enum {
    SBX_BEEP_KEY,      /*short click feedback            */
    SBX_BEEP_OK,       /*action confirmed (double beep)  */
    SBX_BEEP_WARN,     /*refused action (long low beep)  */
    SBX_BEEP_DONE,     /*cycle finished (triple beep)    */
    SBX_BEEP_ALARM,    /*safety abort (repeated beeps)   */
} sbx_beep_t;

typedef struct {
    uint16_t year;
    uint8_t  month;    /*1..12*/
    uint8_t  day;      /*1..31*/
    uint8_t  hour;     /*0..23*/
    uint8_t  minute;   /*0..59*/
} sbx_datetime_t;

/** Data persisted across power cycles (NVS on ESP32, file on PC). */
typedef struct {
    uint32_t magic;             /*validity marker*/
    uint32_t lamp1_seconds;     /*accumulated ON time, lamp 1*/
    uint32_t lamp2_seconds;     /*accumulated ON time, lamp 2*/
    uint32_t total_seconds;     /*accumulated device ON time*/
    uint32_t cycles_done;       /*completed sterilization cycles*/
    uint32_t cycles_aborted;    /*aborted (door opened / stopped)*/
    char     password[16];      /*config screen password*/
} sbx_persist_t;

#define SBX_PERSIST_MAGIC     0x53425831u   /*"SBX1"*/
#define SBX_DEFAULT_PASSWORD  "1234"
/** UVC tube rated life (typical low-pressure Hg lamp: 9000 h) */
#define SBX_LAMP_LIFE_HOURS   9000u

/*------------------------------------------------
 * Core
 *-----------------------------------------------*/
void     sbx_hal_init(void);
uint32_t sbx_hal_millis(void);

/*------------------------------------------------
 * Relays (UV lamp ballasts)
 *-----------------------------------------------*/
void sbx_hal_relay_set(sbx_relay_t relay, bool on);
bool sbx_hal_relay_get(sbx_relay_t relay);

/*------------------------------------------------
 * Buzzer
 *-----------------------------------------------*/
void sbx_hal_buzzer(sbx_beep_t pattern);

/*------------------------------------------------
 * Door sensor (PIR)
 *-----------------------------------------------*/
bool sbx_hal_door_is_open(void);

/*------------------------------------------------
 * Environment sensor (temperature / humidity)
 *-----------------------------------------------*/
bool sbx_hal_read_env(float * temp_c, float * hum_pct);

/*------------------------------------------------
 * USB port (mass storage drive or laser printer)
 *-----------------------------------------------*/
bool sbx_hal_usb_present(void);
/** Write a text report to the USB drive. Returns false if no drive. */
bool sbx_hal_usb_export(const char * filename, const char * text);
/** Send a text report to the attached printer. Returns false if absent. */
bool sbx_hal_usb_print(const char * text);

/*------------------------------------------------
 * Real-time clock
 *-----------------------------------------------*/
void sbx_hal_get_datetime(sbx_datetime_t * dt);
void sbx_hal_set_datetime(const sbx_datetime_t * dt);

/*------------------------------------------------
 * Persistent storage
 *-----------------------------------------------*/
bool sbx_hal_storage_load(sbx_persist_t * data);
bool sbx_hal_storage_save(const sbx_persist_t * data);

/*------------------------------------------------
 * Simulator-only hooks (no-ops on target)
 *-----------------------------------------------*/
void sbx_hal_sim_toggle_door(void);
void sbx_hal_sim_toggle_usb(void);
void sbx_hal_sim_bump_temp(float delta);
void sbx_hal_sim_bump_hum(float delta);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*STERIBOX_HAL_H*/
