#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_light_init(void);

void app_light_set_on(bool on);
bool app_light_is_on(void);

// Set the RGB color (kept while the light is on). "#RRGGBB" form.
void app_light_set_color(uint8_t r, uint8_t g, uint8_t b);
void app_light_get_color(char *out, size_t max);

#ifdef __cplusplus
}
#endif
