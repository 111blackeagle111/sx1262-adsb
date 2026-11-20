#include "gps_nmea.h"
#include <string.h>

static int hex(char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='A' && c<='F') return c-'A'+10;
    if (c>='a' && c<='f') return c-'a'+10;
    return -1;
}

static bool checksum(const char *s) {
    const char *end=strchr(s,'*');
    if (!end || end-s<7 || strlen(end)!=3 || s[0]!='$' || s[6]!=',') return false;
    if (strncmp(s+1,"GP",2) && strncmp(s+1,"GN",2) && strncmp(s+1,"GL",2) &&
        strncmp(s+1,"GA",2) && strncmp(s+1,"GB",2) && strncmp(s+1,"BD",2)) return false;
    for (unsigned i=3;i<6;i++) if (s[i]<'A' || s[i]>'Z') return false;
    int hi=hex(end[1]), lo=hex(end[2]);
    if (hi<0 || lo<0) return false;
    unsigned sum=0;
    for (const char *p=s+1;p<end;p++) sum^=(unsigned char)*p;
    return sum==(unsigned)(hi*16+lo);
}

/* Field numbers include the sentence name as field zero. */
static const char *field(const char *s, unsigned index) {
    while (index--) {
        s=strchr(s,',');
        if (!s) return NULL;
        s++;
    }
    return s;
}

static int number(const char *s) {
    if (!s || *s<'0' || *s>'9') return -1;
    unsigned n=0;
    while (*s>='0' && *s<='9') {
        n=n*10+(unsigned)(*s++-'0');
        if (n>99) return -1;
    }
    return (*s==',' || *s=='*') ? (int)n : -1;
}

static bool coordinate(const char *s, const char *hemisphere, unsigned degree_digits, double *result) {
    if (!s || !hemisphere) return false;
    unsigned degrees=0, minutes=0;
    for (unsigned i=0; i<degree_digits+2; i++) {
        if (*s<'0' || *s>'9') return false;
        if (i<degree_digits) degrees=10*degrees+(*s-'0');
        else minutes=10*minutes+(*s-'0');
        s++;
    }
    double fraction=0, place=0.1;
    if (*s=='.') {
        s++;
        if (*s<'0' || *s>'9') return false;
        while (*s>='0' && *s<='9') {
            fraction+=(*s++-'0')*place;
            place*=0.1;
        }
    }
    unsigned max=degree_digits==2 ? 90 : 180;
    char positive=degree_digits==2 ? 'N' : 'E', negative=degree_digits==2 ? 'S' : 'W';
    if (*s!=',' || (hemisphere[0]!=positive && hemisphere[0]!=negative) ||
        hemisphere[1]!=',' || minutes>=60 || degrees>max ||
        (degrees==max && (minutes || fraction))) return false;
    *result=(hemisphere[0]==positive ? 1 : -1)*(degrees+(minutes+fraction)/60);
    return true;
}

static void sentence(GpsNmea *gps, uint32_t now) {
    gps->line[gps->used]=0;
    if (!checksum(gps->line)) { gps->invalid++; return; }
    gps->valid++; gps->seen=true; gps->last_ms=now;
    unsigned lat_field;
    if (!strncmp(gps->line+3,"GGA",3)) {
        int quality=number(field(gps->line,6));
        int satellites=number(field(gps->line,7));
        gps->fix_known=quality>=0 && quality<=8;
        gps->fix=quality>0; gps->fix_ms=now;
        gps->satellites_known=satellites>=0;
        gps->satellites=satellites>=0 ? (unsigned)satellites : 0;
        gps->satellites_ms=now;
        lat_field=2;
    } else if (!strncmp(gps->line+3,"RMC",3)) {
        const char *status=field(gps->line,2);
        gps->fix_known=status && (status[0]=='A' || status[0]=='V') && status[1]==',';
        gps->fix=gps->fix_known && status[0]=='A'; gps->fix_ms=now;
        lat_field=3;
    } else return;
    double lat,lon;
    gps->position_known=gps->fix_known && gps->fix &&
        coordinate(field(gps->line,lat_field),field(gps->line,lat_field+1),2,&lat) &&
        coordinate(field(gps->line,lat_field+2),field(gps->line,lat_field+3),3,&lon);
    if (gps->position_known) {
        gps->lat=lat; gps->lon=lon; gps->position_ms=now;
    }
}

void gps_nmea_feed(GpsNmea *gps, unsigned char ch, uint32_t now) {
    gps->bytes++;
    if (ch=='$') {
        if (gps->used) gps->invalid++;
        gps->used=0; gps->line[gps->used++]='$';
    } else if (ch=='\r') {
        /* Accept CRLF as well as LF. */
    } else if (ch=='\n' && gps->used) {
        sentence(gps,now); gps->used=0;
    } else if (gps->used) {
        if (ch>=32 && ch<=126 && gps->used<sizeof(gps->line)-1) gps->line[gps->used++]=(char)ch;
        else { gps->overflow++; gps->used=0; }
    }
}

bool gps_nmea_recent(bool known, uint32_t last, uint32_t now) {
    return known && now-last<=5000;
}
