#ifndef LILYGO_HISTORY_H
#define LILYGO_HISTORY_H
#include "receiver.h"

enum { CARDS = 64, LINES = 8, COLUMNS = 16 };
typedef struct {
    uint32_t boot;
    int64_t ms;
    uint8_t word[14], length;
    unsigned test_count;
} Card;

typedef char Screen[LINES][COLUMNS+1];
typedef enum { NO_CARD, TEST_CARD, AIRCRAFT_CARD, POSITION_CARD } CardView;
void history_add(Card c);
void history_clear(void);
void history_set_boot(uint32_t boot);
void history_older(void);
void history_live(void);
CardView history_screen(Screen screen, uint32_t boot, int64_t now);
bool card_valid(const Card *c);
/* Live aircraft view for the color display, in descending reception order. */
typedef struct {
    uint32_t icao;
    int64_t seen;
    char callsign[9], altitude[32], speed[32], heading[17];
    bool fixed;
    double lat, lon;
} AircraftInfo;
unsigned history_aircraft(uint32_t boot, uint32_t addresses[CARDS]);
bool history_aircraft_info(uint32_t boot, uint32_t icao, int64_t now, AircraftInfo *info);
#endif
