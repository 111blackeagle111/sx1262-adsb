#ifndef TDECK_CLOCK_H
#define TDECK_CLOCK_H
#include <stdbool.h>
#include <stdint.h>
enum { TDECK_TIME_LENGTH=20 };
typedef struct { int64_t set_ms, local_ms; bool valid; } TdeckClock;
bool tdeck_clock_set(TdeckClock *clock, const char *digits, int64_t now);
bool tdeck_clock_set_iso(TdeckClock *clock, const char *text, int64_t now);
bool tdeck_clock_restore(TdeckClock *clock, int64_t now);
bool tdeck_clock_format(const TdeckClock *clock, int64_t now, char text[TDECK_TIME_LENGTH]);
bool tdeck_clock_valid_iso(const char *text);
#ifdef ESP_PLATFORM
void tdeck_clock_load_saved(char text[TDECK_TIME_LENGTH]);
bool tdeck_clock_persist(const char *text);
#endif
#endif
