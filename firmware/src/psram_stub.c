#include "esp_err.h"

// HC33 has no accessible PSRAM. platformio.ini defines
// -Desp_spiram_init=hc33_spiram_init_stub so that the call in
// esp32-hal-psram.c:psramInit() lands here instead of the IDF function,
// which would probe the MSPI bus and leave it in a corrupt state.
esp_err_t hc33_spiram_init_stub(void) {
    return ESP_ERR_NOT_FOUND;
}
