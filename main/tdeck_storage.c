/* Keep FAT and SPI card I/O out of the radio, decoder and display tasks. */
#include "tdeck_storage.h"
#include "tdeck_clock.h"
#ifdef TDECK_STORAGE_HOST_TEST
#include "test_tdeck_storage_port.h"
#else
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { SD_UNAVAILABLE, SD_READY, SD_DRAINING, SD_STOPPING, SD_STOPPED, SD_STOPPED_ERROR, SD_ERROR };
enum { BACKLOG=16, SYNC_RECORDS=8, SYNC_US=4000000, MAP_MISSING=0x1926 };
static const char mountpoint[]="/sdcard";
static const char directory[]="/sdcard/SXADSB";
static const char *const TAG="sd journal";
typedef struct {
    Result result;
    unsigned deferred_drops;
    char local_time[TDECK_TIME_LENGTH];
} LogRecord;
static QueueHandle_t queue;
typedef struct { uint32_t session, offset; } JournalRead;
static QueueHandle_t read_requests;
static sdmmc_card_t *card;
static uint32_t boot_id;
static atomic_int state;
static atomic_uint saved, queue_drops;
static unsigned remaining_results;  /* Field task only. */
static QueueHandle_t map_requests;
static QueueHandle_t replay_requests, replay_replies;
static atomic_uint replay_wanted;
static SemaphoreHandle_t map_lock;
static uint16_t *map_front, *map_back;
static bool map_published, map_complete;
static TdeckMapView map_view;
static unsigned map_tile_count;

typedef struct {
    int x,y;
    unsigned zoom, used;
    bool valid;
    uint16_t *pixels;
} MapTile;
static MapTile cache[8];
static unsigned tile_clock;
static uint8_t *png, *scan;

static bool load_tile(MapTile *tile) {
    if (!tile->pixels) tile->pixels=heap_caps_malloc(TILE_SIZE*TILE_SIZE*sizeof(uint16_t),MALLOC_CAP_SPIRAM);
    if (!png) png=heap_caps_malloc(TILE_PNG_MAX,MALLOC_CAP_SPIRAM);
    if (!scan) scan=heap_caps_malloc(TILE_SCAN_MAX,MALLOC_CAP_SPIRAM);
    if (!tile->pixels || !png || !scan) return false;
    char path[64];
    snprintf(path,sizeof(path),"/sdcard/maps/osm/%u/%d/%d.png",tile->zoom,tile->x,tile->y);
    int fd=open(path,O_RDONLY);
    if (fd<0 && errno==ENOENT) {
        snprintf(path,sizeof(path),"/sdcard/map/%u/%d/%d.png",tile->zoom,tile->x,tile->y);
        fd=open(path,O_RDONLY);
    }
    if (fd<0) {
        if (errno!=ENOENT) ESP_LOGW(TAG,"Cannot open map tile: %s",strerror(errno));
        return false;
    }
    size_t size=0;
    ssize_t n=0;
    while (size<TILE_PNG_MAX && atomic_load(&state)==SD_READY) {
        size_t room=TILE_PNG_MAX-size;
        n=read(fd,png+size,room<1024?room:1024);
        if (n<=0) break;
        size+=n;
        /* Release the shared bus between short reads. */
        vTaskDelay(1);
    }
    close(fd);
    if (n!=0 || size==TILE_PNG_MAX || atomic_load(&state)!=SD_READY) return false;
    if (tdeck_map_png(png,size,scan,tile->pixels)) return true;
    ESP_LOGW(TAG,"Cannot decode map tile (%u bytes)",(unsigned)size);
    return false;
}

static const uint16_t *map_tile(unsigned zoom, int x, int y) {
    MapTile *slot=&cache[0];
    for (unsigned i=0; i<sizeof(cache)/sizeof(*cache); i++) {
        MapTile *tile=&cache[i];
        if (tile->used && tile->zoom==zoom && tile->x==x && tile->y==y) {
            tile->used=++tile_clock;
            return tile->valid?tile->pixels:NULL;
        }
        if (tile->used<slot->used) slot=tile;
    }
    slot->zoom=zoom; slot->x=x; slot->y=y; slot->used=++tile_clock;
    slot->valid=load_tile(slot);
    return slot->valid?slot->pixels:NULL;
}

static void publish_map(TdeckMapView view) {
    unsigned tiles=0;
    if (!map_back) map_back=heap_caps_malloc(MAP_WIDTH*MAP_HEIGHT*sizeof(uint16_t),MALLOC_CAP_SPIRAM);
    if (map_back) {
        unsigned depth=MAP_WIDTH*MAP_HEIGHT;
        for (unsigned i=0; i<depth; i++) map_back[i]=MAP_MISSING;
        for (int y=view.top/TILE_SIZE; y<=(view.top+MAP_HEIGHT-1)/TILE_SIZE; y++) {
            for (int x=view.left/TILE_SIZE; x<=(view.left+MAP_WIDTH-1)/TILE_SIZE; x++) {
                if (atomic_load(&state)!=SD_READY || uxQueueMessagesWaiting(map_requests)) return;
                const uint16_t *tile=map_tile(view.zoom,x,y);
                if (!tile) continue;
                int dx=x*TILE_SIZE-view.left, dy=y*TILE_SIZE-view.top;
                int left=dx<0?0:dx, right=dx+TILE_SIZE<MAP_WIDTH?dx+TILE_SIZE:MAP_WIDTH;
                int top=dy<0?0:dy, bottom=dy+TILE_SIZE<MAP_HEIGHT?dy+TILE_SIZE:MAP_HEIGHT;
                for (int row=top; row<bottom; row++)
                    memcpy(map_back+row*MAP_WIDTH+left,tile+(row-dy)*TILE_SIZE+left-dx,
                           (right-left)*sizeof(uint16_t));
                tiles++;
                if (tiles==1 && !map_published) {
                    uint16_t *partial=heap_caps_malloc(depth*sizeof(*partial),MALLOC_CAP_SPIRAM);
                    if (partial) {
                        memcpy(partial,map_back,depth*sizeof(*partial));
                        xSemaphoreTake(map_lock,portMAX_DELAY);
                        map_front=partial;
                        map_view=view;
                        map_tile_count=1;
                        map_published=true;
                        map_complete=false;
                        xSemaphoreGive(map_lock);
                    }
                }
            }
        }
    }
    xSemaphoreTake(map_lock,portMAX_DELAY);
    uint16_t *previous=map_front;
    map_front=map_back;
    map_back=previous;
    map_view=view;
    map_tile_count=tiles;
    map_published=true;
    map_complete=true;
    xSemaphoreGive(map_lock);
}

static void deselect_card(void) {
    /* The SD driver leaves CS as a floating input when it removes the device. */
    gpio_set_level(TDECK_SD_CS,1);
    gpio_set_direction(TDECK_SD_CS,GPIO_MODE_OUTPUT);
}

static void unmount_card(bool journal_ok) {
    esp_err_t err=esp_vfs_fat_sdcard_unmount(mountpoint,card);
    deselect_card();
    if (err!=ESP_OK) {
        ESP_LOGW(TAG,"Unmount failed: %s",esp_err_to_name(err));
        atomic_store(&state,SD_ERROR);
        return;
    }
    atomic_store(&state,journal_ok ? SD_STOPPED : SD_STOPPED_ERROR);
}

static bool append_record(int fd, const LogRecord *record) {
    char line[1540];
    const Result *r=&record->result;
    observation(line,sizeof(line),r,boot_id,r->capture.last_ms,0,record->deferred_drops);
    size_t size=strlen(line);
    if (*record->local_time && size && line[size-1]=='}') {
        size--;
        int extra=snprintf(line+size,sizeof(line)-size,",\"local_time\":\"%s\"}",record->local_time);
        if (extra<0 || (size_t)extra>=sizeof(line)-size-1) return false;
        size+=(size_t)extra;
    }
    line[size++]='\n';
    const char *next=line;
    while (size) {
        ssize_t n=write(fd,next,size);
        if (n<=0) return false;
        next+=n;
        size-=n;
    }
    return true;
}

static bool sd_ready(void) { return atomic_load(&state)==SD_READY; }

typedef struct {
    int fd;
    unsigned pending;
    int64_t last_sync;
    char live_file[TDECK_REPLAY_NAME];
    uint32_t request;
    bool ok;
} Journal;

static bool journal_step(Journal *journal, const LogRecord *record, bool closing) {
    if (record) {
        if (journal->fd<0) {
            char path[40];
            snprintf(path,sizeof(path),"%s/%s",directory,journal->live_file);
            /* A boot ID collision must never overwrite another session. */
            journal->fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0666);
        }
        if (journal->fd<0 || !append_record(journal->fd,record)) return false;
        journal->pending++;
    }
    int64_t now=esp_timer_get_time();
    if (journal->pending && (journal->pending>=SYNC_RECORDS || now-journal->last_sync>=SYNC_US || closing)) {
        if (fsync(journal->fd)<0) return false;
        atomic_fetch_add(&saved,journal->pending);
        journal->pending=0;
        journal->last_sync=now;
    }
    return true;
}

/* At most the reader and journal are open. While loading a long recording,
 * keep servicing the live log between records and respect cancel or unmount. */
static bool replay_progress(void *context) {
    Journal *journal=context;
    if (!journal->ok || !sd_ready() || atomic_load(&replay_wanted)!=journal->request) return false;
    LogRecord record;
    bool received=xQueueReceive(queue,&record,0)==pdTRUE;
    journal->ok=journal_step(journal,received ? &record : NULL,false);
    return journal->ok;
}

static void reply_to_replay(const TdeckReplayRequest *request, Journal *journal) {
    TdeckReplayReply reply, unused;
    journal->request=request->ticket;
    tdeck_replay_read(directory,request,journal->live_file,replay_progress,journal,&reply);
    if (xQueueReceive(replay_replies,&unused,0)==pdTRUE) free(unused.aircraft);
    if (xQueueSend(replay_replies,&reply,0)!=pdTRUE) free(reply.aircraft);
}

static void reply_to_read(JournalRead request) {
    char path[40], hex[2*TDECK_SD_READ_BYTES+1];
    uint8_t bytes[TDECK_SD_READ_BYTES];
    const char *state="error";
    ssize_t count=-1;
    if (request.session==boot_id) state="active";
    else {
        snprintf(path,sizeof(path),"%s/%08"PRIX32".JSL",directory,request.session);
        int fd=open(path,O_RDONLY);
        if (fd>=0) {
            if (lseek(fd,request.offset,SEEK_SET)==(off_t)request.offset)
                count=read(fd,bytes,sizeof(bytes));
            if (close(fd)<0) count=-1;
            if (count>=0) state=count ? "ok" : "eof";
        } else if (errno==ENOENT) state="missing";
    }
    if (count>0) {
        static const char digits[]="0123456789abcdef";
        for (ssize_t i=0;i<count;i++) {
            hex[2*i]=digits[bytes[i]>>4];
            hex[2*i+1]=digits[bytes[i]&15];
        }
        hex[2*count]=0;
    }
    printf("SXSDREAD session=%08"PRIX32" offset=%"PRIu32" state=%s data=%s\n",
           request.session,request.offset,state,count>0 ? hex : "-");
}

static void journal_task(void *unused) {
    (void)unused;
    Journal journal={.fd=-1,.last_sync=esp_timer_get_time(),.ok=true};
    snprintf(journal.live_file,sizeof(journal.live_file),"%08"PRIX32".JSL",boot_id);
    for (;;) {
        bool stopping=atomic_load(&state)==SD_STOPPING;
        LogRecord record;
        bool received=xQueueReceive(queue,&record,stopping?0:pdMS_TO_TICKS(80))==pdTRUE;
        journal.ok=journal_step(&journal,received ? &record : NULL,stopping && !received);
        if (!journal.ok) break;
        if (stopping && !received) break;
        JournalRead read_request;
        if (sd_ready() && read_requests && xQueueReceive(read_requests,&read_request,0)==pdTRUE)
            reply_to_read(read_request);
        TdeckReplayRequest request;
        if (!received && sd_ready() && replay_requests && replay_replies &&
            xQueueReceive(replay_requests,&request,0)==pdTRUE) {
            reply_to_replay(&request,&journal);
            if (!journal.ok) break;
            continue;
        }
        TdeckMapView view;
        if (!received && atomic_load(&state)==SD_READY && map_requests &&
            xQueueReceive(map_requests,&view,0)==pdTRUE) publish_map(view);
    }
    if (!journal.ok) {
        ESP_LOGW(TAG,"Write or sync failed: %s",strerror(errno));
        atomic_store(&state,SD_STOPPING);
    }
    if (journal.fd>=0 && close(journal.fd)<0) journal.ok=false;
    unmount_card(journal.ok);
    vTaskDelete(NULL);
}

void tdeck_storage_start(uint32_t boot) {
    sdmmc_host_t host=SDSPI_HOST_DEFAULT();
    host.slot=SPI2_HOST;
    host.max_freq_khz=10000;
    sdspi_device_config_t device=SDSPI_DEVICE_CONFIG_DEFAULT();
    device.host_id=SPI2_HOST;
    device.gpio_cs=TDECK_SD_CS;
    const esp_vfs_fat_sdmmc_mount_config_t config={
        .format_if_mount_failed=false,
        .max_files=2, /* Journal stays open while a tile is being read. */
    };
    esp_err_t err=esp_vfs_fat_sdspi_mount(mountpoint,&host,&device,&config,&card);
    if (err!=ESP_OK) {
        deselect_card();
        ESP_LOGI(TAG,"Card unavailable: %s",esp_err_to_name(err));
        return;
    }
    boot_id=boot;
    map_lock=xSemaphoreCreateMutex();
    if (map_lock) map_requests=xQueueCreate(1,sizeof(TdeckMapView));
    replay_requests=xQueueCreate(1,sizeof(TdeckReplayRequest));
    replay_replies=xQueueCreate(1,sizeof(TdeckReplayReply));
    read_requests=xQueueCreate(1,sizeof(JournalRead));
    if ((mkdir(directory,0777)==0 || errno==EEXIST) &&
        (queue=xQueueCreate(BACKLOG,sizeof(LogRecord))) &&
        /* The decoder can stay runnable on core 1 through RF bursts. Let SD run on
         * either core when idle, below the radio, decoder and field tasks. */
        xTaskCreate(journal_task,"sd journal",10240,NULL,2,NULL)==pdPASS) {
        atomic_store(&state,SD_READY);
        return;
    }
    if (queue) vQueueDelete(queue);
    unmount_card(false);
}

/* The writer only frees slots, so a slot observed here remains available
 * to the field task. While draining, keep the next result in its FIFO until
 * there is room, even if the SD queue was full at the stop request. */
bool tdeck_storage_backpressured(void) {
    return atomic_load(&state)==SD_DRAINING && uxQueueSpacesAvailable(queue)==0;
}

void tdeck_storage_record(const Result *result, unsigned deferred_drops, const char *local_time) {
    int phase=atomic_load(&state);
    if (phase!=SD_READY && phase!=SD_DRAINING) return;
    LogRecord record={.result=*result,.deferred_drops=deferred_drops};
    snprintf(record.local_time,sizeof(record.local_time),"%s",local_time);
    if (xQueueSend(queue,&record,0)!=pdTRUE) atomic_fetch_add(&queue_drops,1);
    if (phase==SD_DRAINING && --remaining_results==0)
        atomic_compare_exchange_strong(&state,&phase,SD_STOPPING);
}

bool tdeck_storage_replay_request(const TdeckReplayRequest *request) {
    if (!replay_requests || !replay_replies || !sd_ready()) return false;
    atomic_store(&replay_wanted,request->ticket);
    return xQueueOverwrite(replay_requests,request)==pdTRUE;
}

void tdeck_storage_replay_cancel(void) { atomic_store(&replay_wanted,0); }

bool tdeck_storage_replay_reply(TdeckReplayReply *reply) {
    return replay_replies && xQueueReceive(replay_replies,reply,0)==pdTRUE;
}

bool tdeck_storage_read(uint32_t session, uint32_t offset) {
    JournalRead request={session,offset};
    return read_requests && sd_ready() && xQueueSend(read_requests,&request,0)==pdTRUE;
}

void tdeck_storage_stop(unsigned pending_results) {
    int expected=SD_READY;
    if (atomic_compare_exchange_strong(&state,&expected,pending_results ? SD_DRAINING : SD_STOPPING))
        remaining_results=pending_results;
}

TdeckStorageStatus tdeck_storage_status(void) {
    static const char *const names[]={"unavailable","ready","stopping","stopping","stopped","stopped_error","error"};
    return (TdeckStorageStatus){
        .state=names[atomic_load(&state)],.saved=atomic_load(&saved),
        .queue_drops=atomic_load(&queue_drops)
    };
}

bool tdeck_storage_map_available(void) {
    return map_requests && atomic_load(&state)==SD_READY;
}

/* Reproject the last viewport while the SD task builds the next one. */
static bool preview_map(TdeckMapView view, uint16_t *pixels) {
    int out_scale=1, in_scale=1;
    if (view.zoom>map_view.zoom) out_scale<<=view.zoom-map_view.zoom;
    else in_scale<<=map_view.zoom-view.zoom;
    int xs[MAP_WIDTH];
    bool overlap=false;
    for (int x=0; x<MAP_WIDTH; x++)
        xs[x]=(view.left+x)*in_scale/out_scale-map_view.left;
    for (int y=0; y<MAP_HEIGHT; y++) {
        int source_y=(view.top+y)*in_scale/out_scale-map_view.top;
        for (int x=0; x<MAP_WIDTH; x++) {
            bool have=source_y>=0 && source_y<MAP_HEIGHT && xs[x]>=0 && xs[x]<MAP_WIDTH;
            pixels[y*MAP_WIDTH+x]=have ? map_front[source_y*MAP_WIDTH+xs[x]] : MAP_MISSING;
            overlap|=have;
        }
    }
    return overlap;
}

bool tdeck_storage_map_view(TdeckMapView view, bool load, uint16_t *pixels, unsigned *tiles) {
    static TdeckMapView requested;
    static bool have_request;
    *tiles=0;
    if (!tdeck_storage_map_available()) return false;
    if (xSemaphoreTake(map_lock,0)==pdTRUE) {
        bool exact=map_published && tdeck_map_same(view,map_view);
        bool ready=exact && map_complete;
        if (exact && map_front && pixels) {
            memcpy(pixels,map_front,MAP_WIDTH*MAP_HEIGHT*sizeof(*pixels));
            *tiles=map_tile_count;
        } else if (map_published && map_front && map_tile_count && pixels && preview_map(view,pixels))
            *tiles=map_tile_count;
        xSemaphoreGive(map_lock);
        if (ready) return true;
    }
    if (load && (!have_request || !tdeck_map_same(view,requested))) {
        xQueueOverwrite(map_requests,&view);
        requested=view;
        have_request=true;
    }
    return false;
}
