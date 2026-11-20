#include "tdeck_clock.h"
#include <string.h>
#include <time.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_rom_crc.h"
#include "esp_rtc_time.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stddef.h>

typedef struct {
    uint64_t rtc_ms;
    int64_t local_ms;
    uint32_t signature, checksum;
} RetainedClock;

#define CLOCK_SIGNATURE UINT32_C(0x54444301)
static RTC_NOINIT_ATTR RetainedClock retained;
static bool settings_available;
static const char clock_namespace[]="adsb_clock";
static const char clock_key[]="last_time";

static uint32_t clock_checksum(const RetainedClock *value) {
    return esp_rom_crc32_le(0,(const uint8_t *)value,offsetof(RetainedClock,checksum));
}
#endif

static int days_in_month(int year, int month) {
    static const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    return days[month-1]+(month==2 && year%4==0 && (year%100!=0 || year%400==0));
}

static int number(const char *s, unsigned n) {
    int value=0;
    while (n--) value=value*10+(*s++-'0');
    return value;
}

static bool valid_date(int year, int month, int day) {
    return year>0 && month>=1 && month<=12 && day>=1 && day<=days_in_month(year,month);
}

bool tdeck_clock_valid_iso(const char *text) {
    static const char format[]="0000-00-00T00:00:00";
    if (!text || strlen(text)!=sizeof(format)-1) return false;
    for (unsigned i=0;i<sizeof(format)-1;i++) {
        if (format[i]=='0' ? text[i]<'0' || text[i]>'9' : text[i]!=format[i]) return false;
    }
    return valid_date(number(text,4),number(text+5,2),number(text+8,2)) &&
           number(text+11,2)<24 && number(text+14,2)<60 && number(text+17,2)<60;
}

static void set_clock(TdeckClock *clock, int64_t now, int year, int month, int day,
                      int hour, int minute, int second) {
    int64_t elapsed=day-1;
    for (int y=1970; y<year; y++) elapsed+=337+days_in_month(y,2);
    for (int m=1; m<month; m++) elapsed+=days_in_month(year,m);
    *clock=(TdeckClock){.set_ms=now,.local_ms=(elapsed*86400+hour*3600+minute*60+second)*1000,.valid=true};
#ifdef ESP_PLATFORM
    RetainedClock anchor={
        .rtc_ms=esp_rtc_get_time_us()/1000,.local_ms=clock->local_ms,.signature=CLOCK_SIGNATURE
    };
    anchor.checksum=clock_checksum(&anchor);
    retained=anchor;
#endif
}

bool tdeck_clock_set(TdeckClock *clock, const char *digits, int64_t now) {
    if (strlen(digits)!=12 || strspn(digits,"0123456789")!=12) return false;
    int year=number(digits,4), month=number(digits+4,2), day=number(digits+6,2);
    int hour=number(digits+8,2), minute=number(digits+10,2);
    if (year<2000 || year>2099 || !valid_date(year,month,day) ||
        hour>23 || minute>59) return false;
    set_clock(clock,now,year,month,day,hour,minute,0);
    return true;
}

bool tdeck_clock_set_iso(TdeckClock *clock, const char *text, int64_t now) {
    if (!tdeck_clock_valid_iso(text) || number(text,4)<2000) return false;
    set_clock(clock,now,number(text,4),number(text+5,2),number(text+8,2),
              number(text+11,2),number(text+14,2),number(text+17,2));
    return true;
}

bool tdeck_clock_restore(TdeckClock *clock, int64_t now) {
    *clock=(TdeckClock){0};
#ifdef ESP_PLATFORM
    esp_reset_reason_t reset=esp_reset_reason();
    if (reset!=ESP_RST_POWERON && reset!=ESP_RST_BROWNOUT &&
        retained.signature==CLOCK_SIGNATURE && retained.checksum==clock_checksum(&retained)) {
        uint64_t rtc=esp_rtc_get_time_us()/1000;
        if (rtc>=retained.rtc_ms && retained.local_ms>=0 &&
            rtc-retained.rtc_ms<=(uint64_t)(INT64_MAX-retained.local_ms)) {
            *clock=(TdeckClock){.set_ms=now,.local_ms=retained.local_ms+(int64_t)(rtc-retained.rtc_ms),
                                .valid=true};
            return true;
        }
    }
    /* A stale anchor must never be picked up after a later software reset. */
    retained.signature=0;
#else
    (void)now;
#endif
    return false;
}

bool tdeck_clock_format(const TdeckClock *clock, int64_t now, char text[TDECK_TIME_LENGTH]) {
    text[0]=0;
    if (!clock->valid || now<clock->set_ms) return false;
    /* Civil local time as entered by the user, with no implied UTC offset. */
    int64_t delta=now-clock->set_ms;
    if (delta>INT64_MAX-clock->local_ms) return false;
    time_t ticks=(clock->local_ms+delta)/1000;
    struct tm date;
    return gmtime_r(&ticks,&date) &&
           strftime(text,TDECK_TIME_LENGTH,"%Y-%m-%dT%H:%M:%S",&date)==TDECK_TIME_LENGTH-1;
}

#ifdef ESP_PLATFORM
void tdeck_clock_load_saved(char text[TDECK_TIME_LENGTH]) {
    text[0]=0;
    settings_available=nvs_flash_init()==ESP_OK;
    nvs_handle_t handle;
    if (!settings_available || nvs_open(clock_namespace,NVS_READONLY,&handle)!=ESP_OK) return;
    size_t length=TDECK_TIME_LENGTH;
    esp_err_t result=nvs_get_str(handle,clock_key,text,&length);
    nvs_close(handle);
    if (result!=ESP_OK || !tdeck_clock_valid_iso(text) || number(text,4)<2000) text[0]=0;
}

bool tdeck_clock_persist(const char *text) {
    if (!settings_available) settings_available=nvs_flash_init()==ESP_OK;
    nvs_handle_t handle;
    if (!settings_available || nvs_open(clock_namespace,NVS_READWRITE,&handle)!=ESP_OK) return false;
    esp_err_t result=nvs_set_str(handle,clock_key,text);
    if (result==ESP_OK) result=nvs_commit(handle);
    nvs_close(handle);
    return result==ESP_OK;
}
#endif
