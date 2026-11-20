#include "tdeck_replay.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

bool tdeck_replay_name(const char *file) {
    const char *ext=strchr(file,'.');
    if (!ext || ext==file || ext-file>8 || strcasecmp(ext,".JSL")) return false;
    return strspn(file,"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")==(size_t)(ext-file);
}

static int nibble(char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return -1;
}

static bool decode_line(const char *line, TdeckReplayFrame *frame, unsigned order) {
    /* Observation records are shallow. Bound recursion before handing them to cJSON. */
    unsigned depth=0;
    bool quote=false;
    for (const char *s=line; *s; s++) {
        if (quote && *s=='\\' && s[1]) s++;
        else if (*s=='"') quote=!quote;
        else if (!quote && (*s=='{' || *s=='[') && ++depth>8) return false;
        else if (!quote && (*s=='}' || *s==']')) {
            if (!depth) return false;
            depth--;
        }
    }
    cJSON *object=cJSON_ParseWithOpts(line,NULL,true);
    if (!object) return false;
    const cJSON *capture=cJSON_GetObjectItemCaseSensitive(object,"capture_ms");
    const cJSON *time=cJSON_GetArrayItem(capture,1);
    if (!time) time=cJSON_GetObjectItemCaseSensitive(object,"uptime_ms");
    const char *word=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object,"df17"));
    /* Use only representable integral millisecond values. */
    bool ok=cJSON_IsNumber(time) && time->valuedouble>=0 &&
        time->valuedouble<=9007199254740991.0 && floor(time->valuedouble)==time->valuedouble &&
        word && strlen(word)==28;
    if (ok) {
        *frame=(TdeckReplayFrame){.card={.ms=(int64_t)time->valuedouble,.length=14},.order=order};
        Card *card=&frame->card;
        for (unsigned i=0; i<14; i++) {
            int a=nibble(word[i*2]), b=nibble(word[i*2+1]);
            if (a<0 || b<0) { ok=false; break; }
            card->word[i]=a*16+b;
        }
        ok=ok && card->word[0]>>3==17 && card_valid(card);
        const char *local_time=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object,"local_time"));
        if (ok && tdeck_clock_valid_iso(local_time)) strcpy(frame->local_time,local_time);
    }
    cJSON_Delete(object);
    return ok;
}

static int name_order(const void *a, const void *b) { return strcasecmp(a,b); }

static void list_files(const char *directory, const char *live, bool (*progress)(void *),
                       void *context, TdeckReplayReply *reply) {
    DIR *dir=opendir(directory);
    if (!dir) return;
    struct dirent *entry;
    while (progress(context) && (entry=readdir(dir))) {
        if (!tdeck_replay_name(entry->d_name) || !strcasecmp(entry->d_name,live)) continue;
        if (reply->count==TDECK_REPLAY_FILE_LIMIT) { reply->more=true; break; }
        strcpy(reply->names[reply->count++],entry->d_name);
    }
    closedir(dir);
    if (!progress(context)) return;
    qsort(reply->names,reply->count,sizeof(*reply->names),name_order);
    reply->state=reply->count ? REPLAY_FILES : REPLAY_EMPTY;
}

static int by_icao(const TdeckReplayFrame *a, const TdeckReplayFrame *b) {
    return memcmp(a->card.word+1,b->card.word+1,3);
}

static int frame_order(const void *lhs, const void *rhs) {
    const TdeckReplayFrame *a=lhs,*b=rhs;
    int icao=by_icao(a,b);
    if (icao) return icao;
    if (a->card.ms!=b->card.ms) return (a->card.ms>b->card.ms)-(a->card.ms<b->card.ms);
    return (a->order>b->order)-(a->order<b->order);
}

static int latest_first(const void *lhs, const void *rhs) {
    const AircraftInfo *a=&((const TdeckReplayAircraft *)lhs)->info;
    const AircraftInfo *b=&((const TdeckReplayAircraft *)rhs)->info;
    if (a->seen!=b->seen) return (a->seen<b->seen)-(a->seen>b->seen);
    return (a->icao>b->icao)-(a->icao<b->icao);
}

static TdeckReplayState load_frames(FILE *file, bool (*progress)(void *), void *context,
                                    TdeckReplayFrame **frames, unsigned *count) {
    char line[1600];
    unsigned seen=0, capacity=0;
    while (progress(context) && fgets(line,sizeof(line),file)) {
        if (++seen%8==0) vTaskDelay(1);
        if (!strchr(line,'\n') && !feof(file)) {
            int ch;
            while ((ch=fgetc(file))!=EOF && ch!='\n') {
                if (++seen%1024==0) {
                    if (!progress(context)) return REPLAY_ERROR;
                    vTaskDelay(1);
                }
            }
            continue;
        }
        TdeckReplayFrame frame;
        if (!decode_line(line,&frame,*count)) continue;
        if (*count==TDECK_REPLAY_LIMIT) return REPLAY_TOO_LARGE;
        if (*count==capacity) {
            capacity=capacity ? capacity*2 : 64;
            TdeckReplayFrame *next=heap_caps_realloc(*frames,capacity*sizeof(*next),MALLOC_CAP_SPIRAM);
            if (!next) return REPLAY_ERROR;
            *frames=next;
        }
        (*frames)[(*count)++]=frame;
    }
    if (ferror(file) || !progress(context)) return REPLAY_ERROR;
    return *count ? REPLAY_READY : REPLAY_EMPTY;
}

static TdeckReplayState summarize(TdeckReplayFrame *frames, unsigned count, bool (*progress)(void *),
                                   void *context, TdeckReplayReply *reply) {
    qsort(frames,count,sizeof(*frames),frame_order);
    unsigned aircraft=1;
    for (unsigned i=1;i<count;i++) if (by_icao(&frames[i-1],&frames[i])) aircraft++;
    reply->aircraft=heap_caps_calloc(aircraft,sizeof(*reply->aircraft),MALLOC_CAP_SPIRAM);
    if (!reply->aircraft) return REPLAY_ERROR;
    for (unsigned first=0;first<count;) {
        unsigned end=first+1;
        while (end<count && !by_icao(&frames[first],&frames[end])) end++;
        if (!tdeck_replay_aircraft(frames+first,end-first,&reply->aircraft[reply->count],progress,context))
            return REPLAY_ERROR;
        reply->count++;
        first=end;
    }
    qsort(reply->aircraft,reply->count,sizeof(*reply->aircraft),latest_first);
    return REPLAY_READY;
}

void tdeck_replay_read(const char *directory, const TdeckReplayRequest *request,
                       const char *live, bool (*progress)(void *), void *context, TdeckReplayReply *reply) {
    *reply=(TdeckReplayReply){.ticket=request->ticket,.state=REPLAY_ERROR};
    if (!progress(context)) return;
    if (!*request->file) { list_files(directory,live,progress,context,reply); return; }
    if (!tdeck_replay_name(request->file) || !strcasecmp(request->file,live)) return;
    char path[64];
    snprintf(path,sizeof(path),"%s/%s",directory,request->file);
    FILE *file=fopen(path,"r");
    if (!file) return;
    TdeckReplayFrame *frames=NULL;
    reply->state=load_frames(file,progress,context,&frames,&reply->frames);
    fclose(file);
    if (reply->state==REPLAY_READY)
        reply->state=summarize(frames,reply->frames,progress,context,reply);
    free(frames);
    if (reply->state!=REPLAY_READY) {
        free(reply->aircraft);
        reply->aircraft=NULL;
        reply->count=0;
        reply->frames=0;
    }
}
