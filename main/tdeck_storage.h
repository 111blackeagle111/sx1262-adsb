#ifndef TDECK_STORAGE_H
#define TDECK_STORAGE_H

#include "receiver.h"
#include "tdeck_map.h"
#include "tdeck_replay.h"

enum { TDECK_SD_CS=39 };
enum { TDECK_SD_READ_BYTES=512 };

typedef struct {
    const char *state;
    unsigned saved, queue_drops;
} TdeckStorageStatus;

/* Mount on the shared SPI bus before the radio and TFT start using it. */
void tdeck_storage_start(uint32_t boot);
/* The field task is the only record producer and result FIFO consumer.
 * Preserve the FIFO prefix already accepted when the stop was requested. */
void tdeck_storage_record(const Result *result, unsigned deferred_drops, const char *local_time);
void tdeck_storage_stop(unsigned pending_results);
bool tdeck_storage_backpressured(void);
TdeckStorageStatus tdeck_storage_status(void);
/* Read a chunk of a closed session. The SD task replies on the USB console. */
bool tdeck_storage_read(uint32_t session, uint32_t offset);
/* The field task requests a view and copies it once the SD task publishes it. */
bool tdeck_storage_map_available(void);
bool tdeck_storage_map_view(TdeckMapView view, bool load, uint16_t *pixels, unsigned *tiles);
bool tdeck_storage_replay_request(const TdeckReplayRequest *request);
void tdeck_storage_replay_cancel(void);
bool tdeck_storage_replay_reply(TdeckReplayReply *reply);

#endif
