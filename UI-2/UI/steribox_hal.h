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
/** I2C + DS3231 bring-up. MUST be called BEFORE tft.begin(): the display
 *  driver re-routes SDA/SCL (GPIO19/20) to its own I2C peripheral for the
 *  touch panel, after which Wire transactions on those pins can no longer
 *  reach the RTC. Seeds the ESP32 system clock from the RTC. */
void     sbx_hal_rtc_early_init(void);
void     sbx_hal_init(void);
uint32_t sbx_hal_millis(void);

/*------------------------------------------------
 * Relays (UV lamp ballasts)
 *-----------------------------------------------*/
void sbx_hal_relay_set(sbx_relay_t relay, bool on);
bool sbx_hal_relay_get(sbx_relay_t relay);

/*------------------------------------------------
 * Buzzer & System State
 *-----------------------------------------------*/
void sbx_hal_buzzer(sbx_beep_t pattern);
void sbx_hal_set_system_state(uint8_t state);

/*------------------------------------------------
 * Door sensor (PIR)
 *-----------------------------------------------*/
bool sbx_hal_door_is_open(void);

/*------------------------------------------------
 * Environment sensor (temperature / humidity)
 *-----------------------------------------------*/
bool sbx_hal_read_env(float * temp_c, float * hum_pct);

/*------------------------------------------------
 * SD card (onboard slot, SPI)
 *-----------------------------------------------*/
/** Returns true while the SD card is mounted and accessible. */
bool sbx_hal_sd_present(void);

/** Alias kept for compatibility — maps to sbx_hal_sd_present().
 *  On the slave board the SD card IS the export storage medium. */
bool sbx_hal_usb_present(void);

/** Write a text report to /steribox/<filename>. Returns false if no SD. */
bool sbx_hal_usb_export(const char * filename, const char * text);

/** Write/overwrite <filename> under /steribox/ and record in syslog. */
bool sbx_hal_log_snapshot(const char * filename, const char * text);

/** Append one timestamped CSV row to /steribox/syslog.csv.
 *  Format: YYYY-MM-DD HH:MM:SS,<tag>,<detail>\n
 *  Silent no-op when no SD card is present. */
bool sbx_hal_log_event(const char * tag, const char * detail);

/** Append one raw line to an arbitrary file under /steribox/, creating it
 *  with `header` as its first line if it does not exist yet. Used for the
 *  machine-readable per-cycle ledger (cycles.csv). Pass NULL for no header.
 *  Silent no-op when no SD card is present. */
bool sbx_hal_log_append(const char * filename, const char * header,
                        const char * line);

/*------------------------------------------------
 * Streaming file writer (reports / PDF export)
 *
 * One file at a time - the UI is single-threaded and never interleaves
 * exports. Writing a document is always open -> write* -> close, and
 * close() is what reports the final success: on the USB destination the
 * bytes are tunnelled to the master, so nothing is confirmed until the
 * whole document has been acknowledged.
 *-----------------------------------------------*/
typedef enum {
    SBX_DEST_SD  = 0,   /*onboard SD card slot (always present in the box)*/
    SBX_DEST_USB = 1,   /*USB flash drive on the master's USB-OTG port    */
} sbx_dest_t;

/** True when a USB mass-storage drive is mounted on the master's OTG port. */
bool sbx_hal_usb_drive_present(void);

/** Open <filename> for writing on `dest`, truncating any existing file.
 *  Files land in /steribox/ on SD and in the root folder on a USB drive. */
bool sbx_hal_file_open(sbx_dest_t dest, const char * filename);

/** Append `len` bytes to the file opened by sbx_hal_file_open(). */
bool sbx_hal_file_write(const void * data, uint32_t len);

/** Flush and close. Pass commit=false to discard the file instead - a
 *  report that could not be composed must never be left behind in a
 *  half-written state for an operator to pick up. Returns true only when
 *  the complete document reached the medium. */
bool sbx_hal_file_close(bool commit);

/*------------------------------------------------
 * Printer via USB-OTG (ESP32-S3 master, GPIO 19/20)
 *-----------------------------------------------*/
/** Returns true when a USB printer is detected on the master's OTG port.
 *  Slave queries this via sbx_uart_get_printer_present() from the
 *  master telemetry packet (flag bit SBX_FLAG_PRINTER_READY). */
bool sbx_hal_printer_present(void);

/** Send a text report to the attached printer. Returns false if absent.
 *  On the slave this tunnels the text to the master over UART, which
 *  forwards it to the CH-class USB printer driver. */
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
