#ifndef LILYGO_TDECK_UBX_H
#define LILYGO_TDECK_UBX_H

#include <stdbool.h>
#include <stdint.h>

enum {
    GNSS_GPS=0, GNSS_SBAS=1, GNSS_GALILEO=2, GNSS_BEIDOU=3,
    GNSS_QZSS=5, GNSS_GLONASS=6, GNSS_COUNT=7,
    UBX_NAV=0x01, UBX_NAV_SAT=0x35, UBX_MON=0x0a, UBX_MON_VER=0x04
};

typedef struct {
    uint8_t visible[GNSS_COUNT], used[GNSS_COUNT];
} GnssSatellites;

typedef struct {
    /* NAV-SAT carries up to 255 satellite records of 12 bytes each. */
    uint8_t data[8+12*255+6];
    unsigned used, length;
    uint8_t ck_a, ck_b;
} UbxReceiver;

/* A true result leaves class, ID, length and payload in data[0..]. */
bool tdeck_ubx_feed(UbxReceiver *receiver, uint8_t byte);
bool tdeck_ubx_satellites(const UbxReceiver *receiver, GnssSatellites *satellites);

#endif
