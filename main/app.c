/* LilyGO ESP32-S3 / SX1262 passive Mode S receiver.
 *
 * Core 0 owns the radio. It polls for a finished capture, copies it, and
 * rearms. Core 1 runs the decoder. Neither side shares a mutable waveform:
 * FreeRTOS copies each Capture into the queue. The third, low-priority task
 * owns the screen, button, flash and USB. It can sleep through every bit. */
#include "receiver.h"
#include "radio.h"
#include "tuning.h"
#ifdef ADS_B_TDECK_PLUS
#include "nvs.h"
#include "nvs_flash.h"
#include "tdeck_display.h"
#include "tdeck_clock.h"
#include "tdeck_keyboard.h"
#include "tdeck_storage.h"
#include "gps_nmea.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include <strings.h>
#if __has_include("driver/usb_serial_jtag_vfs.h")
#include "driver/usb_serial_jtag_vfs.h"
#else
#include "esp_vfs_usb_serial_jtag.h"
#endif
#else
#include "display.h"
#include "storage.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#endif
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { QUEUE_DEPTH=96, UNCHECKED=8, BOOT_BUTTON=0 };
static QueueHandle_t captures, results;
static uint32_t boot_id;
static atomic_uint bursts, polls, restarts, full_queue, high_water, max_rearm_us, deferred, filtered;
static atomic_bool ready;
static int64_t now_ms(void) { return esp_timer_get_time()/1000; }

static void wake_radio(void *task) { xTaskNotifyGive((TaskHandle_t)task); }

/* Frequency changes cross the cores as requests. Only the radio task touches
 * the synthesizer. "Applied" means calibration and rearming both succeeded. */
static atomic_uint requested_hz=DEFAULT_HZ, applied_hz, failed_hz;
#ifdef ADS_B_TDECK_PLUS
static atomic_uint df17_count, console_drops, noise_floor, trigger_level;
static QueueHandle_t latest_capture;
typedef struct {
    Capture capture;
    char admission;
} RawCapture;
static QueueHandle_t raw_captures;
static atomic_bool raw_enabled;
static atomic_uint raw_queue_drops;
static unsigned raw_exported, raw_usb_drops;
#else
static atomic_bool watching;
static Menu menu;
static uint32_t saved_hz=DEFAULT_HZ;
static bool settings_ok;
static Scope scope;
static ScopeView view;
static portMUX_TYPE scope_lock=portMUX_INITIALIZER_UNLOCKED;

static void load_dial(void) {
    nvs_handle_t handle;
    settings_ok=nvs_flash_init()==ESP_OK;
    if (!settings_ok || nvs_open("adsb",NVS_READONLY,&handle)!=ESP_OK) return;
    uint32_t hz;
    if (nvs_get_u32(handle,"dial",&hz)==ESP_OK && hz>=MIN_HZ && hz<=MAX_HZ) saved_hz=hz;
    nvs_close(handle);
    atomic_store(&requested_hz,saved_hz);
}

static bool save_dial(uint32_t hz) {
    if (saved_hz==hz) return true;
    nvs_handle_t h;
    if (!settings_ok || nvs_open("adsb",NVS_READWRITE,&h)!=ESP_OK) return false;
    esp_err_t err=nvs_set_u32(h,"dial",hz);
    if (err==ESP_OK) err=nvs_commit(h);
    nvs_close(h);
    if (err==ESP_OK) saved_hz=hz;
    return err==ESP_OK;
}
#endif

static esp_err_t arm_channel(Capture *c) {
    uint32_t requested=atomic_load(&requested_hz);
    if (requested!=c->dial_hz) {
        esp_err_t err=radio_tune(requested);
        if (err==ESP_OK) c->dial_hz=requested;
        else {
            atomic_store(&failed_hz,requested);
            /* A rejected dial must not strand the receiver out of lock. */
            unsigned failed=requested;
            atomic_compare_exchange_strong(&requested_hz,&failed,c->dial_hz);
            err=radio_tune(c->dial_hz);
            if (err!=ESP_OK) return err;
        }
    }
    esp_err_t err=radio_calibrate(&c->floor,&c->threshold);
    if (err==ESP_OK) err=radio_arm(c->threshold);
    if (err==ESP_OK) {
#ifdef ADS_B_TDECK_PLUS
        atomic_store(&noise_floor,c->floor);
        atomic_store(&trigger_level,c->threshold);
#endif
        atomic_store(&applied_hz,c->dial_hz);
    }
    return err;
}

#ifndef ADS_B_TDECK_PLUS
static esp_err_t sample_scope(uint32_t hz, int64_t now, const Capture *burst) {
    static int64_t last=-1;
    if (!atomic_load(&watching) || (now==last && !burst)) return ESP_OK;
    uint8_t code;
    esp_err_t err=radio_rssi(&code);
    if (err!=ESP_OK) return err;
    last=now;
    portENTER_CRITICAL(&scope_lock);
    scope_sample(&scope,hz,now,code,burst);
    portEXIT_CRITICAL(&scope_lock);
    return ESP_OK;
}
#endif

static esp_err_t listen(void) {
    Capture c={.dial_hz=DEFAULT_HZ};
    esp_err_t err=radio_start(false);
    if (err==ESP_OK) err=arm_channel(&c);
    if (err!=ESP_OK) return err;
    atomic_store(&ready,true);
    printf("SXREADY %08"PRIx32"\n",boot_id);
    int64_t last_armed=now_ms(), calibrated=last_armed;
    while (true) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        uint8_t state;
        err=radio_state(&state);
        if (err!=ESP_OK) return err;
        atomic_fetch_add(&polls,1);
        int64_t now=now_ms(), detected=esp_timer_get_time();
        if (state==SCAN_COMPLETE) {
            c.first_ms=last_armed;
            c.last_ms=now;
            err=radio_read(c.raw);
            if (err!=ESP_OK) return err;
            c.block=atomic_fetch_add(&bursts,1)+1;
        } else if (state!=SCAN_ARMED) return ESP_ERR_INVALID_RESPONSE;

#ifndef ADS_B_TDECK_PLUS
        err=sample_scope(c.dial_hz,now,state==SCAN_COMPLETE ? &c : NULL);
        if (err!=ESP_OK) return err;
#endif
        bool recalibrate=c.dial_hz!=atomic_load(&requested_hz) || now-calibrated>=60000;
        /* Start the next capture before spending time on this one. During
         * recalibration, copy the completed capture before changing its metadata. */
        if (state==SCAN_COMPLETE && !recalibrate) {
            if ((err=radio_arm(c.threshold))!=ESP_OK) return err;
        }
        int64_t rearmed=esp_timer_get_time();
        if (state==SCAN_COMPLETE) {
#ifdef ADS_B_TDECK_PLUS
            xQueueOverwrite(latest_capture,&c);
#endif
            unsigned waiting=uxQueueMessagesWaiting(captures);
            ReplyShape needed=waiting<QUEUE_DEPTH/2 ? SHORT_REPLY : LONG_REPLY;
            bool rejected=waiting>=UNCHECKED && decoder_candidate(&c)<needed;
            bool dropped=false;
            if (rejected) {
                atomic_fetch_add(&filtered,1);
            } else if (xQueueSend(captures,&c,0)!=pdTRUE) {
                atomic_fetch_add(&full_queue,1);
                dropped=true;
            } else {
                unsigned depth=uxQueueMessagesWaiting(captures);
                if (depth>atomic_load(&high_water)) atomic_store(&high_water,depth);
            }
#ifdef ADS_B_TDECK_PLUS
            if (atomic_load(&raw_enabled)) {
                RawCapture record={.capture=c,.admission=rejected?'F':dropped?'D':'Q'};
                if (xQueueSend(raw_captures,&record,0)!=pdTRUE) atomic_fetch_add(&raw_queue_drops,1);
            }
#else
            (void)dropped;
#endif
        }
        if (recalibrate) {
            if (state==SCAN_ARMED && (err=radio_finish())!=ESP_OK) return err;
            err=arm_channel(&c);
            if (err!=ESP_OK) return err;
            rearmed=esp_timer_get_time();
            calibrated=rearmed/1000;
        }
        last_armed=rearmed/1000;
        if (state==SCAN_COMPLETE) {
            unsigned us=rearmed-detected;
            if (us>atomic_load(&max_rearm_us)) atomic_store(&max_rearm_us,us);
        }
    }
}

static void radio_task(void *unused) {
#ifndef ADS_B_TDECK_PLUS
    ESP_ERROR_CHECK(radio_bus_init());
#endif
    esp_timer_handle_t timer;
    esp_timer_create_args_t args={.callback=wake_radio,.arg=xTaskGetCurrentTaskHandle(),
                                  .name="poll radio",.skip_unhandled_events=true};
    ESP_ERROR_CHECK(esp_timer_create(&args,&timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer,250));
    while (true) {
        esp_err_t err=radio_self_test();
        if (err==ESP_OK) err=listen();
        atomic_store(&ready,false);
        atomic_fetch_add(&restarts,1);
        printf("SXERROR radio %s\n",esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#ifdef ADS_B_TDECK_PLUS
static void report(const Result *r) {
    if (r->length!=14 || r->word[0]>>3!=17 || r->evidence_index || modes_crc(r->word,14)) return;
    atomic_fetch_add(&df17_count,1);
    if (xQueueSend(results,r,0)!=pdTRUE) atomic_fetch_add(&console_drops,1);
}
#else
/* A window of noise can wait for the next periodic trace. An accepted
 * aircraft word always goes through to the log and the display. */
static void report(const Result *r) {
    static int64_t last_report=-1000;
    int64_t now=now_ms();
    if (!r->length && !r->score && now-last_report<250) return;
    if (xQueueSend(results,r,r->length?portMAX_DELAY:0)==pdTRUE) last_report=now;
}
#endif

static void decode_task(void *unused) {
    Capture c;
    while (true) {
        if (xQueueReceive(captures,&c,0)) decoder_capture(&c);
        else if (!decoder_idle(now_ms()) && xQueueReceive(captures,&c,pdMS_TO_TICKS(10)))
            decoder_capture(&c);
        atomic_store(&deferred,decoder_evictions());
        vTaskDelay(1);  /* Let this core's idle task run even in dense traffic. */
    }
}

#ifndef ADS_B_TDECK_PLUS
static const char *menu_message;
static bool save_pending, mounted, have_trace;
static unsigned test_count;
static int64_t cleared_through_ms=-1;

static void close_menu(void) {
    menu.open=false;
    save_pending=false;
    atomic_store(&watching,false);
}

static void finish_save(void) {
    if (!save_pending || !atomic_load(&ready) || menu.hz!=atomic_load(&applied_hz)) return;
    save_pending=false;
    if (save_dial(menu.hz)) close_menu();
    else menu_message="Save failed";
}

static void handle_key(Key key) {
    if (key==KEY_NONE) return;
    if (!menu.open) {
        if (key==KEY_TAP) history_older();
        if (key==KEY_HOLD) history_live();
        if (key==KEY_DOUBLE) {
            menu_open(&menu,atomic_load(&requested_hz));
            portENTER_CRITICAL(&scope_lock);
            memset(&scope,0,sizeof(scope));
            portEXIT_CRITICAL(&scope_lock);
            menu_message=NULL;
            atomic_store(&watching,true);
        }
        return;
    }
    if (key==KEY_REPEAT && menu.action!=TUNE_UP && menu.action!=TUNE_DOWN) return;
    menu_message=NULL;
    save_pending=false;
    if (key==KEY_DOUBLE) {
        save_pending=true;
        finish_save();
    } else if (key==KEY_TAP) menu_next(&menu);
    else if (key==KEY_HOLD || key==KEY_REPEAT) {
        if (menu.action==DISCARD) {
            atomic_store(&requested_hz,saved_hz);
            close_menu();
        } else if (menu.action==CLEAR_HISTORY) {
            int64_t cutoff=now_ms();
            EraseResult result=mounted ? storage_clear_packets() : ERASE_FAILED;
            if (result==ERASE_FAILED) { menu_message="Erase failed"; return; }
            history_clear();
            cleared_through_ms=cutoff;
            test_count=0;
            have_trace=false;
            menu_message=result==ERASE_DONE ? "History cleared" : "Erase incomplete";
        } else {
            menu_adjust(&menu);
            atomic_store(&failed_hz,0);
            atomic_store(&requested_hz,menu.hz);
        }
    }
}

static void button(void) {
    static Button b;
    handle_key(button_update(&b,!gpio_get_level(BOOT_BUTTON),now_ms()));
}

/* USB stays useful in the field without a Python REPL. Commands are lines.
 * An imported card retains its old boot and time, so it appears as saved. */
static void console(bool screen_ok) {
    static char line[1536];
    static size_t used;
    static bool overlong;
    char buffer[128];
    int n=read(STDIN_FILENO,buffer,sizeof(buffer));
    for (int i=0; i<n; i++) {
        char ch=buffer[i];
        if (ch!='\r' && ch!='\n') {
            if (used+1<sizeof(line)) line[used++]=ch;
            else overlong=true;
            continue;
        }
        line[used]=0;
        if (!overlong && used) {
            if (!strcmp(line,"dump") && mounted) storage_dump();
            else if (!strcmp(line,"screen")) display_dump();
            else if (!strncmp(line,"key ",4)) {
                const char *key=line+4;
                if (!strcmp(key,"tap")) handle_key(KEY_TAP);
                if (!strcmp(key,"hold")) handle_key(KEY_HOLD);
                if (!strcmp(key,"double")) handle_key(KEY_DOUBLE);
            }
            else if (!strcmp(line,"status"))
                printf("SXSTATUS %08"PRIx32" ready=%u bursts=%u polls=%u queue_peak=%u queue_full=%u "
                       "filtered=%u rearm_max_us=%u retries=%u deferred_drops=%u log=%u oled=%u dial=%u saved=%u menu=%u scope_cols=%u scope_samples=%u rssi_code=%d peak_code=%d scope_hits=%u wave_block=%u zoom=%u decoder_ram=%s\n",boot_id,atomic_load(&ready),
                       atomic_load(&bursts),atomic_load(&polls),atomic_load(&high_water),atomic_load(&full_queue),
                       atomic_load(&filtered),atomic_load(&max_rearm_us),atomic_load(&restarts),atomic_load(&deferred),mounted,screen_ok,atomic_load(&applied_hz),
                       (unsigned)saved_hz,menu.open,menu.open?view.columns:0,menu.open?view.samples:0,
                       menu.open?view.level:-1,menu.open?view.strongest:-1,menu.open?view.triggers:0,
                       menu.open?(unsigned)view.block:0,menu.open && view.zoom,
                       decoder_workspace_internal()?"internal":"psram");
            else if (!strncmp(line,"import ",7) && mounted) {
                bool ok=storage_import(line+7);
                puts(ok?"SXIMPORT OK":"SXIMPORT ERROR");
            }
        }
        used=0; overlong=false;
    }
}

static void field_task(void *unused) {
    fcntl(STDIN_FILENO,F_SETFL,O_NONBLOCK);
    mounted=storage_init()==ESP_OK;
    bool log_ok=mounted;
    bool oled=display_init()==ESP_OK, screen_ok=oled;
    history_set_boot(boot_id);
    if (mounted) storage_restore();
    gpio_config_t pin={.pin_bit_mask=1ULL<<BOOT_BUTTON,.mode=GPIO_MODE_INPUT,.pull_up_en=true};
    ESP_ERROR_CHECK(gpio_config(&pin));
    int64_t last_save=-30000, last_draw=-1000, last_clock=now_ms();
    uint8_t trace[SAMPLES];
    while (true) {
        Result r;
        bool redraw=false;
        /* A waveform captured before Clear can finish decoding afterwards. */
        if (xQueueReceive(results,&r,pdMS_TO_TICKS(10)) && r.capture.last_ms>cleared_through_ms) {
            int64_t now=now_ms();
            if (r.length && !r.evidence_index) {
                Card c={.boot=boot_id,.ms=r.capture.last_ms,.length=r.length};
                memcpy(c.word,r.word,r.length);
                if (r.word[0]>>3==2) c.test_count=++test_count;
                history_add(c);
                redraw=true;
            }
            char line[1280];
            observation(line,sizeof(line),&r,boot_id,now,test_count,atomic_load(&deferred));
            printf("%s %"PRId64" %s\n",r.evidence_index?"SXSUPPORT":"SXOBS",now,line);
            if (r.length==7 && !r.evidence_index) {
                printf("SXDF11 %08"PRIx32" %"PRId64" ",boot_id,r.capture.last_ms);
                for (int i=0; i<7; i++) printf("%02x",r.word[i]);
                printf(" %u\n",r.evidence_count);
            }
            if (r.length || now-last_save>=30000) {
                log_ok=mounted && storage_save(line,r.length!=0);
                memcpy(trace,r.capture.raw,SAMPLES);
                have_trace=true;
                last_save=now;
            }
        }
        button();
        console(screen_ok);
        finish_save();
        int64_t now=now_ms();
        if (oled && (redraw || now-last_draw>=100)) {
            Screen s;
            if (menu.open) {
                Scope snapshot;
                portENTER_CRITICAL(&scope_lock);
                memcpy(&snapshot,&scope,sizeof(snapshot));
                portEXIT_CRITICAL(&scope_lock);
                if (atomic_load(&failed_hz)==menu.hz) {
                    save_pending=false;
                    menu.hz=atomic_load(&requested_hz);
                    menu_message="PLL did not lock";
                }
                scope_view(&snapshot,menu.hz,now,&view);
                const char *message=menu_message;
                if (menu.hz!=atomic_load(&applied_hz)) message="Tuning...";
                menu_screen(&menu,&view,message,s);
                screen_ok=display_draw(s,NO_CARD,NULL,&view)==ESP_OK;
            } else {
                CardView card=history_screen(s,boot_id,now);
                char *notice=s[card==POSITION_CARD ? 7 : 6];
                if (!atomic_load(&ready)) strcpy(notice,atomic_load(&restarts)?"Radio retry":"Starting radio");
                if (!log_ok) strcpy(notice,"Log unavailable");
                screen_ok=display_draw(s,card,card==NO_CARD && have_trace && !s[6][0]?trace:NULL,NULL)==ESP_OK;
            }
            last_draw=now;
        }
        if (now-last_clock>=60000) {
            printf("SXCLOCK %08"PRIx32" %"PRId64" %u %u %u %u %u 0 %u\n",boot_id,now,
                   atomic_load(&bursts),atomic_load(&polls),atomic_load(&restarts),atomic_load(&high_water),
                   atomic_load(&full_queue),atomic_load(&deferred));
            last_clock=now;
        }
    }
}
#else
static TdeckNav navigation;
static TdeckLanguage ui_language;

static TdeckLanguage load_language(void) {
    uint8_t value;
    nvs_handle_t handle;
    if (nvs_flash_init()!=ESP_OK || nvs_open("tdeck_ui",NVS_READONLY,&handle)!=ESP_OK)
        return TDECK_ENGLISH;
    esp_err_t result=nvs_get_u8(handle,"language",&value);
    nvs_close(handle);
    return result==ESP_OK && value==TDECK_ITALIAN ? TDECK_ITALIAN : TDECK_ENGLISH;
}

static bool save_language(TdeckLanguage choice) {
    if (choice==ui_language) return true;
    nvs_handle_t handle;
    if (nvs_open("tdeck_ui",NVS_READWRITE,&handle)!=ESP_OK) return false;
    esp_err_t result=nvs_set_u8(handle,"language",choice);
    if (result==ESP_OK) result=nvs_commit(handle);
    nvs_close(handle);
    if (result!=ESP_OK) return false;
    ui_language=choice;
    return true;
}

static bool lcd_ok;
static int battery_mv;
static int battery_adc_mv;
static unsigned brightness=12;
static GpsNmea gps_nmea;
static TdeckClock local_clock;
static char clock_saved[TDECK_TIME_LENGTH];
static bool clock_setup;
static bool clock_restored, restart_pending, restart_error, restart_sd_warning;
static int64_t restart_deadline;
static unsigned keyboard_keys;
static uint8_t keyboard_last;
static TdeckReplayReply replay_files;
static uint32_t replay_ticket;
static TdeckReplayAircraft *sd_archive;
static unsigned sd_aircraft_count, sd_frames;
static char replay_name[TDECK_REPLAY_NAME];
static TdeckPage replay_origin;
static TdeckMapView active_map_view;
static unsigned visible_map_tiles;
static bool map_loading, map_preloaded;
static bool gps_uart_ok;
static unsigned gps_baud=38400;
static uint32_t gps_probe_ms;
static char gps_model[27];
static bool gps_version_pending;
static unsigned gps_version_baud;
static uint32_t gps_version_ms;
static UbxReceiver gps_ubx;
static GnssSatellites gps_constellations;
static bool gps_sat_fresh;
static uint32_t gps_sat_ms, gps_sat_query_ms;
static TdeckGpsPower gps_power;
static uint32_t gps_power_ms, gps_rx_ms;
enum { TDECK_MIN_DIAL=1089000000, TDECK_MAX_DIAL=1091000000, TDECK_DIAL_STEP=50000 };
enum { TDECK_BENCH_MIN_DIAL=867000000, TDECK_BENCH_MAX_DIAL=870000000 };
enum { TDECK_GPS_TX=43, TDECK_GPS_RX=44 };
static const uint8_t gps_wake_byte=0xff;

static esp_err_t gps_start(void) {
    const uart_config_t config={
        .baud_rate=gps_baud,.data_bits=UART_DATA_8_BITS,.parity=UART_PARITY_DISABLE,
        .stop_bits=UART_STOP_BITS_1,.flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT
    };
    esp_err_t err=uart_param_config(UART_NUM_1,&config);
    if (err==ESP_OK) err=uart_set_pin(UART_NUM_1,TDECK_GPS_TX,TDECK_GPS_RX,
                                    UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE);
    if (err==ESP_OK) err=uart_driver_install(UART_NUM_1,1024,0,0,NULL,0);
    /* A host restart should also resume a receiver left in UART standby. */
    if (err==ESP_OK && uart_write_bytes(UART_NUM_1,&gps_wake_byte,1)!=1) err=ESP_FAIL;
    return err;
}

static TdeckGpsState gps_state(uint32_t now) {
    if (!gps_uart_ok) return GPS_UART_ERROR;
    if (!gps_nmea_recent(gps_nmea.seen,gps_nmea.last_ms,now)) return GPS_NO_DATA;
    if (!gps_nmea_recent(gps_nmea.fix_known,gps_nmea.fix_ms,now)) return GPS_NMEA;
    return gps_nmea.fix ? GPS_FIX : GPS_NO_FIX;
}

static int gps_satellites(uint32_t now) {
    if (gps_power!=GPS_RUNNING) return -1;
    return gps_nmea_recent(gps_nmea.satellites_known,gps_nmea.satellites_ms,now) ?
           (int)gps_nmea.satellites : -1;
}

static bool gps_can_sleep(void) {
    return gps_uart_ok && !strncmp(gps_model,"MIA-M10Q",8) &&
           (gps_model[8]==0 || gps_model[8]=='-');
}

static void gps_power_request(bool enable, uint32_t now) {
    /* UBX-RXM-PMREQ: duration zero, backup + force, wake on UART RX. */
    static const uint8_t standby[]={
        0xb5,0x62,0x02,0x41,0x10,0, 0,0,0,0, 0,0,0,0,
        6,0,0,0, 8,0,0,0, 0x61,0x6b
    };
    if (!gps_can_sleep()) {
        puts("SXGPSPOWER unsupported");
        return;
    }
    if (enable) {
        if (gps_power!=GPS_STANDBY && gps_power!=GPS_WAKE_FAILED) return;
    } else if (gps_power!=GPS_RUNNING || !gps_nmea_recent(gps_nmea.seen,gps_nmea.last_ms,now)) {
        puts("SXGPSPOWER no recent NMEA");
        return;
    }
    const uint8_t *packet=enable ? &gps_wake_byte : standby;
    size_t bytes=enable ? 1 : sizeof(standby);
    if (uart_write_bytes(UART_NUM_1,packet,bytes)!=(int)bytes) {
        puts("SXGPSPOWER UART error");
        return;
    }
    gps_version_pending=false;
    gps_sat_fresh=false;
    gps_power=enable ? GPS_WAKING : GPS_SLEEPING;
    gps_power_ms=now;
    if (enable) {
        gps_nmea.seen=false;
        gps_nmea.used=0;
        gps_nmea.fix_known=false;
        gps_nmea.satellites_known=false;
    }
    puts(enable ? "SXGPSPOWER waking" : "SXGPSPOWER standby requested");
}

static void gps_ubx_message(uint32_t now) {
    if (gps_power!=GPS_RUNNING) return;
    if (tdeck_ubx_satellites(&gps_ubx,&gps_constellations)) {
        gps_sat_ms=now;
        gps_sat_fresh=true;
        return;
    }
    unsigned length=gps_ubx.length;
    if (!gps_version_pending || gps_ubx.data[0]!=UBX_MON || gps_ubx.data[1]!=UBX_MON_VER ||
        length<40 || (length-40)%30) return;
    gps_version_pending=false;
    const char *payload=(const char *)gps_ubx.data+4;
    printf("SXGPSVER hw=%.10s sw=%.30s\n",payload+30,payload);
    for (unsigned j=40; j<length; j+=30) {
        const char *entry=payload+j;
        printf("SXGPSVER %.30s\n",entry);
        if (!strncmp(entry,"MOD=",4)) snprintf(gps_model,sizeof(gps_model),"%.26s",entry+4);
    }
}

static void gps_query_version(uint32_t now) {
    static const uint8_t poll[]={0xb5,0x62,0x0a,0x04,0,0,0x0e,0x34};
    if (gps_power!=GPS_RUNNING || !gps_uart_ok ||
        !gps_nmea_recent(gps_nmea.seen,gps_nmea.last_ms,now)) {
        puts("SXGPSVER no recent NMEA");
        return;
    }
    if (uart_write_bytes(UART_NUM_1,poll,sizeof(poll))!=(int)sizeof(poll)) {
        puts("SXGPSVER UART error");
        return;
    }
    gps_version_ms=now;
    gps_version_pending=true;
    gps_version_baud=gps_baud;
}

static void gps_poll(uint32_t now) {
    if (!gps_uart_ok) return;
    uint8_t bytes[128];
    int count=uart_read_bytes(UART_NUM_1,bytes,sizeof(bytes),0);
    if (now-gps_rx_ms>250) gps_ubx.used=0;
    if (count>0) gps_rx_ms=now;
    for (int i=0; i<count; i++) {
        bool binary=gps_ubx.used || bytes[i]==0xb5;
        if (tdeck_ubx_feed(&gps_ubx,bytes[i])) gps_ubx_message(now);
        if (binary) {
            gps_nmea.bytes++;
            gps_nmea.used=0;
        } else gps_nmea_feed(&gps_nmea,bytes[i],now);
    }
    if (gps_version_pending && now-gps_version_ms>=2000) {
        gps_version_pending=false;
        puts("SXGPSVER timeout");
    }
    if (gps_power==GPS_SLEEPING) {
        /* Allow time for the final UART data before checking for silence. */
        if (now-gps_power_ms>=6000) {
            bool quiet=now-gps_rx_ms>=5000;
            gps_power=quiet ? GPS_STANDBY : GPS_RUNNING;
            puts(quiet ? "SXGPSPOWER standby (NMEA silent)" : "SXGPSPOWER standby not confirmed");
            if (quiet) {
                gps_ubx.used=0;
                gps_nmea.seen=false;
                gps_nmea.used=0;
                gps_nmea.fix_known=false;
                gps_nmea.satellites_known=false;
            }
        }
        return;
    }
    if (gps_power==GPS_STANDBY || gps_power==GPS_WAKING || gps_power==GPS_WAKE_FAILED) {
        if (gps_nmea_recent(gps_nmea.seen,gps_nmea.last_ms,now)) {
            gps_power=GPS_RUNNING;
            puts("SXGPSPOWER NMEA resumed");
        } else if (gps_power==GPS_WAKING && now-gps_power_ms>=8000) {
            gps_power=GPS_WAKE_FAILED;
            puts("SXGPSPOWER wake not confirmed");
        }
        if (gps_power!=GPS_RUNNING) return;
    }
    if (gps_nmea_recent(gps_nmea.seen,gps_nmea.last_ms,now)) {
        if (!gps_model[0] && gps_version_baud!=gps_baud) gps_query_version(now);
        if (navigation.page==TDECK_GPS && gps_model[0] && !gps_version_pending &&
            now-gps_sat_query_ms>=2000) {
            /* UBX-NAV-SAT poll, no receiver configuration changes. */
            static const uint8_t poll[]={0xb5,0x62,UBX_NAV,UBX_NAV_SAT,0,0,0x36,0xa3};
            uart_write_bytes(UART_NUM_1,poll,sizeof(poll));
            gps_sat_query_ms=now;
        }
        return;
    }
    if (now-gps_probe_ms<6000) return;
    unsigned next=gps_baud==38400 ? 9600 : 38400;
    gps_probe_ms=now;
    if (uart_set_baudrate(UART_NUM_1,next)==ESP_OK) {
        uart_flush_input(UART_NUM_1);
        gps_ubx.used=0;
        gps_sat_fresh=false;
        gps_nmea.used=0;
        gps_baud=next;
    }
}

#ifdef ADS_B_TDECK_REPLAY
enum { REPLAY_BOOT=1 };
#include "tdeck_replay_data.h"
static bool replay_on;
#endif
static uint32_t ui_boot(void) {
#ifdef ADS_B_TDECK_REPLAY
    if (replay_on) return REPLAY_BOOT;
#endif
    return boot_id;
}

static void sd_replay_close(void) {
    free(sd_archive);
    sd_archive=NULL;
    sd_aircraft_count=sd_frames=0;
}

static void sd_replay_leave(void) {
    tdeck_storage_replay_cancel();
    ++replay_ticket;
    sd_replay_close();
    replay_name[0]=0;
    replay_origin=TDECK_LIST;
    replay_files=(TdeckReplayReply){0};
    navigation=(TdeckNav){0};
}

static void sd_replay_request(const char *name) {
#ifdef ADS_B_TDECK_REPLAY
    if (replay_on) return;
#endif
    sd_replay_close();
    navigation.page=TDECK_REPLAY_FILES;
    if (!name) navigation.replay_index=0;
    TdeckReplayRequest request={.ticket=++replay_ticket};
    if (name && !tdeck_replay_name(name)) {
        replay_files=(TdeckReplayReply){.state=REPLAY_ERROR};
        return;
    }
    if (name) {
        snprintf(request.file,sizeof(request.file),"%s",name);
        strcpy(replay_name,request.file);
    }
    replay_files=(TdeckReplayReply){
        .state=tdeck_storage_replay_request(&request) ? REPLAY_LOADING : REPLAY_NO_SD
    };
}

static void sd_replay_tick(void) {
    TdeckReplayReply reply;
    if (!tdeck_storage_replay_reply(&reply)) return;
    if (reply.ticket!=replay_ticket || navigation.page!=TDECK_REPLAY_FILES) {
        free(reply.aircraft);
    } else if (reply.state==REPLAY_READY) {
        sd_archive=reply.aircraft;
        sd_aircraft_count=reply.count;
        sd_frames=reply.frames;
        navigation.page=TDECK_LIST;
        navigation.index=0;
    } else {
        replay_files=reply;
        navigation.replay_index=0;
        if (reply.state==REPLAY_FILES) {
            for (unsigned i=0;i<reply.count;i++) {
                if (strcasecmp(reply.names[i],replay_name)) continue;
                navigation.replay_index=i;
                break;
            }
        }
    }
}

static void clock_begin_edit(void) {
    char value[TDECK_TIME_LENGTH];
    bool running=tdeck_clock_format(&local_clock,now_ms(),value);
    tdeck_clock_edit_begin(&navigation,running ? value : clock_saved);
}

static void clock_close(void) {
    navigation.page=clock_setup ? TDECK_LIST : TDECK_SYSTEM;
    clock_setup=false;
    navigation.clock_storage_error=false;
}

static void clock_finish(int64_t now) {
    char value[TDECK_TIME_LENGTH];
    bool saved=tdeck_clock_format(&local_clock,now,value) &&
               (!strcmp(value,clock_saved) || tdeck_clock_persist(value));
    if (saved) strcpy(clock_saved,value);
    puts(saved ? "SXCLOCK saved" : "SXCLOCK storage_error");
    if (navigation.page!=TDECK_CLOCK && navigation.page!=TDECK_CLOCK_CONFIRM) {
        if (saved) clock_setup=false;
        return;
    }
    if (saved) clock_close();
    else {
        navigation.page=TDECK_CLOCK_CONFIRM;
        navigation.clock_storage_error=true;
        navigation.clock_modify=false;
    }
}

static void clock_save(const char *digits) {
    int64_t now=now_ms();
    if (!tdeck_clock_set(&local_clock,digits,now)) {
        navigation.clock_error=true;
        puts("SXCLOCK invalid");
        return;
    }
    clock_restored=false;
    clock_finish(now);
}

static void clock_select(void) {
    int64_t now=now_ms();
    /* Confirmation preserves a running RTC, including its seconds. On cold
     * boot a saved value is only a suggestion until the user accepts it. */
    if (navigation.clock_modify ||
        (!local_clock.valid && !tdeck_clock_set_iso(&local_clock,clock_saved,now))) {
        clock_begin_edit();
        return;
    }
    clock_finish(now);
}

static void restart_request(void) {
    if (restart_pending) return;
    sd_replay_leave();
    navigation.page=TDECK_RESTART;
    restart_error=false;
    restart_sd_warning=false;
    restart_pending=true;
    restart_deadline=now_ms()+10000;
    tdeck_storage_stop(uxQueueMessagesWaiting(results));
    puts("SXRESTART waiting for SD");
}

static void restart_poll(int64_t now) {
    if (!restart_pending) return;
    TdeckStorageStatus sd=tdeck_storage_status();
    if (!strcmp(sd.state,"stopped_error")) {
        if (!restart_sd_warning) {
            restart_sd_warning=true;
            restart_deadline=now+2000;
            puts("SXRESTART SD journal error, card unmounted");
        } else if (now>=restart_deadline) esp_restart();
        return;
    }
    if (!strcmp(sd.state,"stopped") || !strcmp(sd.state,"unavailable")) {
        puts("SXRESTART restarting");
        esp_restart();
    }
    if (!strcmp(sd.state,"error") || now>=restart_deadline) {
        restart_pending=false;
        restart_error=true;
        puts("SXRESTART SD stop failed");
    }
}

static const char *screen_status(void) {
    static const char *const names[]={
        [TDECK_DISPLAY_ON]="on",[TDECK_DISPLAY_OFF]="off",[TDECK_DISPLAY_WAKING]="waking"
    };
    return lcd_ok ? names[tdeck_display_power()] : "unavailable";
}

static void keyboard_action(uint8_t key, bool confirm_replay) {
    if (restart_pending) return;
    if (lcd_ok && (key=='l' || key=='L')) {
        tdeck_display_request(!tdeck_display_requested());
        return;
    }
    if (lcd_ok && (tdeck_display_power()!=TDECK_DISPLAY_ON || !tdeck_display_requested())) {
        tdeck_display_request(true);
        return;
    }
    if (navigation.page==TDECK_CLOCK_CONFIRM) {
        if (key>='0' && key<='9') clock_begin_edit();
        else {
            if (key=='\r' || key=='\n') clock_select();
            return;
        }
    }
    if (navigation.page==TDECK_CLOCK) {
        if (tdeck_clock_edit_key(&navigation,key)) clock_save(navigation.clock_entry);
        return;
    }
    if (key=='r' || key=='R') {
        if (sd_archive || navigation.page==TDECK_REPLAY_FILES) sd_replay_leave();
        else if (confirm_replay) tdeck_navigate(&navigation,TDECK_PICK_REPLAY,ui_boot());
        else sd_replay_request(NULL);
    }
}

static void request_dial(unsigned hz) {
    atomic_store(&failed_hz,0);
    atomic_store(&requested_hz,hz);
}

static void ui_action(TdeckAction action) {
    if (lcd_ok && (tdeck_display_power()!=TDECK_DISPLAY_ON || !tdeck_display_requested())) {
        if (!restart_pending && (action==TDECK_OPEN || action==TDECK_MENU || action==TDECK_OPEN_SYSTEM))
            tdeck_display_request(true);
        return;
    }
    if (navigation.page==TDECK_RESTART) {
        if (!restart_pending && (action==TDECK_BACK || action==TDECK_MENU))
            navigation.page=TDECK_SYSTEM;
        return;
    }
    if (sd_archive) {
        tdeck_navigate_replay(&navigation,action,sd_archive,sd_aircraft_count);
        if (navigation.page==TDECK_REPLAY_FILES) sd_replay_request(NULL);
        return;
    }
    if (navigation.page==TDECK_CLOCK_CONFIRM) {
        if (action==TDECK_OPEN) clock_select();
        else if (action==TDECK_BACK || action==TDECK_MENU) {
            if (navigation.clock_storage_error || !clock_setup) clock_close();
        } else if (action==TDECK_PREV || action==TDECK_NEXT || action==TDECK_UP ||
                   action==TDECK_DOWN || action==TDECK_LEFT || action==TDECK_RIGHT)
            navigation.clock_modify=!navigation.clock_modify;
        return;
    }
    if (navigation.page==TDECK_CLOCK) {
        if (action==TDECK_BACK || action==TDECK_MENU) {
            if (!clock_setup) clock_close();
            else if (*clock_saved || local_clock.valid) navigation.page=TDECK_CLOCK_CONFIRM;
        }
        else if (tdeck_clock_edit_navigate(&navigation,action)) clock_save(navigation.clock_entry);
        return;
    }
    if (navigation.page==TDECK_REPLAY_FILES) {
        if (action==TDECK_BACK || action==TDECK_MENU) {
            TdeckPage previous=replay_origin;
            unsigned menu_item=navigation.menu_item;
            sd_replay_leave();
            navigation.page=previous;
            navigation.menu_item=menu_item;
        }
        else if (action==TDECK_OPEN && replay_files.state!=REPLAY_LOADING) {
            if (replay_files.state==REPLAY_FILES) {
                /* The request copies the name before replacing the catalog. */
                sd_replay_request(replay_files.names[navigation.replay_index]);
            } else sd_replay_request(NULL);
        } else if (replay_files.state==REPLAY_FILES) {
            if (action==TDECK_LEFT || action==TDECK_UP || action==TDECK_PREV)
                navigation.replay_index=(navigation.replay_index+replay_files.count-1)%replay_files.count;
            if (action==TDECK_RIGHT || action==TDECK_DOWN || action==TDECK_NEXT)
                navigation.replay_index=(navigation.replay_index+1)%replay_files.count;
        }
        return;
    }
    if (navigation.page==TDECK_GPS && action==TDECK_OPEN) {
        gps_power_request(gps_power==GPS_STANDBY || gps_power==GPS_WAKE_FAILED,(uint32_t)now_ms());
        return;
    }
    bool previous=action==TDECK_PREV || action==TDECK_LEFT || action==TDECK_UP;
    bool next_item=action==TDECK_NEXT || action==TDECK_RIGHT || action==TDECK_DOWN;
    if (previous || next_item) {
        int delta=next_item ? 1 : -1;
        if (navigation.page==TDECK_BRIGHTNESS) {
            int next=(int)brightness+delta;
            if (next>=1 && next<=16) {
                brightness=(unsigned)next;
                tdeck_backlight(brightness);
            }
            return;
        }
        if (navigation.page==TDECK_TUNING) {
            int next=(int)atomic_load(&requested_hz)+delta*TDECK_DIAL_STEP;
            if (next>=TDECK_MIN_DIAL && next<=TDECK_MAX_DIAL) request_dial((unsigned)next);
            return;
        }
    }
    if (navigation.page==TDECK_LANGUAGE && action==TDECK_OPEN &&
        !save_language(navigation.language_choice)) {
        navigation.language_save_error=true;
        return;
    }
    TdeckPage origin=navigation.page;
    tdeck_navigate(&navigation,action,ui_boot());
    if (navigation.page==TDECK_REPLAY_FILES) {
        replay_origin=origin;
        sd_replay_request(NULL);
    }
    if (navigation.page==TDECK_LANGUAGE && origin!=TDECK_LANGUAGE) {
        navigation.language_choice=ui_language;
        navigation.language_save_error=false;
    }
    if (navigation.page==TDECK_RESTART) restart_request();
    if (navigation.page==TDECK_SCREEN_OFF) {
        navigation.page=TDECK_SYSTEM;
        if (lcd_ok) tdeck_display_request(false);
    }
    if (navigation.page==TDECK_CLOCK) clock_begin_edit();
}

static void replay_console_status(void) {
    TdeckReplayAircraft entry={0};
    bool visible=false;
    if (sd_archive) {
        entry=sd_archive[navigation.index];
        int x,y;
        visible=navigation.page==TDECK_MAP &&
            tdeck_map_point(active_map_view,(TdeckPosition){entry.info.lat,entry.info.lon,entry.info.fixed},&x,&y) &&
            x>=14 && x<=MAP_WIDTH-14 && y>=19 && y<=MAP_HEIGHT-16;
    }
    printf("SXREPLAY active=%u state=%u frames=%u shown=%u finished=%u files=%u "
           "icao=%06"PRIX32" fixed=%u visible=%u lat=%.5f lon=%.5f altitude=\"%s\" "
           "aircraft=%u index=%u file=%s received_at=%s position_at=%s speed=\"%s\"\n",
           sd_archive!=NULL,sd_archive ? REPLAY_READY : replay_files.state,sd_frames,sd_frames,
           sd_archive!=NULL,replay_files.state==REPLAY_FILES ? replay_files.count : 0,
           entry.info.icao,entry.info.fixed,visible,entry.info.lat,entry.info.lon,entry.info.altitude,
           sd_aircraft_count,navigation.index+1,*replay_name ? replay_name : "-",
           *entry.received_at ? entry.received_at : "-",*entry.position_at ? entry.position_at : "-",entry.info.speed);
    if (!sd_archive && replay_files.state==REPLAY_FILES)
        for (unsigned i=0; i<replay_files.count; i++) printf("SXREPLAYFILE %s\n",replay_files.names[i]);
}

static void sd_read_command(const char *args) {
    if (strspn(args,"0123456789abcdefABCDEF")!=8 || args[8]!=' ' ||
        !args[9] || args[9+strspn(args+9,"0123456789")]) {
        puts("SXSDREAD state=invalid");
        return;
    }
    unsigned long offset=strtoul(args+9,NULL,10);
    if (offset>INT32_MAX) {
        puts("SXSDREAD state=invalid");
        return;
    }
    uint32_t session=strtoul(args,NULL,16);
    if (!tdeck_storage_read(session,(uint32_t)offset))
        printf("SXSDREAD session=%08"PRIX32" offset=%lu state=unavailable data=-\n",session,offset);
}

static void console_command(const char *line) {
    if (!strcmp(line,"status")) {
        uint32_t now=(uint32_t)now_ms();
        TdeckStorageStatus sd=tdeck_storage_status();
        const char *const gps_status[]={"no_data","nmea","no_fix","fix","uart_error"};
        printf("SXSTATUS %08"PRIx32" board=tdeck-plus ready=%u dial=%u requested=%u failed=%u df17=%u "
               "bursts=%u polls=%u queue_peak=%u queue_full=%u filtered=%u rearm_max_us=%u "
               "retries=%u deferred_drops=%u console_drops=%u floor=%u trigger=%u lcd=%u page=%u list_focus=%u "
               "battery_mv=%d battery_sense_mv=%d light=%u gps=%s gps_sat=%d gps_bytes=%"PRIu32
               " gps_nmea=%"PRIu32" gps_baud=%u gps_power=%u ble=disabled"
               " uptime_ms=%"PRIu32" raw=%u raw_exported=%u raw_queue_drops=%u raw_usb_drops=%u"
               " sd=%s sd_saved=%u sd_queue_drops=%u screen=%s decoder_ram=%s language=%s\n",boot_id,
               atomic_load(&ready),atomic_load(&applied_hz),atomic_load(&requested_hz),
               atomic_load(&failed_hz),atomic_load(&df17_count),atomic_load(&bursts),atomic_load(&polls),
               atomic_load(&high_water),atomic_load(&full_queue),atomic_load(&filtered),atomic_load(&max_rearm_us),
               atomic_load(&restarts),atomic_load(&deferred),atomic_load(&console_drops),
               atomic_load(&noise_floor),atomic_load(&trigger_level),lcd_ok,navigation.page,navigation.list_focus,
               battery_mv,battery_adc_mv,brightness,gps_status[gps_state(now)],gps_satellites(now),
               gps_nmea.bytes,gps_nmea.valid,gps_baud,(unsigned)gps_power,
               now,(unsigned)atomic_load(&raw_enabled),raw_exported,
               atomic_load(&raw_queue_drops),raw_usb_drops,sd.state,sd.saved,sd.queue_drops,screen_status(),
               decoder_workspace_internal()?"internal":"psram",ui_language==TDECK_ITALIAN?"it":"en");
    } else if (!strcmp(line,"screen on") || !strcmp(line,"screen off") || !strcmp(line,"screen status")) {
        if (lcd_ok && !restart_pending && strcmp(line,"screen status"))
            tdeck_display_request(!strcmp(line,"screen on"));
        printf("SXSCREEN state=%s requested=%s\n",screen_status(),
               lcd_ok && tdeck_display_requested() ? "on" : "off");
    } else if (!strcmp(line,"sd status") || !strcmp(line,"sd stop")) {
        if (!strcmp(line,"sd stop")) tdeck_storage_stop(uxQueueMessagesWaiting(results));
        TdeckStorageStatus sd=tdeck_storage_status();
        printf("SXSD state=%s saved=%u queue_drops=%u\n",sd.state,sd.saved,sd.queue_drops);
    } else if (!strncmp(line,"sd read ",8)) {
        sd_read_command(line+8);
    } else if (!strcmp(line,"map status")) {
        printf("SXMAP active=%u mode=%s zoom=%u left=%d top=%d tiles=%u loading=%u preloaded=%u\n",
               navigation.page==TDECK_MAP,navigation.map.zooming?"zoom":"pan",active_map_view.zoom,
               active_map_view.left,active_map_view.top,visible_map_tiles,map_loading,map_preloaded);
    } else if (!strcmp(line,"replay status")) {
        replay_console_status();
    } else if (!strncmp(line,"replay play ",12)) {
        sd_replay_request(line+12);
    } else if (!strcmp(line,"replay sd")) {
        keyboard_action('r',false);
    } else if (!strcmp(line,"time status")) {
        char value[TDECK_TIME_LENGTH];
        bool valid=tdeck_clock_format(&local_clock,now_ms(),value);
        printf("SXCLOCK source=%s local=%s restored=%u reset=%u saved=%s setup=%u\n",
               valid?"manual":"unset",valid?value:"-",clock_restored,(unsigned)esp_reset_reason(),
               *clock_saved ? clock_saved : "-",clock_setup);
    } else if (!strncmp(line,"time set ",9)) {
        clock_save(line+9);
    } else if (!strcmp(line,"time edit")) {
        sd_replay_leave();
        clock_begin_edit();
    } else if (!strcmp(line,"kbd status")) {
        printf("SXKEY received=%u last=%u\n",keyboard_keys,keyboard_last);
    } else if (!strcmp(line,"restart")) {
        restart_request();
    } else if (!strcmp(line,"raw on") || !strcmp(line,"raw off")) {
        bool enabled=!strcmp(line,"raw on");
        atomic_store(&raw_enabled,enabled);
        printf("SXRAWSTATE %s\n",enabled?"on":"off");
    } else if (!strcmp(line,"gps satellites")) {
        uint32_t now=(uint32_t)now_ms();
        if (gps_power!=GPS_RUNNING || !gps_nmea_recent(gps_sat_fresh,gps_sat_ms,now)) {
            puts("SXGPSSAT unavailable");
        } else {
            printf("SXGPSSAT GPS=%u/%u Galileo=%u/%u BeiDou=%u/%u GLONASS=%u/%u QZSS=%u/%u SBAS=%u/%u\n",
                   gps_constellations.visible[GNSS_GPS],gps_constellations.used[GNSS_GPS],
                   gps_constellations.visible[GNSS_GALILEO],gps_constellations.used[GNSS_GALILEO],
                   gps_constellations.visible[GNSS_BEIDOU],gps_constellations.used[GNSS_BEIDOU],
                   gps_constellations.visible[GNSS_GLONASS],gps_constellations.used[GNSS_GLONASS],
                   gps_constellations.visible[GNSS_QZSS],gps_constellations.used[GNSS_QZSS],
                   gps_constellations.visible[GNSS_SBAS],gps_constellations.used[GNSS_SBAS]);
        }
    } else if (!strcmp(line,"gps version")) {
        gps_query_version((uint32_t)now_ms());
    } else if (!strcmp(line,"gps standby") || !strcmp(line,"gps wake")) {
        gps_power_request(!strcmp(line,"gps wake"),(uint32_t)now_ms());
    } else if (!strncmp(line,"tune ",5)) {
        const char *digits=line+5;
        size_t length=strlen(digits);
        unsigned long hz=digits[0]!='0' && length>=9 && length<=10 &&
                         strspn(digits,"0123456789")==length ? strtoul(digits,NULL,10) : 0;
        bool bench=hz>=TDECK_BENCH_MIN_DIAL && hz<=TDECK_BENCH_MAX_DIAL;
        bool adsb=hz>=TDECK_MIN_DIAL && hz<=TDECK_MAX_DIAL;
        if (!bench && !adsb) {
            puts("SXTUNE ERROR expected 867000000-870000000 or 1089000000-1091000000 Hz");
            return;
        }
        request_dial(hz);
        printf("SXTUNE requested=%lu\n",hz);
    } else if (!strncmp(line,"ui ",3)) {
        const char *key=line+3;
        if (!strcmp(key,"list")) ui_action(TDECK_BACK);
        else if (!strcmp(key,"rf")) ui_action(TDECK_RADIO);
        else if (!strcmp(key,"system"))
            ui_action(navigation.page==TDECK_LIST && !sd_archive ? TDECK_OPEN_SYSTEM : TDECK_MENU);
        else if (!strcmp(key,"hold")) ui_action(TDECK_MENU);
        else if (!strcmp(key,"open")) ui_action(TDECK_OPEN);
        else if (!strcmp(key,"next")) ui_action(TDECK_NEXT);
        else if (!strcmp(key,"prev")) ui_action(TDECK_PREV);
        else if (!strcmp(key,"map")) ui_action(TDECK_OPEN_MAP);
        else if (!strcmp(key,"left")) ui_action(TDECK_LEFT);
        else if (!strcmp(key,"right")) ui_action(TDECK_RIGHT);
        else if (!strcmp(key,"up")) ui_action(TDECK_UP);
        else if (!strcmp(key,"down")) ui_action(TDECK_DOWN);
#ifdef ADS_B_TDECK_REPLAY
    } else if (!strcmp(line,"replay on")) {
        sd_replay_leave();
        history_clear();
        history_set_boot(REPLAY_BOOT);
        for (unsigned i=0; i<sizeof(replay_cards)/sizeof(*replay_cards); i++) history_add(replay_cards[i]);
        replay_on=true;
        navigation=(TdeckNav){0};
        puts("SXREPLAY ON");
    } else if (!strcmp(line,"replay off")) {
        history_clear();
        history_set_boot(boot_id);
        replay_on=false;
        navigation=(TdeckNav){0};
        puts("SXREPLAY OFF");
#endif
    }
}

static void console(void) {
    static char line[32];
    static size_t used;
    static bool overlong;
    char buffer[32];
    int n=read(STDIN_FILENO,buffer,sizeof(buffer));
    for (int i=0; i<n; i++) {
        char ch=buffer[i];
        if (ch!='\r' && ch!='\n') {
            if (used+1<sizeof(line)) line[used++]=ch;
            else overlong=true;
            continue;
        }
        line[used]=0;
        if (!overlong && used) console_command(line);
        used=0;
        overlong=false;
    }
}

static void export_raw(void) {
    static const char hex[]="0123456789abcdef";
    RawCapture record;
    for (unsigned i=0; i<16 && xQueueReceive(raw_captures,&record,0)==pdTRUE; i++) {
        const Capture *c=&record.capture;
        char line[128+2*SAMPLES];
        int n=snprintf(line,128,"SXRAW %08"PRIx32" %"PRIu32" %"PRId64" %"PRId64" %"PRIu32" %u %u %c ",
                       boot_id,c->block,c->first_ms,c->last_ms,c->dial_hz,c->floor,c->threshold,record.admission);
        if (n<0 || n>=128) {
            raw_usb_drops++;
            continue;
        }
        for (unsigned j=0; j<SAMPLES; j++) {
            line[n++]=hex[c->raw[j]>>4];
            line[n++]=hex[c->raw[j]&15];
        }
        line[n++]='\n';
        if (usb_serial_jtag_write_bytes(line,n,0)==n) raw_exported++;
        else raw_usb_drops++;
    }
}

static void field_task(void *unused) {
    fcntl(STDIN_FILENO,F_SETFL,O_NONBLOCK);
    uint16_t *frame=heap_caps_malloc(TDECK_WIDTH*TDECK_HEIGHT*sizeof(*frame),MALLOC_CAP_SPIRAM);
    uint16_t *map_pixels=NULL;
    TdeckMap startup_map={0};
    esp_err_t err=frame ? tdeck_display_init() : ESP_ERR_NO_MEM;
    lcd_ok=err==ESP_OK;
    if (!lcd_ok) printf("SXERROR display %s\n",esp_err_to_name(err));
    err=tdeck_controls_init();
    bool controls_ok=err==ESP_OK;
    if (!controls_ok) printf("SXERROR trackball %s\n",esp_err_to_name(err));
    err=tdeck_keyboard_init();
    if (err!=ESP_OK) printf("SXERROR keyboard %s\n",esp_err_to_name(err));
    err=tdeck_battery_init();
    bool battery_ok=err==ESP_OK;
    if (!battery_ok) printf("SXERROR battery %s\n",esp_err_to_name(err));
    err=gps_start();
    gps_uart_ok=err==ESP_OK;
    if (!gps_uart_ok) printf("SXERROR gps %s\n",esp_err_to_name(err));
    history_set_boot(boot_id);
    int64_t last_draw=-1000, last_battery=-5000, map_moved=0;
    unsigned previous_polls=atomic_load(&polls);
    Capture trace;
    bool have_trace=false;
    while (true) {
        Result r;
        TickType_t wait=atomic_load(&raw_enabled)?1:pdMS_TO_TICKS(10);
        if (tdeck_storage_backpressured()) vTaskDelay(1);
        else if (xQueueReceive(results,&r,wait)) {
            char local_time[TDECK_TIME_LENGTH];
            tdeck_clock_format(&local_clock,r.capture.last_ms,local_time);
            tdeck_storage_record(&r,atomic_load(&deferred),local_time);
            Card card={.boot=boot_id,.ms=r.capture.last_ms,.length=r.length};
            memcpy(card.word,r.word,r.length);
#ifdef ADS_B_TDECK_REPLAY
            if (!replay_on)
#endif
                history_add(card);
            char word[29];
            for (unsigned i=0; i<14; i++) snprintf(word+2*i,3,"%02x",r.word[i]);
            printf("SXADSB %08"PRIx32" ms=%"PRId64" dial=%"PRIu32" df=17 crc=ok word=%s\n",
                   boot_id,r.capture.last_ms,r.capture.dial_hz,word);
        }
        int64_t now=now_ms();
        gps_poll((uint32_t)now);
        console();
        restart_poll(now_ms());
        export_raw();
        now=now_ms();
        uint8_t key=tdeck_keyboard_read(now);
        if (key) {
            keyboard_last=key;
            keyboard_keys++;
            keyboard_action(key,true);
        }
        sd_replay_tick();
        TdeckAction action;
        if (controls_ok && tdeck_controls(&action,now,navigation.page==TDECK_MAP,navigation.map.zooming))
            ui_action(action);
        if (lcd_ok) {
            err=tdeck_display_poll(now_ms());
            if (err!=ESP_OK) printf("SXERROR screen power %s\n",esp_err_to_name(err));
        }
        if (battery_ok && now-last_battery>=5000) {
            battery_adc_mv=tdeck_battery_mv();
            battery_mv=battery_adc_mv>=2500 && battery_adc_mv<=4600 ? battery_adc_mv : 0;
            last_battery=now;
        }
        if (lcd_ok && tdeck_display_power()==TDECK_DISPLAY_ON &&
            now-last_draw>=(navigation.page==TDECK_MAP ? 100 : 200)) {
            char local_time[TDECK_TIME_LENGTH];
            tdeck_clock_format(&local_clock,now,local_time);
            unsigned current_polls=atomic_load(&polls);
            TdeckStatus status={
                .language=ui_language,.boot=ui_boot(),.dial=atomic_load(&applied_hz),.ready=atomic_load(&ready),
                .requested=atomic_load(&requested_hz),.failed=atomic_load(&failed_hz),
                .battery_mv=battery_mv,.brightness=brightness,
                .gps=gps_state((uint32_t)now),.satellites=gps_satellites((uint32_t)now),
                .gps_baud=gps_uart_ok ? gps_baud : 0,.gps_sentences=gps_nmea.valid,.gps_model=gps_model,
                .gps_power=gps_power,.gps_can_sleep=gps_can_sleep(),
                .gps_constellations=gps_power==GPS_RUNNING &&
                    gps_nmea_recent(gps_sat_fresh,gps_sat_ms,(uint32_t)now) ? &gps_constellations : NULL,
                .usb_power=battery_adc_mv>4600 && battery_adc_mv<=5500,
                .retry=atomic_load(&restarts)!=0,.df17=atomic_load(&df17_count),
                .bursts=atomic_load(&bursts),.queue_full=atomic_load(&full_queue),
                .radio_polled=current_polls!=previous_polls,
                .local_time=local_time,.saved_time=clock_saved,.clock_setup=clock_setup,.replay_files=&replay_files,
                .replay=sd_archive!=NULL || navigation.page==TDECK_REPLAY_FILES,
                .archive=sd_archive,.archive_count=sd_aircraft_count,.archive_name=replay_name,
                .restart_error=restart_error,.restart_sd_warning=restart_sd_warning
            };
            previous_polls=current_polls;
            if (!status.dial) status.dial=DEFAULT_HZ;
            if (navigation.page==TDECK_RF) {
                if (xQueueReceive(latest_capture,&trace,0)==pdTRUE) have_trace=true;
                if (have_trace && trace.dial_hz==status.dial) status.trace=&trace;
            }
            status.gps_position=(TdeckPosition){
                gps_nmea.lat,gps_nmea.lon,gps_power==GPS_RUNNING && status.gps==GPS_FIX &&
                gps_nmea_recent(gps_nmea.position_known,gps_nmea.position_ms,(uint32_t)now)
            };
            if (navigation.page==TDECK_LIST && !sd_archive && !active_map_view.zoom && !map_preloaded &&
                tdeck_storage_map_available()) {
                unsigned tiles;
                TdeckMapView initial=tdeck_map_view(&startup_map,status.gps_position);
                map_preloaded=tdeck_storage_map_view(initial,true,NULL,&tiles);
            }
            if (navigation.page==TDECK_MAP) {
                status.map_view=tdeck_map_view(&navigation.map,sd_archive ? (TdeckPosition){0} : status.gps_position);
                if (!tdeck_map_same(active_map_view,status.map_view))
                    map_moved=active_map_view.zoom ? now : now-150;
                active_map_view=status.map_view;
                status.map_available=tdeck_storage_map_available();
                if (status.map_available && !map_pixels)
                    map_pixels=heap_caps_malloc(MAP_WIDTH*MAP_HEIGHT*sizeof(*map_pixels),MALLOC_CAP_SPIRAM);
                status.map_pixels=map_pixels;
                bool loaded=map_pixels &&
                    tdeck_storage_map_view(status.map_view,now-map_moved>=150,map_pixels,&status.map_tiles);
                visible_map_tiles=status.map_tiles;
                status.map_loading=map_loading=status.map_available && map_pixels && !loaded;
            }
#ifdef ADS_B_TDECK_REPLAY
            if (replay_on) {
                status.replay=true;
                status.df17=sizeof(replay_cards)/sizeof(*replay_cards);
                status.bursts=status.df17;
                status.queue_full=0;
                status.dial=replay_trace.dial_hz;
                status.trace=&replay_trace;
                now=replay_cards[sizeof(replay_cards)/sizeof(*replay_cards)-1].ms+2000;
            }
#endif
            tdeck_render(frame,&navigation,&status,now);
            err=tdeck_display_draw(frame);
            if (err!=ESP_OK) {
                lcd_ok=false;
                printf("SXERROR display %s\n",esp_err_to_name(err));
            }
            last_draw=now_ms();
        }
    }
}
#endif

void app_main(void) {
    boot_id=esp_random();
#ifdef ADS_B_TDECK_PLUS
    ui_language=load_language();
    clock_restored=tdeck_clock_restore(&local_clock,now_ms());
    tdeck_clock_load_saved(clock_saved);
    clock_setup=true;
    if (*clock_saved || clock_restored) {
        navigation.page=TDECK_CLOCK_CONFIRM;
        navigation.clock_modify=!clock_restored;
    } else clock_begin_edit();
    usb_serial_jtag_driver_config_t usb={.rx_buffer_size=256,.tx_buffer_size=8192};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
#if __has_include("driver/usb_serial_jtag_vfs.h")
    usb_serial_jtag_vfs_use_driver();
#else
    esp_vfs_usb_serial_jtag_use_driver();
#endif
#else
    load_dial();
#endif
    /* Reserve the decoder's contiguous internal workspace before the queues split the heap. */
    ESP_ERROR_CHECK(decoder_init(report) ? ESP_OK : ESP_ERR_NO_MEM);
    captures=xQueueCreate(QUEUE_DEPTH,sizeof(Capture));
    results=xQueueCreate(16,sizeof(Result));
    ESP_ERROR_CHECK(captures && results ? ESP_OK : ESP_ERR_NO_MEM);
#ifdef ADS_B_TDECK_PLUS
    latest_capture=xQueueCreate(1,sizeof(Capture));
    raw_captures=xQueueCreate(64,sizeof(RawCapture));
    ESP_ERROR_CHECK(latest_capture && raw_captures ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(radio_bus_init());
    tdeck_storage_start(boot_id);
#endif
    ESP_ERROR_CHECK(decoder_self_test() ? ESP_OK : ESP_ERR_INVALID_RESPONSE);
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(field_task,"field",12288,NULL,3,NULL,0)==pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(decode_task,"decoder",24576,NULL,5,NULL,1)==pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(radio_task,"radio",8192,NULL,8,NULL,0)==pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
}
