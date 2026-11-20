#ifndef TDECK_KEYBOARD_H
#define TDECK_KEYBOARD_H
#include "esp_err.h"
#include <stdint.h>
esp_err_t tdeck_keyboard_init(void);
uint8_t tdeck_keyboard_read(int64_t now);
#endif
