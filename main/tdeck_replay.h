#ifndef TDECK_REPLAY_H
#define TDECK_REPLAY_H
#include "history.h"
#include "tdeck_clock.h"

enum { TDECK_REPLAY_NAME=13, TDECK_REPLAY_FILE_LIMIT=32, TDECK_REPLAY_LIMIT=8192 };
typedef struct {
    Card card;
    unsigned order;
    char local_time[TDECK_TIME_LENGTH];
} TdeckReplayFrame;

typedef struct {
    AircraftInfo info;
    char received_at[TDECK_TIME_LENGTH], position_at[TDECK_TIME_LENGTH];
} TdeckReplayAircraft;
typedef enum {
    REPLAY_IDLE, REPLAY_LOADING, REPLAY_FILES, REPLAY_READY, REPLAY_NO_SD,
    REPLAY_EMPTY, REPLAY_ERROR, REPLAY_TOO_LARGE
} TdeckReplayState;
typedef struct {
    uint32_t ticket;
    char file[TDECK_REPLAY_NAME];
} TdeckReplayRequest;
typedef struct {
    uint32_t ticket;
    TdeckReplayState state;
    unsigned count, frames;
    bool more;
    char names[TDECK_REPLAY_FILE_LIMIT][TDECK_REPLAY_NAME];
    TdeckReplayAircraft *aircraft;
} TdeckReplayReply;
/* Frames are one ICAO in reception order. Store the last available measurements. */
bool tdeck_replay_aircraft(const TdeckReplayFrame *frames, unsigned count,
                           TdeckReplayAircraft *aircraft, bool (*progress)(void *), void *context);
bool tdeck_replay_name(const char *file);
void tdeck_replay_read(const char *directory, const TdeckReplayRequest *request,
                       const char *live, bool (*progress)(void *), void *context, TdeckReplayReply *reply);

#endif
