#include "gps_nmea.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)

static void feed(GpsNmea *gps, const char *s, uint32_t now) {
    for (;*s;s++) gps_nmea_feed(gps,(unsigned char)*s,now);
}

static void message(GpsNmea *gps, const char *body, uint32_t now) {
    unsigned sum=0;
    for (const char *s=body;*s;s++) sum^=(unsigned char)*s;
    char line[200];
    snprintf(line,sizeof(line),"$%s*%02X\r\n",body,sum);
    feed(gps,line,now);
}

int main(void) {
    GpsNmea gps={0};
    CHECK(!gps_nmea_recent(gps.seen,gps.last_ms,0));
    feed(&gps,"noise\r\n",0);
    CHECK(gps.bytes==7 && !gps.seen && !gps.valid);
    feed(&gps,"$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n",100);
    CHECK(gps.valid==1 && gps.seen && gps.fix_known && gps.fix);
    CHECK(gps.last_ms==100 && gps.fix_ms==100);
    CHECK(gps.position_known && gps.position_ms==100 &&
          gps.lat>48.1172 && gps.lat<48.1174 && gps.lon>11.5166 && gps.lon<11.5168);
    feed(&gps,"$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n",200);
    CHECK(gps.valid==2 && gps.satellites_known && gps.satellites==8 && gps.fix);
    CHECK(gps_nmea_recent(gps.seen,gps.last_ms,5200));
    CHECK(!gps_nmea_recent(gps.seen,gps.last_ms,5201));
    feed(&gps,"$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*00\n",300);
    CHECK(gps.invalid==1 && gps.valid==2 && gps.last_ms==200);
    message(&gps,"GNRMC,,V,,,,,,,,,",400);
    CHECK(gps.valid==3 && gps.fix_known && !gps.fix && !gps.position_known);
    message(&gps,"GNGGA,,,,,,0,00,,,,,,,",500);
    CHECK(gps.fix_known && !gps.fix && gps.satellites_known && gps.satellites==0);
    message(&gps,"GNGGA,,,,,,,,,,,,,",600);
    CHECK(!gps.fix_known && !gps.satellites_known);
    message(&gps,"GNGGA,,,,,,999999999999999999,99999999999,,,,,,,",700);
    CHECK(!gps.fix_known && !gps.satellites_known);
    message(&gps,"GNRMC,,Agarbage,,,,,,,,,",800);
    CHECK(!gps.fix_known);
    message(&gps,"GNRMC",900);
    CHECK(!gps.fix_known);
    message(&gps,"XXRMC,,A,,,,,,,,,",1000);
    CHECK(gps.last_ms==800);
    feed(&gps,"$GNRMC,,V*ZZ\n$GNRMC,,V\n",1100);
    CHECK(gps.last_ms==800);
    feed(&gps,"$",1200);
    for (unsigned i=0;i<150;i++) feed(&gps,"a",1200);
    CHECK(gps.overflow==1 && gps.used==0);
    message(&gps,"GNRMC,,A,,,,,,,,,",1300);
    CHECK(gps.fix_known && gps.fix && gps.last_ms==1300);
    feed(&gps,"$partial$GNRMC,,V*00\n",1400);
    CHECK(gps.last_ms==1300);
    message(&gps,"GNRMC,,V,,,,,,,,,",UINT32_MAX-100);
    CHECK(gps_nmea_recent(gps.seen,gps.last_ms,100));
    CHECK(!gps_nmea_recent(gps.seen,gps.last_ms,5000));
    /* Other valid navigation sentences prove activity, but cannot refresh a fix. */
    message(&gps,"GNGSA,A,1,,,,,,,,,,,,,,,,",6000);
    CHECK(gps_nmea_recent(gps.seen,gps.last_ms,6000));
    CHECK(!gps_nmea_recent(gps.fix_known,gps.fix_ms,6000));
    message(&gps,"GNRMC,,A,3359.500,S,07030.000,W,,,,,,",6100);
    CHECK(gps.position_known && gps.lat<-33.99 && gps.lon==-70.5);
    message(&gps,"GNRMC,,A,3360.000,S,07030.000,W,,,,,,",6200);
    CHECK(gps.fix && !gps.position_known);
    return 0;
}
