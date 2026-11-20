/* L76K standby control and optional receive-only console observer. */
#include <Arduino.h>
#include <stdio.h>
#include "board.h"
#include "gps_nmea.h"

static GpsNmea nmea;
static TaskHandle_t listener;
static bool standby_requested;
enum { GPS_WAKE=34 }; /* P1.02, active low; separate from shared board power. */

void board_gps_standby(bool standby) {
    if (standby) {
        digitalWrite(GPS_WAKE,LOW);
        pinMode(GPS_WAKE,OUTPUT);
        /* Enable input readback while holding low; never drive this pin high. */
        NRF_P1->PIN_CNF[2]&=~GPIO_PIN_CNF_INPUT_Msk;
    } else {
        /* The module's internal pull-up returns it to continuous mode. */
        pinMode(GPS_WAKE,INPUT);
    }
    standby_requested=standby;
}

static void listen_task(void *) {
    for (;;) {
        while (Serial2.available()) {
            int ch=Serial2.read();
            uint32_t now=(uint32_t)millis();
            taskENTER_CRITICAL();
            gps_nmea_feed(&nmea,(unsigned char)ch,now);
            taskEXIT_CRITICAL();
        }
        vTaskDelay(1);
    }
}

void board_gps_status(bool stop) {
    if (stop) {
        if (listener) {
            vTaskSuspend(listener);
            Serial2.end();
            vTaskDelete(listener); listener=NULL;
            pinMode(41,INPUT);
        }
        char msg[160];
        snprintf(msg,sizeof(msg),"SXGPS monitor=0 requested=%s wake_level=%d activity=unknown fix=unknown satellites=-1\n",
            standby_requested?"standby":"continuous",digitalRead(GPS_WAKE));
        board_print(msg);
        return;
    }
    if (!listener) {
        nmea={};
        pinMode(41,INPUT);
        /* Serial2 defaults to RX8/TX6 with our PCA10056 core. Use GPS RX41,
         * and leave its command pin40 and reset pin37 untouched. Starting
         * observation does not change the separately controlled wake pin34.
         * The temporary TX route is an unused MCU pin, immediately detached. */
        Serial2.setPins(41,6);
        Serial2.begin(9600);
        NRF_UARTE1->PSEL.TXD=0xffffffffu;
        NRF_UARTE1->PSEL.RTS=0xffffffffu;
        NRF_UARTE1->PSEL.CTS=0xffffffffu;
        if (xTaskCreate(listen_task,"gps-listen",384,NULL,2,&listener)!=pdPASS) {
            listener=NULL; Serial2.end();
            board_print("SXGPS monitor=0 error=task_memory\n");
            return;
        }
    }
    GpsNmea snapshot;
    taskENTER_CRITICAL(); snapshot=nmea; taskEXIT_CRITICAL();
    uint32_t now=(uint32_t)millis();
    bool recent=gps_nmea_recent(snapshot.seen,snapshot.last_ms,now);
    char age[16];
    if (snapshot.seen) snprintf(age,sizeof(age),"%lu",(unsigned long)(now-snapshot.last_ms));
    else snprintf(age,sizeof(age),"-1");
    char msg[256];
    snprintf(msg,sizeof(msg),"SXGPS monitor=1 requested=%s wake_level=%d activity=%s baud=9600 bytes=%lu nmea=%lu invalid=%lu overflow=%lu last_ms=%s fix=%s satellites=%d rx_only=1\n",
        standby_requested?"standby":"continuous",digitalRead(GPS_WAKE),recent?"active":snapshot.seen?"no_recent_data":"no_data",
        (unsigned long)snapshot.bytes,(unsigned long)snapshot.valid,
        (unsigned long)snapshot.invalid,(unsigned long)snapshot.overflow,age,
        gps_nmea_recent(snapshot.fix_known,snapshot.fix_ms,now)?(snapshot.fix?"yes":"no"):"unknown",
        gps_nmea_recent(snapshot.satellites_known,snapshot.satellites_ms,now)?(int)snapshot.satellites:-1);
    board_print(msg);
}
