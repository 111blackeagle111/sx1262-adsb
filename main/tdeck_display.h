#ifndef LILYGO_TDECK_DISPLAY_H
#define LILYGO_TDECK_DISPLAY_H

#include "esp_err.h"
#include "tdeck_ui.h"

enum { TDECK_STRIP_ROWS=2 };
typedef enum { TDECK_DISPLAY_ON, TDECK_DISPLAY_OFF, TDECK_DISPLAY_WAKING } TdeckDisplayPower;
esp_err_t tdeck_display_init(void);
esp_err_t tdeck_display_draw(const uint16_t pixels[TDECK_WIDTH*TDECK_HEIGHT]);
void tdeck_backlight(unsigned level);
void tdeck_display_request(bool on);
bool tdeck_display_requested(void);
TdeckDisplayPower tdeck_display_power(void);
esp_err_t tdeck_display_poll(int64_t now);
esp_err_t tdeck_battery_init(void);
int tdeck_battery_mv(void);
esp_err_t tdeck_controls_init(void);
bool tdeck_controls(TdeckAction *action, int64_t now, bool map, bool zoom);

#endif
