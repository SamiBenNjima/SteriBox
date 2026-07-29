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
#include <string.h>
#include <stdio.h>

/*==================================================================
 * Pin / address map
 *=================================================================*/
#define I2C_SDA       19          /* white I2C port */
#define I2C_SCL       20
#define RTC_ADDR      0x68        /* DS1307 / DS3231 (WIRED) - same registers */
// #define PCF8574_ADDR 0x20      /* I/O expander (NOT WIRED yet) */

/*------------------------------------------------------------------
 * Build switches
 *-----------------------------------------------------------------*/
/* Serial1 (link to the GPIO master) uses GPIO 43/44 - the SAME pins as
 * UART0, i.e. the CH340 USB-serial console. As soon as sbx_uart_init()
 * runs, the Serial Monitor goes silent / prints binary garbage.
 * Set to 0 to keep a clean console while debugging (relays/buzzer/door
 * on the master board stop working while it is 0). */
#ifndef SBX_MASTER_UART_ENABLED
#define SBX_MASTER_UART_ENABLED 1
#endif

/* If the DS3231 reports a power loss (oscillator stopped -> it would come
 * back as 2000-01-01 00:00, which is exactly the "00:00" on the tile),
 * seed it once from this sketch's compile time. Re-flash right after a
 * compile so the build time is close to the real time, then use the
 * Config screen to fine-tune. */
#ifndef SBX_RTC_SEED_FROM_BUILD
#define SBX_RTC_SEED_FROM_BUILD 1
#endif

/* Set to 1 for ONE upload to force-write the compile time into the RTC,
 * even if the RTC looks healthy. Put it back to 0 afterwards. */
#ifndef SBX_RTC_FORCE_SET
#define SBX_RTC_FORCE_SET 0
#endif

/* SD card SPI pins (confirmed by the CrowPanel SD_Test / test_sensors sketch) */
#define SD_SCK        12
#define SD_MISO       13
#define SD_MOSI       11
#define SD_CS         10

static bool sd_ok       = false;
static bool s_rtc_found = false;   /* DS3231 ACKed at boot */

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

    // Check for dummy/unresponsive bus (0xFF from floating bus)
    if (s == 0xFF && mi == 0xFF && hr == 0xFF) return false;

    uint8_t month_dec = bcd2dec(mo & 0x1F);
    uint8_t day_dec   = bcd2dec(d & 0x3F);

    // Validate bounds for struct tm safety
    if (month_dec < 1 || month_dec > 12) return false;
    if (day_dec < 1 || day_dec > 31) return false;

    t->tm_sec  = bcd2dec(s & 0x7F);
    t->tm_min  = bcd2dec(mi & 0x7F);
    t->tm_hour = bcd2dec(hr & 0x3F);        /* assume 24h mode */
    t->tm_mday = day_dec;
    t->tm_mon  = month_dec - 1;            /* 0..11 */
    t->tm_year = bcd2dec(yr) + 100;         /* years since 1900 (20xx) */
    t->tm_isdst = -1;
    return true;
}

/** DS3231 status register 0x0F, bit7 = OSF (Oscillator Stop Flag).
 *  Set by the chip whenever it lost both Vcc and battery -> the time it
 *  reports is meaningless (typically 2000-01-01 00:00:00).
 *  Returns false if the register could not be read at all. */
static bool ds3231_read_osf(bool * osf)
{
    Wire.beginTransmission(RTC_ADDR);
    Wire.write(0x0F);
    if(Wire.endTransmission() != 0) return false;
    if(Wire.requestFrom(RTC_ADDR, 1) != 1) return false;
    uint8_t st = Wire.read();
    if(st == 0xFF) return false;              /* floating bus */
    *osf = (st & 0x80) != 0;
    return true;
}

/** Clear OSF so the next boot can tell a real power loss from this one. */
static void ds3231_clear_osf(void)
{
    Wire.beginTransmission(RTC_ADDR);
    Wire.write(0x0F);
    if(Wire.endTransmission(false) != 0) { Wire.endTransmission(true); return; }
    if(Wire.requestFrom(RTC_ADDR, 1) != 1) return;
    uint8_t st = Wire.read();
    Wire.beginTransmission(RTC_ADDR);
    Wire.write(0x0F);
    Wire.write((uint8_t)(st & 0x7F));
    Wire.endTransmission();
}

/** Parse __DATE__ / __TIME__ ("Jul 28 2026" / "12:01:21") into a struct tm. */
static bool build_time_to_tm(struct tm * t)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int  day = 0, year = 0, hh = 0, mm = 0, ss = 0;

    if(sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3) return false;
    if(sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss) != 3)     return false;

    const char * p = strstr(months, mon);
    if(!p) return false;

    t->tm_year  = year - 1900;
    t->tm_mon   = (int)(p - months) / 3;
    t->tm_mday  = day;
    t->tm_hour  = hh;
    t->tm_min   = mm;
    t->tm_sec   = ss;
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
/** Seed the ESP32 system clock from the DS3231.
 *  Called from setup() BEFORE tft.begin() - see the header for why. */
void sbx_hal_rtc_early_init(void)
{
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(100000);        /* DS3231 is happy at 100 kHz */
    Wire.setTimeOut(50);          /* never block the bus on a silent device */
    delay(100);                   /* power-up delay for I2C slaves */

    Wire.beginTransmission(RTC_ADDR);
    s_rtc_found = (Wire.endTransmission() == 0);
    if(!s_rtc_found) {
        Serial.println("[HAL] RTC absent from the I2C bus -> clock will show 00:00");
        return;
    }

    struct tm t;
    bool have_time = ds3231_read(&t);

    /* A DS3231 that lost Vcc *and* battery raises OSF and comes back as
     * 2000-01-01 00:00 - exactly the "00:00" bug. Re-seed it once from the
     * compile time so the panel is never left with a meaningless clock. */
    bool osf = false;
    bool osf_ok = ds3231_read_osf(&osf);
    bool needs_seed = SBX_RTC_FORCE_SET
                   || (osf_ok && osf)
                   || (have_time && (t.tm_year + 1900) < 2024);

#if SBX_RTC_SEED_FROM_BUILD
    if(osf_ok && needs_seed) {
        struct tm bt;
        if(build_time_to_tm(&bt)) {
            time_t e = mktime(&bt);
            if(e != (time_t)-1) {
                localtime_r(&e, &bt);       /* fill tm_wday for the RTC */
                ds3231_write(&bt);
                ds3231_clear_osf();
                delay(10);
                have_time = ds3231_read(&t);
                Serial.printf("[HAL] RTC had lost the time -> re-seeded from build %s %s\n",
                              __DATE__, __TIME__);
            }
        }
    }
#else
    (void)needs_seed;
#endif

    if(!have_time) {
        Serial.println("[HAL] RTC read failed -> clock will show 00:00");
        return;
    }

    time_t epoch = mktime(&t);
    if(epoch == (time_t)-1) {
        Serial.println("[HAL] RTC values rejected by mktime() -> clock will show 00:00");
        return;
    }
    struct timeval tv = { epoch, 0 };
    settimeofday(&tv, NULL);
    Serial.printf("[HAL] RTC -> system clock: %04d-%02d-%02d %02d:%02d\n",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
}

void sbx_hal_init(void)
{
    prefs.begin("steribox", false);

    /* NOTE: by this point tft.begin() has re-routed GPIO19/20 to the touch
     * I2C peripheral, so Wire no longer reaches the RTC. Never probe the bus
     * here - each silent address costs a ~1 s driver timeout. Runtime RTC
     * writes re-claim the pins themselves, see sbx_hal_set_datetime(). */

    /* ---- SD card (onboard SPI, same method as the proven SD_Test) ---- */
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    sd_ok = SD.begin(SD_CS);
    if(sd_ok) { SD.mkdir("/steribox"); }
    Serial.printf("[HAL] SD %s\n", sd_ok ? "mounted" : "FAILED");

    /* ---- PCF8574T (NOT WIRED yet) : uncomment once the expander is on the bus
    Wire.beginTransmission(PCF8574_ADDR);
    Wire.write(0xFF);                 // all P0..P7 released (inputs / relays off)
    Wire.endTransmission();
    */

    /* ---- GPIO master link (relays / buzzer / door / DHT22) ----
     * WARNING: GPIO 43/44 are also UART0 = the USB console. Everything
     * printed after this point can be lost or corrupted. Keep it last. */
#if SBX_MASTER_UART_ENABLED
    Serial.println("[HAL] UART link to GPIO master (RX=44 TX=43) - console ends here");
    Serial.flush();
    sbx_uart_init();
#else
    Serial.println("[HAL] Master UART disabled - console kept alive");
    Serial.flush();
#endif
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
    if(epoch == (time_t)-1) return;
    localtime_r(&epoch, &t);               /* fill tm_wday for the RTC */

    /* Re-claim SDA/SCL: the display's touch driver shares these pins and may
     * own the GPIO matrix routing since tft.begin(). Without this the write
     * is silently swallowed and the new time is lost at the next reboot. */
    struct timeval tv = { epoch, 0 };      /* the ESP32 system clock always */
    settimeofday(&tv, NULL);

    if(!s_rtc_found) {
        Serial.println("[HAL] RTC absent: time set for this session only (lost on reboot)");
        Serial.flush();
        return;
    }

    /* Re-claim SDA/SCL: the touch driver owns the GPIO routing since
     * tft.begin(), so without this the write is silently swallowed and the
     * new time is lost at the next reboot. */
    Wire.end();
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(100000);
    Wire.setTimeOut(50);

    ds3231_write(&t);                      /* persist in the DS3231 */
    ds3231_clear_osf();                    /* time is valid again */
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
