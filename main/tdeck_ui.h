#ifndef LILYGO_TDECK_UI_H
#define LILYGO_TDECK_UI_H

#include "history.h"
#include "tdeck_ubx.h"
#include "tdeck_map.h"
#include "tdeck_replay.h"

enum { TDECK_WIDTH=320, TDECK_HEIGHT=240, TDECK_CLOCK_DIGITS=12 };
typedef enum {
    TDECK_LIST, TDECK_FLIGHT, TDECK_RF, TDECK_SYSTEM, TDECK_BRIGHTNESS, TDECK_TUNING,
    TDECK_GPS, TDECK_MAP, TDECK_REPLAY_FILES, TDECK_CLOCK, TDECK_RESTART, TDECK_SCREEN_OFF,
    TDECK_CLOCK_CONFIRM, TDECK_LANGUAGE
} TdeckPage;
typedef enum {
    TDECK_PREV, TDECK_NEXT, TDECK_OPEN, TDECK_BACK, TDECK_RADIO, TDECK_MENU,
    TDECK_LEFT, TDECK_RIGHT, TDECK_UP, TDECK_DOWN, TDECK_OPEN_MAP,
    TDECK_OPEN_SYSTEM, TDECK_PICK_REPLAY
} TdeckAction;
typedef enum {
    TDECK_FOCUS_AIRCRAFT, TDECK_FOCUS_MAP, TDECK_FOCUS_REPLAY, TDECK_FOCUS_SYSTEM
} TdeckListFocus;
typedef enum { TDECK_ENGLISH, TDECK_ITALIAN } TdeckLanguage;
typedef enum { GPS_NO_DATA, GPS_NMEA, GPS_NO_FIX, GPS_FIX, GPS_UART_ERROR } TdeckGpsState;
typedef enum { GPS_RUNNING, GPS_SLEEPING, GPS_STANDBY, GPS_WAKING, GPS_WAKE_FAILED } TdeckGpsPower;

typedef struct {
    TdeckPage page;
    TdeckListFocus list_focus;
    uint32_t selected;
    unsigned index;
    unsigned menu_item;
    TdeckLanguage language_choice;
    bool language_save_error;
    TdeckMap map;
    unsigned replay_index;
    char clock_entry[TDECK_CLOCK_DIGITS+1];
    unsigned clock_cursor;
    bool clock_error, clock_modify, clock_storage_error;
} TdeckNav;

typedef struct {
    TdeckLanguage language;
    uint32_t boot, dial, requested, failed;
    unsigned df17, bursts, queue_full;
    int battery_mv;
    TdeckGpsState gps;
    TdeckGpsPower gps_power;
    bool gps_can_sleep;
    int satellites; /* -1 when no recent GGA satellite count is available. */
    unsigned gps_baud;
    uint32_t gps_sentences;
    const char *gps_model;
    const GnssSatellites *gps_constellations;
    unsigned brightness;
    bool ready, retry, replay, usb_power;
    bool radio_polled; /* At least one radio status read since the previous frame. */
    const Capture *trace;
    TdeckPosition gps_position;
    TdeckMapView map_view;
    const uint16_t *map_pixels;
    unsigned map_tiles;
    bool map_available, map_loading;
    const TdeckReplayReply *replay_files;
    const TdeckReplayAircraft *archive;
    unsigned archive_count;
    const char *archive_name;
    const char *local_time;
    const char *saved_time;
    bool clock_setup;
    bool restart_error, restart_sd_warning;
} TdeckStatus;

void tdeck_navigate(TdeckNav *nav, TdeckAction action, uint32_t boot);
void tdeck_navigate_replay(TdeckNav *nav, TdeckAction action,
                          const TdeckReplayAircraft *aircraft, unsigned count);
void tdeck_clock_edit_begin(TdeckNav *nav, const char *iso);
bool tdeck_clock_edit_navigate(TdeckNav *nav, TdeckAction action);
bool tdeck_clock_edit_key(TdeckNav *nav, uint8_t key);
void tdeck_render(uint16_t pixels[TDECK_WIDTH*TDECK_HEIGHT], TdeckNav *nav,
                  const TdeckStatus *status, int64_t now);

#endif
