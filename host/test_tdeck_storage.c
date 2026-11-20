/* Inject a full writer queue and filesystem errors without an SD or a radio.
 * The production journal still formats, writes, fsyncs and closes each run. */
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "selftest.h"

static FILE *journal_file;
static int journal_fd;
typedef enum { DISK_OK, DISK_FULL, SYNC_FAILS, CLOSE_FAILS } DiskFault;
static DiskFault disk_fault;
static bool unmount_fails;
static unsigned unmounts, deselected;

static int open_journal(const char *path, int flags, ...) {
    assert(!strcmp(path,"/sdcard/SXADSB/00000001.JSL"));
    assert(flags==(O_WRONLY|O_CREAT|O_EXCL));
    journal_fd=dup(fileno(journal_file));
    return journal_fd;
}

static ssize_t write_journal(int fd, const void *data, size_t size) {
    if (disk_fault==DISK_FULL) { errno=ENOSPC; return -1; }
    return write(fd,data,size);
}

static int sync_journal(int fd) {
    if (disk_fault==SYNC_FAILS) { errno=EIO; return -1; }
    return fsync(fd);
}

static int close_journal(int fd) {
    int rc=close(fd);
    if (disk_fault==CLOSE_FAILS) { errno=EIO; return -1; }
    return rc;
}

#define TDECK_STORAGE_HOST_TEST
#define open open_journal
#define write write_journal
#define fsync sync_journal
#define close close_journal
#include "../main/tdeck_storage.c"
#undef open
#undef write
#undef fsync
#undef close

struct TestQueue {
    LogRecord records[BACKLOG];
    unsigned head, count;
};
static struct TestQueue writer_queue;
static unsigned next_result, total_results;

static int xQueueSend(QueueHandle_t q, const void *item, int timeout) {
    assert(q==queue && timeout==0);
    if (q->count==BACKLOG) return 0;
    q->records[(q->head+q->count)%BACKLOG]=*(const LogRecord *)item;
    q->count++;
    return pdTRUE;
}

static unsigned uxQueueMessagesWaiting(QueueHandle_t q) { return q->count; }
static unsigned uxQueueSpacesAvailable(QueueHandle_t q) { return BACKLOG-q->count; }

static void submit(unsigned number) {
    Result r={.method="fixture",.length=14,.capture={.block=number}};
    memcpy(r.word,garden_word,sizeof(r.word));
    tdeck_storage_record(&r,0,"2026-11-20T18:00:00");
}

static int xQueueReceive(QueueHandle_t q, void *item, int timeout) {
    (void)timeout;
    assert(q==queue);
    /* Give the field task a turn before every disk operation. Its single FIFO
     * also holds a post-cutoff packet, so a repeated stop must not extend it. */
    if (next_result<=total_results && !tdeck_storage_backpressured())
        submit(next_result++);
    if (!q->count) return 0;
    *(LogRecord *)item=q->records[q->head];
    q->head=(q->head+1)%BACKLOG;
    q->count--;
    return pdTRUE;
}

static int esp_vfs_fat_sdcard_unmount(const char *path, sdmmc_card_t *unused) {
    (void)unused;
    assert(!strcmp(path,"/sdcard"));
    assert(journal_fd<0 || (fcntl(journal_fd,F_GETFD)==-1 && errno==EBADF));
    unmounts++;
    return unmount_fails ? -1 : ESP_OK;
}

static void gpio_set_level(int pin, int level) {
    assert(unmounts==1 && pin==TDECK_SD_CS && level==1);
}

static void gpio_set_direction(int pin, int mode) {
    assert(unmounts==1 && pin==TDECK_SD_CS && mode==GPIO_MODE_OUTPUT);
    deselected++;
}

void tdeck_replay_read(const char *dir, const TdeckReplayRequest *req, const char *live,
                       bool (*progress)(void *), void *context, TdeckReplayReply *reply) {
    abort(); /* No replay is requested during these tests. */
}

static void run_case(unsigned queued, unsigned pending, DiskFault fault, bool fail_unmount) {
    assert(queued<=BACKLOG);
    writer_queue=(struct TestQueue){0};
    queue=&writer_queue;
    boot_id=1;
    atomic_store(&state,SD_READY);
    atomic_store(&queue_drops,0);
    atomic_store(&saved,0);
    journal_file=tmpfile();
    assert(journal_file);
    journal_fd=-1;
    disk_fault=fault;
    unmount_fails=fail_unmount;
    unmounts=deselected=0;
    for (unsigned i=0; i<queued; i++) submit(i+1);
    tdeck_storage_stop(pending);
    next_result=queued+1;
    total_results=queued+pending+1;
    tdeck_storage_stop(pending+1);
    assert(tdeck_storage_backpressured()==(pending && queued==BACKLOG));
    journal_task(NULL);
    TdeckStorageStatus sd=tdeck_storage_status();
    assert(!strcmp(sd.state,fail_unmount ? "error" : fault!=DISK_OK ? "stopped_error" : "stopped"));
    assert(unmounts==1 && deselected==1 && sd.queue_drops==0);
    unsigned written=fault==DISK_FULL ? 0 : queued+pending;
    assert(sd.saved==(fault==SYNC_FAILS ? 0 : written));
    rewind(journal_file);
    char line[1600];
    for (unsigned i=1; i<=written; i++) {
        char block[40];
        snprintf(block,sizeof(block),"\"block\":%u,",i);
        assert(fgets(line,sizeof(line),journal_file));
        assert(strstr(line,block) && strstr(line,"\"df17\":\"8d48c12b990ca5b098403ec9dd45\""));
    }
    assert(!fgets(line,sizeof(line),journal_file));
    fclose(journal_file);
}

int main(void) {
    run_case(BACKLOG,16,DISK_OK,false);
    run_case(0,0,DISK_OK,false);
    run_case(1,2,DISK_FULL,false);
    run_case(1,2,SYNC_FAILS,false);
    run_case(1,2,CLOSE_FAILS,false);
    run_case(1,2,DISK_OK,true);
    run_case(1,2,DISK_FULL,true);
    puts("SD shutdown: both queues drained in order, late packet excluded, write/sync/close/unmount faults distinguished");
    return 0;
}
