#ifndef TECHO_GPS_NMEA_H
#define TECHO_GPS_NMEA_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Observe only: a silent UART cannot establish that the GNSS is powered off. */
typedef struct {
    uint32_t bytes, valid, invalid, overflow, last_ms, fix_ms, satellites_ms;
    bool seen, fix_known, fix, satellites_known;
    unsigned satellites, used;
    uint32_t position_ms;
    bool position_known;
    double lat, lon;
    char line[128];
} GpsNmea;

void gps_nmea_feed(GpsNmea *gps, unsigned char ch, uint32_t now);
bool gps_nmea_recent(bool known, uint32_t last, uint32_t now);

#ifdef __cplusplus
}
#endif
#endif
