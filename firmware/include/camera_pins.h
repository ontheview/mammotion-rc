// OV3660 DVP pinout for the Heltec HT-HC33.
//
// In-repo on purpose.  Heltec's board variant
// (ESP_HaLow/variants/HT-HC33/pins_arduino.h) defines these same macros with
// these same names, but that variant only lands on the include path for
// env:hc33, which sets `board_build.variant = HT-HC33` against Heltec's
// arduino-esp32 fork.  env:hc33-standard-wifi -- the env the camera runs on
// -- builds against the stock esp32-s3-devkitc-1 variant of espressif32@6.10.0,
// where none of these exist.  Depending on an external ~600 MB clone for a
// pinout is also how bring-up turns into a toolchain argument.
//
// Only included from camera_stream.cpp, which is compiled solely when
// ENABLE_CAMERA is defined (standard-wifi).  It is therefore never seen by a
// build where Heltec's variant has already defined these, so the plain
// #defines below cannot collide.
//
// NOTE: firmware/camera-test/include/camera_pins.h is a deliberate duplicate
// of this file -- that project is standalone by design.  If a pin ever
// changes, change both.
//
// Values transcribed from Heltec's variant and cross-checked against it.
//
// PIN CONFLICTS -- read before wiring anything else to these GPIOs
// ----------------------------------------------------------------
//   GPIO 18  Y6, and also LED_BUILTIN in Heltec's variant.  Driving the
//            built-in LED corrupts video data.  Don't use LED_BUILTIN on a
//            camera build.
//   GPIO 45  SIOD, and an ESP32-S3 strapping pin (VDD_SPI select).
//   GPIO 46  Y7, and an ESP32-S3 strapping pin (boot mode).
//            Both are fine because the sensor is passive during reset, but
//            nothing may drive them while the S3 comes out of reset.
//   GPIO 19  RGB LED in Heltec's variant; GPIO 20 is PWDN here.  On the
//            ESP32-S3 these two are the native USB D-/D+ pins.  Free on the
//            HC33 because it talks over a CP210x UART bridge -- but enabling
//            native USB-CDC would fight the camera's power-down line.
//
// NOT in conflict, verified against Heltec's variant:
//   MM6108 (HaLow) uses GPIO 2,3,4,5,6,7,8,9 -- no overlap with any pin
//   below, which is what makes a camera + HaLow build possible at all.
//   Octal PSRAM claims GPIO 33-37 -- no overlap either (asserted at the
//   bottom of this file).

#pragma once

// ---- Power / clock ----
#define PWDN_GPIO_NUM   20
#define RESET_GPIO_NUM  -1    // no hardware reset line; SCCB soft-reset only
#define XCLK_GPIO_NUM   47

// ---- SCCB (I2C-like sensor control bus) ----
#define SIOD_GPIO_NUM   45
#define SIOC_GPIO_NUM   42

// ---- 8-bit parallel data bus (D7..D0 = Y9..Y2) ----
#define Y9_GPIO_NUM     38
#define Y8_GPIO_NUM     48
#define Y7_GPIO_NUM     46
#define Y6_GPIO_NUM     18
#define Y5_GPIO_NUM     14
#define Y4_GPIO_NUM     12
#define Y3_GPIO_NUM     13
#define Y2_GPIO_NUM     17

// ---- Sync ----
#define VSYNC_GPIO_NUM  40
#define HREF_GPIO_NUM   39
#define PCLK_GPIO_NUM   21

// ---- Build-time guard: octal PSRAM reserves GPIO 33-37 ----
//
// This project runs with memory_type=qio_opi, which hands GPIO 33-37 to the
// MSPI bus.  No camera pin uses them today; this assert makes sure a future
// pinout edit can't quietly break that and produce corruption that would
// read as a sensor fault.
#define HC33_CAM_PIN_FREE(p) ((p) < 33 || (p) > 37)
#if !(HC33_CAM_PIN_FREE(PWDN_GPIO_NUM)  && \
      HC33_CAM_PIN_FREE(RESET_GPIO_NUM) && \
      HC33_CAM_PIN_FREE(XCLK_GPIO_NUM)  && \
      HC33_CAM_PIN_FREE(SIOD_GPIO_NUM)  && \
      HC33_CAM_PIN_FREE(SIOC_GPIO_NUM)  && \
      HC33_CAM_PIN_FREE(Y2_GPIO_NUM)    && \
      HC33_CAM_PIN_FREE(Y3_GPIO_NUM)    && \
      HC33_CAM_PIN_FREE(Y4_GPIO_NUM)    && \
      HC33_CAM_PIN_FREE(Y5_GPIO_NUM)    && \
      HC33_CAM_PIN_FREE(Y6_GPIO_NUM)    && \
      HC33_CAM_PIN_FREE(Y7_GPIO_NUM)    && \
      HC33_CAM_PIN_FREE(Y8_GPIO_NUM)    && \
      HC33_CAM_PIN_FREE(Y9_GPIO_NUM)    && \
      HC33_CAM_PIN_FREE(VSYNC_GPIO_NUM) && \
      HC33_CAM_PIN_FREE(HREF_GPIO_NUM)  && \
      HC33_CAM_PIN_FREE(PCLK_GPIO_NUM))
#error "A camera pin lands on GPIO33-37, which octal PSRAM reserves. Fix the pinout or drop to memory_type=qio_qspi (and lose PSRAM)."
#endif
