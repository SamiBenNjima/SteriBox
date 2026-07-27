/**************************************************************
 * SteriBox UV Sterilizer - CrowPanel ESP32 5.0" HMI sketch
 *
 * Display / touch bring-up is the Elecrow CrowPanel example code.
 * The only SteriBox additions are the three marked lines in setup():
 *     sbx_hal_init();  ui_init();  steribox_app_init();
 *
 * All UI + logic sources live in this same folder and are compiled
 * automatically by the Arduino IDE:
 *   ui*.c/.h, ui_img_*.c, ui_font_*.c,
 *   steribox_app.c/.h, steribox_hal.h, steribox_hal_esp32.cpp, lv_conf.h
 *
 * HARDWARE (see steribox_hal_esp32.cpp for the wiring/pins):
 *   Shared I2C (SDA=19, SCL=20): PCA9557 touch + DS3231 RTC
 *   DS3231 RTC + SD-card logging are ACTIVE (local to this board).
 *   Relays / buzzer / door-PIR / DHT22 live on a separate ESP32-S
 *   "GPIO master" board and are driven over UART1 (RX=44 TX=43) -
 *   see steribox_uart.cpp/.h and sbx_uart_protocol.h, and the
 *   SteriBox_Master sketch for the master-side firmware.
 *   The CH376S USB printer is still commented (not wired yet).
 *   sbx_hal_init() sets up I2C / DS3231 / SD / UART link - nothing to do here.
 *
 * IMPORTANT (one-time checks in lv_conf.h):
 *   #define LV_TICK_CUSTOM   1     // else every timer/clock/countdown freezes
 *   #define LV_COLOR_DEPTH   16
 *   Montserrat 16 / 36 / 48 enabled (14 no longer used)
 **************************************************************/

#include <Wire.h>
#include <SPI.h>
#include <SD.h>             // SD-card logging (used by the HAL)
#include <PCA9557.h>

#include <lvgl.h>
//#include "demos/lv_demos.h"      // leave commented: wastes flash
//#include "examples/lv_examples.h"

#include "gfx_conf.h"

#include "ui.h"
#include "steribox_app.h"          // SteriBox: application logic
#include "steribox_hal.h"          // SteriBox: hardware abstraction
#include "steribox_uart.h"         // SteriBox: UART link to the GPIO master (ESP32-S)

static lv_disp_draw_buf_t draw_buf;
static lv_color_t disp_draw_buf1[screenWidth * screenHeight / 10];
static lv_color_t disp_draw_buf2[screenWidth * screenHeight / 10];
static lv_disp_drv_t disp_drv;

PCA9557 Out;    // I/O expander (touch timing / power-up)

/* Display flushing */
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  tft.pushImageDMA(area->x1, area->y1, w, h, (lgfx::rgb565_t *)&color_p->full);

  lv_disp_flush_ready(disp);
}

/* Touch reading */
void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
  uint16_t touchX, touchY;
  bool touched = tft.getTouch(&touchX, &touchY);
  if (!touched) {
    data->state = LV_INDEV_STATE_REL;
  } else {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = touchX;
    data->point.y = touchY;
  }
}

void setup()
{
  Serial.begin(115200);

  // 1. I/O expander (touch + power-up sequencing)
  Wire.begin(19, 20);
  Out.reset();
  Out.setMode(IO_OUTPUT);
  Out.setState(IO0, IO_LOW);
  Out.setState(IO1, IO_LOW);
  delay(20);
  Out.setState(IO0, IO_HIGH);   // stable power
  delay(100);
  Out.setMode(IO1, IO_INPUT);

  // 2. Screen power rail (GPIO 38 = ON for CrowPanel 5.0)
  pinMode(38, OUTPUT);
  digitalWrite(38, HIGH);

  // RGB aux pins
  pinMode(17, OUTPUT); digitalWrite(17, LOW);
  pinMode(18, OUTPUT); digitalWrite(18, LOW);
  pinMode(42, OUTPUT); digitalWrite(42, LOW);

  // 3. LovyanGFX
  tft.begin();
  tft.setBrightness(255);
  tft.fillScreen(TFT_BLACK);
  delay(200);

  // 4. LVGL core
  lv_init();
  lv_disp_draw_buf_init(&draw_buf, disp_draw_buf1, disp_draw_buf2,
                        screenWidth * screenHeight / 10);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.full_refresh = 1;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  // 5. SteriBox: HAL, UI, then application logic (order matters)
  sbx_hal_init();          // I2C + DS3231 RTC + SD card (relays/DHT mocked till wired)
  ui_init();               // build all screens
  steribox_app_init();     // wire events, cycle state machine, clock, chart

  Serial.println("SteriBox UI ready");
}

void loop()
{
  sbx_uart_task();   // SteriBox: pump the GPIO-master UART link (non-blocking)
  lv_timer_handler();
  delay(5);
}
