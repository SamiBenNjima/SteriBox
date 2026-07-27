/**
 * @file steribox_hal_esp32.cpp
 * SteriBox HAL - CrowPanel ESP32-S3 5.0"  (partial hardware bring-up)
 *
 * WIRING (100% safe, GPIO_D / IO38 left empty):
 *   I2C cascade (white I2C port): SDA=IO19, SCL=IO20  -> DS3231 RTC + PCF8574T
 *   DS3231 RTC  @ 0x68   (WIRED)   -> real time
 *   SD card (onboard SPI)          -> report logging
 *   PCF8574T    @ 0x20   (NOT WIRED yet) : P0..P2 relays, P3 button, P4 DHT22
 *   CH376S USB key (UART0)         (NOT WIRED yet) : print / mass-storage
 *
 * ACTIVE in this build : I2C init, DS3231 RTC, SD-card export/log, NVS persist.
 * COMMENTED (buy + wire, then uncomment) : PCF8574T relays/button/DHT22, CH376S.
 * While commented, those functions fall back to safe mock values so the UI runs.
 *
 * Only ONE HAL .cpp may be in the sketch at a time.
 */
#if defined(ARDUINO) && !defined(STERIBOX_SIMULATOR)

#include "steribox_hal.h"
#include "steribox_uart.h"   /* SteriBox: link to the GPIO master (ESP32-S) */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>

/*==================================================================
 * Pin / address map
 *=================================================================*/
#define I2C_SDA       19          /* white I2C port */
#define I2C_SCL       20
#define RTC_ADDR      0x68        /* DS1307 / DS3231 (WIRED) - same registers */
// #define PCF8574_ADDR 0x20      /* I/O expander (NOT WIRED yet) */

/* SD card SPI pins (confirmed by the CrowPanel SD_Test / test_sensors sketch) */
#define SD_SCK        12
#define SD_MISO       13
#define SD_MOSI       11
#define SD_CS         10

static bool sd_ok = false;

static Preferences prefs;
static bool  relay_state[2];   /* optimistic local mirror, see sbx_hal_relay_set() */

/*==================================================================
 * BCD helpers for the DS3231
 *=================================================================*/
static uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
static uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

static bool ds3231_read(struct tm * t)
{
    Wire.beginTransmission(RTC_ADDR);
    Wire.write(0x00);
    if(Wire.endTransmission() != 0) return false;
    if(Wire.requestFrom(RTC_ADDR, 7) != 7) return false;

    uint8_t s  = Wire.read();
    uint8_t mi = Wire.read();
    uint8_t hr = Wire.read();
    uint8_t dw = Wire.read();
    uint8_t d  = Wire.read();
    uint8_t mo = Wire.read();
    uint8_t yr = Wire.read();
    (void)dw;

    t->tm_sec  = bcd2dec(s  & 0x7F);
    t->tm_min  = bcd2dec(mi & 0x7F);
    t->tm_hour = bcd2dec(hr & 0x3F);        /* assume 24h mode */
    t->tm_mday = bcd2dec(d  & 0x3F);
    t->tm_mon  = bcd2dec(mo & 0x1F) - 1;    /* 0..11 */
    t->tm_year = bcd2dec(yr) + 100;         /* years since 1900 (20xx) */
    t->tm_isdst = -1;
    return true;
}

static void ds3231_write(const struct tm * t)
{
    Wire.beginTransmission(RTC_ADDR);
    Wire.write(0x00);
    Wire.write(dec2bcd((uint8_t)t->tm_sec));
    Wire.write(dec2bcd((uint8_t)t->tm_min));
    Wire.write(dec2bcd((uint8_t)t->tm_hour));   /* 24h */
    Wire.write(dec2bcd((uint8_t)(t->tm_wday + 1)));
    Wire.write(dec2bcd((uint8_t)t->tm_mday));
    Wire.write(dec2bcd((uint8_t)(t->tm_mon + 1)));
    Wire.write(dec2bcd((uint8_t)(t->tm_year - 100)));
    Wire.endTransmission();
}

/*==================================================================
 * Init
 *=================================================================*/
void sbx_hal_init(void)
{
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setTimeOut(50);          /* never block the bus on a silent device */
    prefs.begin("steribox", false);

    /* ---- SD card (onboard SPI, same method as the proven SD_Test) ---- */
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    sd_ok = SD.begin(SD_CS);
    if(sd_ok) { SD.mkdir("/steribox"); }
    Serial.printf("[HAL] SD %s\n", sd_ok ? "mounted" : "FAILED");

    /* ---- DS1307/DS3231 -> ESP32 system clock ---- */
    struct tm t;
    if(ds3231_read(&t)) {
        time_t epoch = mktime(&t);
        struct timeval tv = { epoch, 0 };
        settimeofday(&tv, NULL);
        Serial.println("[HAL] RTC synced (DS1307/DS3231)");
    } else {
        Serial.println("[HAL] RTC not found (using system time)");
    }

    /* ---- PCF8574T (NOT WIRED yet) : uncomment once the expander is on the bus
    Wire.beginTransmission(PCF8574_ADDR);
    Wire.write(0xFF);                 // all P0..P7 released (inputs / relays off)
    Wire.endTransmission();
    */

    /* ---- GPIO master link (relays / buzzer / door / DHT22) ---- */
    sbx_uart_init();
    Serial.println("[HAL] UART link to GPIO master started (RX=44 TX=43)");
}

uint32_t sbx_hal_millis(void) { return millis(); }

/*==================================================================
 * Relays  (physically on the GPIO master, ESP32-S -> commanded over UART)
 *=================================================================*/
void sbx_hal_relay_set(sbx_relay_t relay, bool on)
{
    relay_state[relay] = on;               /* optimistic local cache */
    sbx_uart_send_relay((uint8_t)relay, on);
}

bool sbx_hal_relay_get(sbx_relay_t relay) { return relay_state[relay]; }

/*==================================================================
 * Buzzer  (physically on the GPIO master -> commanded over UART)
 *=================================================================*/
void sbx_hal_buzzer(sbx_beep_t pattern) { sbx_uart_send_buzzer((uint8_t)pattern); }

/*==================================================================
 * Door / PIR  (physically on the GPIO master -> read from telemetry cache)
 * Non-blocking: returns the last value received, "closed" until the first
 * telemetry packet arrives.
 *=================================================================*/
bool sbx_hal_door_is_open(void)
{
    return sbx_uart_get_door_open();
}

/*==================================================================
 * Environment (DHT22, physically on the GPIO master -> telemetry cache)
 * Returns false (keeps last known values in temp_c/hum_pct untouched by
 * the caller) if no valid reading has ever been received - e.g. link
 * down or DHT22 not wired yet on the master.
 *=================================================================*/
bool sbx_hal_read_env(float * t, float * h)
{
    return sbx_uart_get_env(t, h);
}

/*==================================================================
 * SD-card logging / export   (ACTIVE)
 *  usb_present  -> SD card mounted
 *  usb_export   -> write the report to /steribox/<filename> and append
 *                  a timestamped line to /steribox/history.log
 *=================================================================*/
bool sbx_hal_usb_present(void) { return sd_ok; }

bool sbx_hal_usb_export(const char * filename, const char * text)
{
    if(!sd_ok) return false;

    char path[64];
    snprintf(path, sizeof(path), "/steribox/%s", filename);
    File f = SD.open(path, FILE_WRITE);
    if(!f) return false;
    f.print(text);
    f.close();

    /* Append one line to the rolling history log with a system timestamp */
    File log = SD.open("/steribox/history.log", FILE_APPEND);
    if(log) {
        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);
        char ts[24];
        snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
        log.printf("%s  saved %s\n", ts, filename);
        log.close();
    }
    return true;
}

/*==================================================================
 * Printer via CH376S on UART0  (NOT WIRED yet -> stub)
 *=================================================================*/
bool sbx_hal_usb_print(const char * text)
{
    (void)text;
    return false;

    /* ---- CH376S version (uncomment when wired on UART0) ----
     * Serial.print(text);   // UART0 -> CH376S TXD/RXD (see BLOC C)
     * return true;
     */
}

/*==================================================================
 * RTC  (DS3231, ACTIVE)
 *=================================================================*/
void sbx_hal_get_datetime(sbx_datetime_t * dt)
{
    /* Fast, non-blocking: use the ESP32 system clock (seeded from the RTC at
     * boot). Never touch I2C here - a missing RTC would stall every call. */
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    dt->year   = (uint16_t)(t.tm_year + 1900);
    dt->month  = (uint8_t)(t.tm_mon + 1);
    dt->day    = (uint8_t)t.tm_mday;
    dt->hour   = (uint8_t)t.tm_hour;
    dt->minute = (uint8_t)t.tm_min;
}

void sbx_hal_set_datetime(const sbx_datetime_t * dt)
{
    struct tm t = {};
    t.tm_year = dt->year - 1900;
    t.tm_mon  = dt->month - 1;
    t.tm_mday = dt->day;
    t.tm_hour = dt->hour;
    t.tm_min  = dt->minute;
    t.tm_sec  = 0;
    t.tm_isdst = -1;
    time_t epoch = mktime(&t);
    localtime_r(&epoch, &t);               /* fill tm_wday for the RTC */

    ds3231_write(&t);                      /* persist in the DS3231 */

    struct timeval tv = { epoch, 0 };      /* and the ESP32 system clock */
    settimeofday(&tv, NULL);
}

/*==================================================================
 * Persistence (NVS via Preferences — no pins, always available)
 *=================================================================*/
bool sbx_hal_storage_load(sbx_persist_t * data)
{
    size_t n = prefs.getBytes("persist", data, sizeof(*data));
    return n == sizeof(*data) && data->magic == SBX_PERSIST_MAGIC;
}

bool sbx_hal_storage_save(const sbx_persist_t * data)
{
    return prefs.putBytes("persist", data, sizeof(*data)) == sizeof(*data);
}

/*==================================================================
 * Simulator hooks — no-ops on hardware
 *=================================================================*/
void sbx_hal_sim_toggle_door(void) {}
void sbx_hal_sim_toggle_usb(void) {}
void sbx_hal_sim_bump_temp(float d) { (void)d; }
void sbx_hal_sim_bump_hum(float d)  { (void)d; }

#endif /*ARDUINO && !STERIBOX_SIMULATOR*/
