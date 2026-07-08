// Route ESP_LOGx through Arduino's Serial.printf.
//
// The qio_qspi SDK is built with CONFIG_LOG_MAXIMUM_LEVEL=1 (ERROR only),
// so ESP_LOGI/W/D from <esp_log.h> are compiled out entirely — every
// non-error log message is silently dropped. We override the macros here
// so all four levels reach the CP210x via UART0.
//
// Include AFTER <esp_log.h>.
#pragma once
#include <Arduino.h>

#undef ESP_LOGE
#undef ESP_LOGW
#undef ESP_LOGI
#undef ESP_LOGD
#undef ESP_LOGV

#define ESP_LOGE(tag, fmt, ...) do { Serial.printf("[E][%s] " fmt "\n", tag, ##__VA_ARGS__); Serial.flush(); } while (0)
#define ESP_LOGW(tag, fmt, ...) Serial.printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) Serial.printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) Serial.printf("[D][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) Serial.printf("[V][%s] " fmt "\n", tag, ##__VA_ARGS__)
