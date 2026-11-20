#ifndef TDECK_MAP_H
#define TDECK_MAP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { MAP_WIDTH=320, MAP_TOP=30, MAP_HEIGHT=183, TILE_SIZE=256 };
enum { MAP_MIN_ZOOM=1, MAP_DEFAULT_ZOOM=10, MAP_MAX_ZOOM=13 };
enum { TILE_PNG_MAX=384*1024, TILE_SCAN_MAX=(4*TILE_SIZE+1)*TILE_SIZE };

typedef struct { double lat, lon; bool valid; } TdeckPosition;
typedef struct { int left, top; unsigned zoom; } TdeckMapView;
typedef struct {
    double x, y;
    unsigned zoom;
    bool initialized, waiting_fix, zooming;
} TdeckMap;

TdeckMapView tdeck_map_view(TdeckMap *map, TdeckPosition gps);
void tdeck_map_move(TdeckMap *map, int dx, int dy);
bool tdeck_map_same(TdeckMapView a, TdeckMapView b);
bool tdeck_map_point(TdeckMapView view, TdeckPosition position, int *x, int *y);
double tdeck_map_metres_per_pixel(TdeckMapView view);

/* Decode a square map tile into the display palette. The PNG input is scratch. */
bool tdeck_map_png(uint8_t *png, size_t size, uint8_t scan[TILE_SCAN_MAX],
                   uint16_t *output);

#endif
