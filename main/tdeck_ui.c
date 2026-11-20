#include "tdeck_ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TR(language,en,it) ((language)==TDECK_ITALIAN ? (it) : (en))
#define RGB(r,g,b) (((r)>>3)<<11 | ((g)>>2)<<5 | (b)>>3)
enum {
    BG=RGB(34,37,44), TEXT=RGB(255,244,221), DIM=RGB(203,194,175),
    AMBER=RGB(255,197,107), RULE=RGB(77,76,74), STRIP=RGB(45,48,56)
};

typedef struct { uint16_t offset; int8_t left, top; uint8_t width, height, advance; } Glyph;
typedef struct { const uint8_t *pixels; const Glyph *glyphs; bool ascii; } Font;
#include "tdeck_font.inc"

static const Glyph *glyph(const Font *font, unsigned char ch) {
    if (font->ascii) {
        unsigned index=ch==0xb0 ? 95 : ch>=32 && ch<=126 ? ch-32 : '?' - 32;
        return &font->glyphs[index];
    }
    const char *characters=" 0123456789,.-";
    const char *p=ch ? strchr(characters,ch) : NULL;
    return &font->glyphs[p ? p-characters : 0];
}

static int width(const Font *font, const char *s) {
    int pixels=0;
    for (; *s; s++) pixels+=glyph(font,*s)->advance;
    return pixels;
}

static uint16_t blend(uint16_t bg, uint16_t fg, unsigned coverage) {
    unsigned red=((bg>>11)*(3-coverage)+(fg>>11)*coverage+1)/3;
    unsigned green=(((bg>>5)&63)*(3-coverage)+((fg>>5)&63)*coverage+1)/3;
    unsigned blue=((bg&31)*(3-coverage)+(fg&31)*coverage+1)/3;
    return red<<11 | green<<5 | blue;
}

static void pixel(uint16_t *p, int x, int y, uint16_t color) {
    if (x>=0 && x<TDECK_WIDTH && y>=0 && y<TDECK_HEIGHT) p[y*TDECK_WIDTH+x]=color;
}

static void rect(uint16_t *p, int x, int y, int w, int h, uint16_t color) {
    for (int row=y; row<y+h; row++)
        for (int col=x; col<x+w; col++) p[row*TDECK_WIDTH+col]=color;
}

static void line(uint16_t *p, int x, int y, int x2, int y2, uint16_t color) {
    int dx=abs(x2-x), dy=-abs(y2-y), error=dx+dy;
    int sx=x<x2 ? 1 : -1, sy=y<y2 ? 1 : -1;
    while (true) {
        pixel(p,x,y,color);
        if (x==x2 && y==y2) return;
        int step=2*error;
        if (step>=dy) { error+=dy; x+=sx; }
        if (step<=dx) { error+=dx; y+=sy; }
    }
}

static void text(uint16_t *p, int x, int baseline, const Font *font, uint16_t color, const char *s) {
    for (; *s; s++) {
        const Glyph *g=glyph(font,*s);
        for (int row=0; row<g->height; row++) {
            int y=baseline+g->top+row;
            if (y<0 || y>=TDECK_HEIGHT) continue;
            for (int col=0; col<g->width; col++) {
                int px=x+g->left+col, i=row*g->width+col;
                unsigned coverage=(font->pixels[g->offset+i/4]>>(2*(i%4)))&3;
                if (coverage && px>=0 && px<TDECK_WIDTH) {
                    uint16_t *dst=&p[y*TDECK_WIDTH+px];
                    *dst=blend(*dst,color,coverage);
                }
            }
        }
        x+=g->advance;
    }
}

static void center(uint16_t *p, int x, int y, const Font *font, uint16_t color, const char *s) {
    text(p,x-width(font,s)/2,y,font,color,s);
}

static void right(uint16_t *p, int x, int y, const Font *font, uint16_t color, const char *s) {
    text(p,x-width(font,s),y,font,color,s);
}

static void sync_selection(TdeckNav *nav, const uint32_t *addresses, unsigned count) {
    for (unsigned i=0; i<count; i++) {
        if (addresses[i]==nav->selected) {
            nav->index=i;
            return;
        }
    }
    nav->index=count && nav->index>=count ? count-1 : nav->index;
    if (!count) nav->index=0;
    nav->selected=count ? addresses[nav->index] : 0;
}

static const TdeckPage system_destinations[]={
    TDECK_RF,TDECK_BRIGHTNESS,TDECK_TUNING,TDECK_GPS,TDECK_REPLAY_FILES,TDECK_CLOCK,
    TDECK_LANGUAGE,TDECK_SCREEN_OFF,TDECK_RESTART
};
enum { SYSTEM_CHOICES=sizeof(system_destinations)/sizeof(*system_destinations) };

static void map_controls(TdeckMap *map, TdeckAction action) {
    if (action==TDECK_OPEN) map->zooming=!map->zooming;
    else if (action==TDECK_LEFT) tdeck_map_move(map,-1,0);
    else if (action==TDECK_RIGHT) tdeck_map_move(map,1,0);
    else if (action==TDECK_UP) tdeck_map_move(map,0,-1);
    else if (action==TDECK_DOWN) tdeck_map_move(map,0,1);
}

void tdeck_navigate(TdeckNav *nav, TdeckAction action, uint32_t boot) {
    if (action==TDECK_PICK_REPLAY) {
        nav->page=TDECK_LIST;
        nav->list_focus=TDECK_FOCUS_REPLAY;
        return;
    }
    if (nav->page==TDECK_LIST) {
        TdeckListFocus focus=nav->list_focus;
        if (action==TDECK_LEFT || action==TDECK_RIGHT) {
            if (focus==TDECK_FOCUS_AIRCRAFT) nav->list_focus=TDECK_FOCUS_MAP;
            else {
                enum { BUTTONS=TDECK_FOCUS_SYSTEM-TDECK_FOCUS_MAP+1 };
                int step=action==TDECK_RIGHT ? 1 : BUTTONS-1;
                nav->list_focus=TDECK_FOCUS_MAP+(focus-TDECK_FOCUS_MAP+step)%BUTTONS;
            }
            return;
        }
        if (action==TDECK_MENU) {
            nav->list_focus=TDECK_FOCUS_SYSTEM;
            return;
        }
        nav->list_focus=TDECK_FOCUS_AIRCRAFT;
        if (focus!=TDECK_FOCUS_AIRCRAFT) {
            if (action==TDECK_UP || action==TDECK_DOWN || action==TDECK_PREV || action==TDECK_NEXT)
                return; /* First vertical movement restores focus to the current aircraft. */
            if (action==TDECK_OPEN) {
                if (focus==TDECK_FOCUS_MAP) action=TDECK_OPEN_MAP;
                else {
                    nav->page=focus==TDECK_FOCUS_REPLAY ? TDECK_REPLAY_FILES : TDECK_SYSTEM;
                    return;
                }
            }
        }
    }
    if (action==TDECK_OPEN_SYSTEM) {
        nav->page=TDECK_SYSTEM;
        return;
    }
    if (nav->page==TDECK_MAP) {
        if (action==TDECK_MENU || action==TDECK_BACK) nav->page=TDECK_LIST;
        else if (action==TDECK_RADIO) nav->page=TDECK_RF;
        else map_controls(&nav->map,action);
        return;
    }
    if (action==TDECK_OPEN_MAP) {
        nav->page=TDECK_MAP;
        nav->map=(TdeckMap){0};
        tdeck_map_view(&nav->map,(TdeckPosition){0});
        return;
    }
    if (action==TDECK_LEFT || action==TDECK_UP) action=TDECK_PREV;
    if (action==TDECK_RIGHT || action==TDECK_DOWN) action=TDECK_NEXT;
    if (nav->page==TDECK_LANGUAGE) {
        if (action==TDECK_PREV || action==TDECK_NEXT) {
            nav->language_choice=nav->language_choice==TDECK_ENGLISH ? TDECK_ITALIAN : TDECK_ENGLISH;
            nav->language_save_error=false;
        } else if (action==TDECK_BACK || action==TDECK_MENU || action==TDECK_OPEN) {
            nav->page=TDECK_SYSTEM;
        }
        return;
    }
    uint32_t addresses[CARDS];
    unsigned count=history_aircraft(boot,addresses);
    sync_selection(nav,addresses,count);
    if (action==TDECK_BACK) nav->page=TDECK_LIST;
    if (action==TDECK_RADIO) nav->page=TDECK_RF;
    if (action==TDECK_MENU) nav->page=nav->page==TDECK_SYSTEM ? TDECK_LIST : TDECK_SYSTEM;
    if (nav->page==TDECK_SYSTEM) {
        if (action==TDECK_PREV || action==TDECK_NEXT)
            nav->menu_item=(nav->menu_item+(action==TDECK_NEXT ? 1 : SYSTEM_CHOICES-1))%SYSTEM_CHOICES;
        if (action==TDECK_OPEN)
            nav->page=system_destinations[nav->menu_item];
        return;
    }
    if (action==TDECK_OPEN) {
        if (nav->page==TDECK_RF || nav->page>=TDECK_BRIGHTNESS)
            nav->page=TDECK_SYSTEM;
        else if (count) nav->page=nav->page==TDECK_LIST ? TDECK_FLIGHT : TDECK_LIST;
    }
    if (count && (nav->page==TDECK_LIST || nav->page==TDECK_FLIGHT) &&
        (action==TDECK_PREV || action==TDECK_NEXT)) {
        nav->index=(nav->index+(action==TDECK_NEXT ? 1 : count-1))%count;
        nav->selected=addresses[nav->index];
    }
}

void tdeck_navigate_replay(TdeckNav *nav, TdeckAction action,
                          const TdeckReplayAircraft *aircraft, unsigned count) {
    if (!count || nav->index>=count) return;
    bool back=action==TDECK_MENU || action==TDECK_BACK;
    if (nav->page==TDECK_MAP) {
        if (back) nav->page=TDECK_FLIGHT;
        else map_controls(&nav->map,action);
    } else if (nav->page==TDECK_FLIGHT) {
        const AircraftInfo *info=&aircraft[nav->index].info;
        if (back) nav->page=TDECK_LIST;
        else if (info->fixed && (action==TDECK_OPEN || action==TDECK_OPEN_MAP)) {
            nav->page=TDECK_MAP;
            nav->map=(TdeckMap){0};
            tdeck_map_view(&nav->map,(TdeckPosition){info->lat,info->lon,true});
        }
    } else if (nav->page==TDECK_LIST) {
        if (back) nav->page=TDECK_REPLAY_FILES;
        else if (action==TDECK_OPEN) nav->page=TDECK_FLIGHT;
        else if (action==TDECK_DOWN || action==TDECK_RIGHT || action==TDECK_NEXT)
            nav->index=(nav->index+1)%count;
        else if (action==TDECK_UP || action==TDECK_LEFT || action==TDECK_PREV)
            nav->index=(nav->index+count-1)%count;
    }
}

static void aircraft_name(const AircraftInfo *info, char name[9]) {
    if (info->callsign[0]) strcpy(name,info->callsign);
    else snprintf(name,9,"%06X",(unsigned)info->icao);
}

static void aircraft_age(char age[16], const AircraftInfo *info, int64_t now) {
    uint64_t seconds=now>info->seen ? (uint64_t)(now-info->seen)/1000 : 0;
    unsigned value=(unsigned)(seconds<60 ? seconds : seconds<3600 ? seconds/60 :
                              seconds<360000 ? seconds/3600 : 99);
    snprintf(age,16,"%u %s",value,seconds<60 ? "s" : seconds<3600 ? "min" : "h");
}

static void grouped(char dst[24], long n) {
    char digits[24];
    snprintf(digits,sizeof(digits),"%ld",n);
    const char *p=digits;
    char *out=dst;
    if (*p=='-') *out++=*p++;
    size_t remaining=strlen(p);
    while (remaining) {
        *out++=*p++;
        if (--remaining && remaining%3==0) *out++=' ';
    }
    *out=0;
}

static bool aircraft_altitude(TdeckLanguage lang, const AircraftInfo *info, char altitude[24], const char **unit) {
    const char *p=info->altitude;
    if (!strncmp(p,"Alt ",4)) { p+=4; *unit="ft"; }
    else if (!strncmp(p,"GNSS ",5)) { p+=5; *unit="m"; }
    else {
        strcpy(altitude,!strcmp(p,"On ground") ? TR(lang,"On ground","A terra") : "--");
        *unit="";
        return false;
    }
    grouped(altitude,strtol(p,NULL,10));
    return true;
}

static void motion(const AircraftInfo *info, const char **speed, char heading[16]) {
    *speed=info->speed;
    if (!strncmp(*speed,"GS ",3)) *speed+=3;
    const char *angle=strchr(info->heading,' ');
    if (angle) snprintf(heading,16,"%03ld\xb0",strtol(angle,NULL,10));
    else strcpy(heading,"--");
}

static void comma(char *s) {
    for (; *s; s++) if (*s=='.') *s=',';
}

static void battery_label(char value[24], const TdeckStatus *status, bool compact) {
    TdeckLanguage lang=status->language;
    int mv=status->battery_mv;
    if (status->usb_power) strcpy(value,compact ? "USB" : TR(lang,"USB power","Alimentazione USB"));
    else if (!mv) strcpy(value,compact ? "--" : TR(lang,"Unavailable","Non disponibile"));
    else if (compact) {
        /* Same voltage-based estimate as the T-Echo. */
        int percent=mv<=3300 ? 0 : mv<3600 ? (mv-3300)/30 :
                    mv<4200 ? 10+(mv-3600)*15/100 : 100;
        snprintf(value,24,"%d%%",percent);
    } else snprintf(value,24,TR(lang,"%d.%02d V","%d,%02d V"),mv/1000,mv%1000/10);
}

static void header(uint16_t *p, const TdeckStatus *status, const char *middle, const char *channel) {
    TdeckLanguage lang=status->language;
    const char *state=status->replay ? "REPLAY" : status->ready ? "LIVE" :
                      status->retry ? TR(lang,"RX error","RX errore") : TR(lang,"Starting","Avvio");
    rect(p,14,13,5,5,AMBER);
    text(p,24,20,&small_font,AMBER,state);
    center(p,160,20,&small_font,TEXT,middle);
    right(p,306,20,&small_font,DIM,channel);
    rect(p,0,30,320,1,RULE);
    rect(p,0,205,320,35,STRIP);
    rect(p,0,205,320,1,RULE);
}

static void empty(uint16_t *p, const TdeckStatus *status, int64_t now) {
    TdeckLanguage lang=status->language;
    center(p,160,94,&name_font,TEXT,status->retry && !status->ready ? TR(lang,"Radio error","Radio in errore") : TR(lang,"Listening","In ascolto"));
    center(p,160,117,&small_font,DIM,status->retry && !status->ready ?
           TR(lang,"Retrying radio","Nuovo tentativo in corso") : TR(lang,"Waiting for the first ADS-B message","In attesa del primo messaggio ADS-B"));
    char captures[16];
    snprintf(captures,sizeof(captures),"%u",status->bursts);
    center(p,160,163,&dial_font,AMBER,captures);
    const char *label=TR(lang,"RF captures","Catture RF");
    center(p,160,184,&small_font,DIM,label);
    if (status->ready && status->radio_polled && !status->replay && now%1000<500) {
        int x=160-width(&small_font,label)/2-13;
        rect(p,x+1,176,4,6,AMBER);
        rect(p,x,177,6,4,AMBER);
    }
}

static void format_dial(TdeckLanguage lang, char value[24], uint32_t hz) {
    unsigned khz=(hz+500)/1000;
    snprintf(value,24,TR(lang,"%u.%03u","%u,%03u"),khz/1000,khz%1000);
}

static void flight_page(TdeckLanguage lang, uint16_t *p, const AircraftInfo *info, unsigned index, unsigned count, int64_t now) {
    char name[9], icao[20], alt[24], age[16], heading[16], pos[48], index_text[32];
    const char *unit, *speed;
    aircraft_name(info,name);
    aircraft_age(age,info,now);
    motion(info,&speed,heading);
    snprintf(icao,sizeof(icao),"ICAO %06X",(unsigned)info->icao);
    text(p,14,65,&name_font,TEXT,name);
    right(p,306,65,&small_font,DIM,icao);
    bool numeric=aircraft_altitude(lang,info,alt,&unit);
    const Font *alt_face=numeric ? &alt_font : &name_font;
    text(p,14,112,alt_face,AMBER,alt);
    text(p,23+width(alt_face,alt),112,&small_font,AMBER,unit);
    rect(p,14,130,292,1,RULE);
    text(p,14,149,&small_font,DIM,TR(lang,"Speed","Velocita"));
    text(p,111,149,&small_font,DIM,!strncmp(info->heading,"Hdg ",4) ? TR(lang,"Heading","Prua") : TR(lang,"Track","Rotta"));
    text(p,209,149,&small_font,DIM,TR(lang,"Last message","Ultimo msg"));
    text(p,14,170,&row_font,TEXT,*speed ? speed : "--");
    text(p,111,170,&row_font,TEXT,heading);
    text(p,209,170,&row_font,TEXT,age);
    if (info->fixed) {
        snprintf(pos,sizeof(pos),"%.5f %c / %.5f %c",fabs(info->lat),info->lat<0 ? 'S' : 'N',
                 fabs(info->lon),info->lon<0 ? 'W' : 'E');
        if (lang==TDECK_ITALIAN) comma(pos);
    } else strcpy(pos,TR(lang,"Position unavailable","Posizione non disponibile"));
    text(p,14,192,&small_font,DIM,pos);
    snprintf(index_text,sizeof(index_text),TR(lang,"Aircraft %u of %u","Aereo %u di %u"),index+1,count);
    center(p,160,227,&small_font,DIM,index_text);
    if (count>1) {
        text(p,21,227,&row_font,TEXT,"<");
        right(p,299,227,&row_font,TEXT,">");
    }
}

static void list_page(TdeckLanguage lang, uint16_t *p, const uint32_t *addresses, unsigned count, const TdeckNav *nav,
                      const AircraftInfo *selected, uint32_t boot, int64_t now) {
    char flights[24];
    snprintf(flights,sizeof(flights),"%u %s",count,TR(lang,"aircraft",count==1 ? "aereo" : "aerei"));
    text(p,23,48,&small_font,DIM,flights);
    right(p,228,48,&small_font,DIM,TR(lang,"Altitude (ft)","Quota (ft)"));
    right(p,296,48,&small_font,DIM,TR(lang,"Last seen","Ultimo"));
    unsigned start=nav->index/3*3;
    for (unsigned i=start; i<count && i<start+3; i++) {
        AircraftInfo info;
        if (!history_aircraft_info(boot,addresses[i],now,&info)) continue;
        int y=56+37*(i-start);
        bool active=nav->list_focus==TDECK_FOCUS_AIRCRAFT && i==nav->index;
        if (active) {
            rect(p,14,y,292,37,STRIP);
            rect(p,14,y,2,37,AMBER);
        }
        rect(p,16,y,290,1,RULE);
        char name[9], altitude[24], age[16], reading[28];
        const char *unit;
        aircraft_name(&info,name);
        aircraft_age(age,&info,now);
        aircraft_altitude(lang,&info,altitude,&unit);
        snprintf(reading,sizeof(reading),"%s%s",altitude,!strcmp(unit,"m") ? " m" : "");
        text(p,23,y+24,&row_font,active ? AMBER : TEXT,name);
        right(p,228,y+23,&small_font,TEXT,reading);
        right(p,296,y+23,&small_font,TEXT,age);
    }
    char icao[20], detail[48], heading[16];
    const char *speed;
    snprintf(icao,sizeof(icao),"ICAO %06X",(unsigned)selected->icao);
    motion(selected,&speed,heading);
    bool has_heading=selected->heading[0]!=0;
    if (*speed && has_heading) snprintf(detail,sizeof(detail),"%s / %s",speed,heading);
    else snprintf(detail,sizeof(detail),"%s",*speed ? speed : has_heading ? heading : TR(lang,"Motion unavailable","Moto non disponibile"));
    text(p,14,186,&small_font,DIM,icao);
    right(p,306,186,&small_font,TEXT,detail);
}

static void list_buttons(TdeckLanguage lang, uint16_t *p, TdeckListFocus focus, bool has_aircraft) {
    const char *const labels[]={TR(lang,"Map","Mappa"),TR(lang,"SD Replay","Replay SD"),TR(lang,"System","Sistema")};
    rect(p,0,198,TDECK_WIDTH,42,STRIP);
    rect(p,0,198,TDECK_WIDTH,1,RULE);
    for (unsigned i=0; i<3; i++) {
        int x=14+99*i;
        bool active=focus==TDECK_FOCUS_MAP+i;
        rect(p,x,202,94,21,active ? AMBER : RGB(129,120,105));
        if (!active) rect(p,x+1,203,92,19,STRIP);
        center(p,x+47,217,&row_font,active ? BG : TEXT,labels[i]);
    }
    const char *help=focus!=TDECK_FOCUS_AIRCRAFT ? TR(lang,"Left/right to select  /  Click to open","Sinistra/destra per scegliere  /  Clic per aprire") :
                     has_aircraft ? TR(lang,"Up/down for aircraft  /  Left/right for menu","Su/giu per gli aerei  /  Sinistra/destra per il menu") :
                                    TR(lang,"Left/right for menu","Sinistra/destra per il menu");
    center(p,160,236,&small_font,DIM,help);
}

static void rf_page(uint16_t *p, const TdeckStatus *status) {
    TdeckLanguage lang=status->language;
    char dial[24], value[16];
    format_dial(lang,dial,status->dial);
    text(p,14,49,&small_font,DIM,TR(lang,"Tuning","Sintonia"));
    text(p,14,80,&dial_font,AMBER,dial);
    text(p,24+width(&dial_font,dial),80,&small_font,AMBER,"MHz");
    const Capture *trace=status->trace;
    text(p,14,108,&small_font,DIM,trace ? TR(lang,"Last capture","Ultima cattura") : TR(lang,"No captures","Nessuna cattura"));
    right(p,306,108,&small_font,DIM,TR(lang,"Relative RSSI","RSSI relativo"));
    rect(p,14,114,292,47,STRIP);
    for (int y=125; y<161; y+=11) rect(p,14,y,292,1,RULE);
    if (trace) {
        unsigned strongest=trace->floor;
        for (unsigned i=0; i<SAMPLES; i++) if (trace->raw[i]<strongest) strongest=trace->raw[i];
        int range=trace->floor-strongest;
        if (range<12) range=12;
        int x=16, y=157;
        for (int i=0; i<SAMPLES; i++) {
            int power=trace->floor-trace->raw[i];
            int next_y=157-(power>0 ? power*38/range : 0);
            int next_x=16+287*i/(SAMPLES-1);
            if (i) line(p,x,y,next_x,next_y,AMBER);
            x=next_x;
            y=next_y;
        }
    }
    const unsigned stats[]={status->df17,status->bursts,status->queue_full};
    const char *labels[]={TR(lang,"Valid DF17","DF17 validi"),TR(lang,"Captures","Catture"),TR(lang,"Queue full","Coda piena")};
    for (int i=0; i<3; i++) {
        snprintf(value,sizeof(value),"%u",stats[i]);
        text(p,14+i*99,181,&row_font,TEXT,value);
        text(p,14+i*99,197,&small_font,DIM,labels[i]);
    }
    text(p,14,227,&small_font,DIM,TR(lang,"1090 MHz ADS-B channel","Canale ADS-B 1090 MHz"));
    right(p,306,227,&small_font,DIM,status->replay ? TR(lang,"Playback","Registrazione") : status->ready ? TR(lang,"RX active","RX attivo") : TR(lang,"RX waiting","RX in attesa"));
}

static void gps_label(char value[32], const TdeckStatus *status) {
    TdeckLanguage lang=status->language;
    const char *const power_labels[]={
        [GPS_SLEEPING]=TR(lang,"Entering standby","Standby in corso"),[GPS_STANDBY]=TR(lang,"Standby","In standby"),
        [GPS_WAKING]=TR(lang,"Waking up","Riattivazione in corso"),[GPS_WAKE_FAILED]=TR(lang,"Wake unconfirmed","Risveglio da verificare")
    };
    if (status->gps_power!=GPS_RUNNING) {
        snprintf(value,32,"%s",power_labels[status->gps_power]);
        return;
    }
    const char *label;
    switch (status->gps) {
    case GPS_FIX: label=TR(lang,"Valid fix","Fix valido"); break;
    case GPS_NO_FIX: label=TR(lang,"No fix","Senza fix"); break;
    case GPS_NMEA: label=TR(lang,"NMEA received","NMEA ricevuto"); break;
    case GPS_UART_ERROR: label=TR(lang,"UART error","Errore UART"); break;
    default: label=TR(lang,"No data","Nessun dato"); break;
    }
    if ((status->gps==GPS_FIX || status->gps==GPS_NO_FIX) && status->satellites>=0)
        snprintf(value,32,"%s / %d sat",label,status->satellites);
    else snprintf(value,32,"%s",label);
}

static void system_page(uint16_t *p, const TdeckNav *nav, const TdeckStatus *status) {
    TdeckLanguage lang=status->language;
    const char *title=nav->page==TDECK_SYSTEM ? TR(lang,"System","Sistema") :
                      nav->page==TDECK_BRIGHTNESS ? TR(lang,"Brightness","Luminosita") : TR(lang,"Tuning","Sintonia");
    header(p,status,title,"T-Deck Plus");
    if (nav->page==TDECK_SYSTEM) {
        char battery[24], gps[32], frequency[24], level[16];
        battery_label(battery,status,false);
        gps_label(gps,status);
        text(p,14,49,&small_font,DIM,TR(lang,"Battery (estimated)","Batteria (stima)"));
        right(p,306,49,&small_font,AMBER,battery);
        text(p,14,67,&small_font,DIM,"GPS");
        right(p,306,67,&small_font,TEXT,gps);
        text(p,14,85,&small_font,DIM,"Bluetooth LE");
        right(p,306,85,&small_font,TEXT,TR(lang,"Off","Disattivato"));
        format_dial(lang,frequency,status->dial);
        snprintf(level,sizeof(level),"%u/16",status->brightness);
        const char *labels[SYSTEM_CHOICES]={
            TR(lang,"RF reception","Ricezione RF"),
            TR(lang,"Brightness","Luminosita"),
            TR(lang,"Tuning (MHz)","Sintonia (MHz)"),
            TR(lang,"GPS reception","Ricezione GPS"),
            TR(lang,"SD Replay","Replay SD"),
            TR(lang,"Date and time","Data e ora"),
            TR(lang,"Language","Lingua"),
            TR(lang,"Screen off","Spegni schermo"),
            TR(lang,"Restart","Riavvia")
        };
        const char *values[SYSTEM_CHOICES]={
            status->ready ? TR(lang,"RX active","RX attivo") : TR(lang,"RX waiting","RX in attesa"),level,frequency,"","R","",lang==TDECK_ITALIAN ? "Italiano" : "English","L",""
        };
        unsigned start=nav->menu_item>=4 ? nav->menu_item-3 : 0;
        for (unsigned i=start; i<start+4; i++) {
            int y=96+26*(i-start);
            if (i==nav->menu_item) {
                rect(p,14,y,292,26,STRIP);
                rect(p,14,y,2,26,AMBER);
            }
            rect(p,16,y,290,1,RULE);
            text(p,23,y+19,&row_font,i==nav->menu_item ? AMBER : TEXT,labels[i]);
            right(p,296,y+19,&small_font,DIM,values[i]);
        }
        center(p,160,227,&small_font,DIM,TR(lang,"Click to open  /  Hold to return to LIVE","Premi per aprire  /  Tieni per tornare alla LIVE"));
        return;
    }
    if (nav->page==TDECK_BRIGHTNESS) {
        char level[16];
        snprintf(level,sizeof(level),"%u",status->brightness);
        center(p,160,110,&alt_font,AMBER,level);
        center(p,160,138,&small_font,DIM,TR(lang,"Level 1 to 16","Livello da 1 a 16"));
    } else {
        char frequency[24];
        format_dial(lang,frequency,status->requested);
        center(p,160,110,&dial_font,AMBER,frequency);
        center(p,160,136,&small_font,DIM,TR(lang,"MHz  /  50 kHz steps","MHz  /  Passo 50 kHz"));
        if (status->failed) center(p,160,166,&small_font,DIM,TR(lang,"Radio rejected frequency","Frequenza rifiutata dalla radio"));
        else if (status->requested!=status->dial) center(p,160,166,&small_font,DIM,TR(lang,"Tuning","Sintonia in corso"));
    }
    center(p,160,227,&small_font,DIM,TR(lang,"Roll to adjust  /  Click to return","Ruota per regolare  /  Premi per tornare"));
}

static void gps_page(uint16_t *p, const TdeckStatus *status) {
    TdeckLanguage lang=status->language;
    char battery[24], fix[32], serial[48];
    battery_label(battery,status,true);
    gps_label(fix,status);
    header(p,status,TR(lang,"GPS reception","Ricezione GPS"),battery);
    text(p,14,51,&row_font,AMBER,fix);
    right(p,306,51,&small_font,DIM,status->gps_model && *status->gps_model ?
          status->gps_model : TR(lang,"Module unidentified","Modulo da identificare"));
    text(p,14,72,&small_font,DIM,TR(lang,"Constellation","Costellazione"));
    right(p,229,72,&small_font,DIM,TR(lang,"In view","In vista"));
    right(p,306,72,&small_font,DIM,TR(lang,"In fix","Nel fix"));
    static const struct { unsigned id; const char *name; } rows[]={
        {GNSS_GPS,"GPS"},{GNSS_GALILEO,"Galileo"},{GNSS_GLONASS,"GLONASS"},
        {GNSS_BEIDOU,"BeiDou"},{GNSS_QZSS,"QZSS"},{GNSS_SBAS,"SBAS"}
    };
    const GnssSatellites *sat=status->gps_constellations;
    for (unsigned i=0; i<sizeof(rows)/sizeof(*rows); i++) {
        int y=77+18*i;
        unsigned id=rows[i].id;
        char visible[8]="--", used[8]="--";
        if (sat) {
            snprintf(visible,sizeof(visible),"%u",sat->visible[id]);
            snprintf(used,sizeof(used),"%u",sat->used[id]);
        }
        rect(p,14,y,292,1,RULE);
        text(p,14,y+13,&small_font,TEXT,rows[i].name);
        right(p,229,y+13,&small_font,TEXT,visible);
        right(p,306,y+13,&small_font,sat && sat->used[id] ? AMBER : TEXT,used);
    }
    snprintf(serial,sizeof(serial),TR(lang,"%lu valid NMEA sentences","%lu NMEA validi"),(unsigned long)status->gps_sentences);
    text(p,14,198,&small_font,DIM,serial);
    if (status->gps_baud) {
        snprintf(serial,sizeof(serial),"%u baud",status->gps_baud);
        right(p,306,198,&small_font,DIM,serial);
    }
    const char *action=TR(lang,"Hold for System","Tieni premuto per Sistema");
    if (status->gps_can_sleep && status->gps_power==GPS_RUNNING) action=TR(lang,"Click for standby  /  Hold for System","Premi per standby  /  Tieni per Sistema");
    if (status->gps_power==GPS_STANDBY || status->gps_power==GPS_WAKE_FAILED)
        action=TR(lang,"Click to wake  /  Hold for System","Premi per riattivare  /  Tieni per Sistema");
    center(p,160,227,&small_font,DIM,action);
}

static void map_marker(TdeckLanguage lang, uint16_t *p, const AircraftInfo *aircraft, int x, int y, bool selected) {
    uint16_t color=selected ? AMBER : RGB(235,199,146);
    int course=0;
    bool oriented=sscanf(aircraft->heading,"%*s %d",&course)==1;
    if (oriented) {
        double angle=course*0.017453292519943295;
        int dx=lround(7*sin(angle)), dy=lround(-7*cos(angle));
        line(p,x-dx/2,y-dy/2,x+dx,y+dy,color);
        line(p,x+dx,y+dy,x-dy/2,y+dx/2,color);
        line(p,x+dx,y+dy,x+dy/2,y-dx/2,color);
        if (selected) line(p,x+dx+dx/2,y+dy+dy/2,x+2*dx,y+2*dy,RGB(151,117,71));
    } else rect(p,x-2,y-2,5,5,color);
    char name[9], height[24];
    aircraft_name(aircraft,name);
    const char *unit;
    bool altitude=selected && aircraft_altitude(lang,aircraft,height,&unit);
    char subtitle[26];
    if (altitude) snprintf(subtitle,sizeof(subtitle),"%.19s %s",height,unit);
    int box=width(&small_font,name)+10;
    if (altitude && width(&small_font,subtitle)+10>box) box=width(&small_font,subtitle)+10;
    int left=x+box+12<MAP_WIDTH ? x+10 : x-box-8;
    int top=y-11;
    if (top<53) top=53;
    if (top>174) top=174;
    rect(p,left,top,box,altitude?29:16,STRIP);
    if (selected) rect(p,left,top,1,altitude?29:16,AMBER);
    text(p,left+5,top+12,&small_font,color,name);
    if (altitude) text(p,left+5,top+25,&small_font,DIM,subtitle);
}

static bool plot_aircraft(TdeckLanguage lang, uint16_t *p, const AircraftInfo *info, TdeckMapView view, bool selected) {
    int x,y;
    if (!tdeck_map_point(view,(TdeckPosition){info->lat,info->lon,info->fixed},&x,&y) ||
        x<14 || x>MAP_WIDTH-14 || y<19 || y>MAP_HEIGHT-16) return false;
    map_marker(lang,p,info,x,y+MAP_TOP,selected);
    return true;
}

static void map_page(uint16_t *p, const TdeckNav *nav, const TdeckStatus *status,
                     const uint32_t *addresses, unsigned count, int64_t now) {
    TdeckLanguage lang=status->language;
    char battery[24], label[40];
    battery_label(battery,status,true);
    header(p,status,TR(lang,"Traffic map","Mappa traffico"),battery);
    if (status->map_pixels && status->map_tiles)
        memcpy(p+MAP_TOP*MAP_WIDTH,status->map_pixels,MAP_WIDTH*MAP_HEIGHT*sizeof(*p));
    else rect(p,0,MAP_TOP,MAP_WIDTH,MAP_HEIGHT,RGB(20,32,44));
    if (!status->map_tiles) {
        const char *reason=!status->map_available ? TR(lang,"MicroSD unavailable","MicroSD non disponibile") :
                           status->map_loading ? TR(lang,"Loading map","Caricamento mappa") : TR(lang,"Map tiles unavailable","Tessere non disponibili");
        center(p,160,status->archive ? 83 : 119,&small_font,DIM,reason);
    }

    int mx,my;
    if (!status->replay && tdeck_map_point(status->map_view,status->gps_position,&mx,&my) &&
        mx>=8 && mx<MAP_WIDTH-8 && my>=8 && my<MAP_HEIGHT-10) {
        my+=MAP_TOP;
        uint16_t gps_color=RGB(143,220,229);
        rect(p,mx-4,my-4,9,9,RGB(38,60,68));
        rect(p,mx-2,my-2,5,5,gps_color);
        text(p,mx+8,my+4,&small_font,gps_color,TR(lang,"YOU","TU"));
    }
    if (nav->map.zooming) {
        int x=MAP_WIDTH/2,y=MAP_TOP+MAP_HEIGHT/2;
        line(p,x-6,y,x+6,y,DIM);
        line(p,x,y-6,x,y+6,DIM);
    }
    if (status->archive) {
        const TdeckReplayAircraft *entry=&status->archive[nav->index];
        AircraftInfo marker=entry->info;
        /* The latest heading and altitude may belong to other messages. */
        marker.heading[0]=marker.altitude[0]=0;
        plot_aircraft(lang,p,&marker,status->map_view,true);
        if (*entry->position_at) snprintf(label,sizeof(label),TR(lang,"Position %s","Posizione %s"),entry->position_at);
        else strcpy(label,TR(lang,"Position time unknown","Posizione senza data e ora"));
    } else {
        unsigned drawn=0;
        for (unsigned pass=0; pass<2; pass++) {
            for (unsigned i=0; i<count; i++) {
                bool selected=addresses[i]==nav->selected;
                if (selected!=(pass==1) || (!selected && drawn>=6)) continue;
                AircraftInfo info;
                if (history_aircraft_info(status->boot,addresses[i],now,&info) &&
                    plot_aircraft(lang,p,&info,status->map_view,selected)) drawn++;
            }
        }
        strcpy(label,status->replay ? TR(lang,"History","Storico") : status->gps_position.valid ? TR(lang,"GPS active","GPS attivo") : TR(lang,"No GPS fix","Senza fix GPS"));
    }
    rect(p,7,35,width(&small_font,label)+8,16,BG);
    text(p,11,47,&small_font,DIM,label);
    if (!status->archive && status->map_loading && status->map_tiles) {
        rect(p,108,35,104,16,BG);
        center(p,160,47,&small_font,DIM,TR(lang,"Loading","Caricamento"));
    }
    rect(p,286,35,25,28,BG);
    center(p,298,47,&small_font,TEXT,"N");
    line(p,298,50,294,57,TEXT);
    line(p,298,50,302,57,TEXT);
    double per_pixel=tdeck_map_metres_per_pixel(status->map_view);
    static const double distances[]={0.01,0.025,0.05,0.1,0.25,0.5,1,2,5,10,20,50,100,200,500,1000};
    double nm=distances[0];
    for (unsigned i=0; i<sizeof(distances)/sizeof(*distances); i++)
        if (distances[i]*1852/per_pixel<=49) nm=distances[i];
    int length=(int)lround(nm*1852/per_pixel);
    snprintf(label,sizeof(label),"%g NM",nm);
    rect(p,7,186,65,22,BG);
    text(p,12,198,&small_font,TEXT,label);
    line(p,12,203,12+length,203,DIM);
    line(p,12,201,12,205,DIM);
    line(p,12+length,201,12+length,205,DIM);
    if (status->map_tiles) {
        const char *credit="(c) OpenStreetMap";
        rect(p,305-width(&small_font,credit),193,width(&small_font,credit)+7,16,BG);
        right(p,308,205,&small_font,DIM,credit);
    }
    rect(p,0,MAP_TOP+MAP_HEIGHT,320,27,STRIP);
    rect(p,0,MAP_TOP+MAP_HEIGHT,320,1,RULE);
    text(p,14,224,&small_font,AMBER,nav->map.zooming ? "ZOOM" : TR(lang,"PAN","SPOSTA"));
    center(p,163,224,&small_font,TEXT,nav->map.zooming ? TR(lang,"Up/down to zoom  /  Click to pan","Su/giu zoom  /  Clic per spostare") : TR(lang,"Click to zoom","Clic per zoom"));
    snprintf(label,sizeof(label),"Z%u",status->map_view.zoom);
    right(p,306,224,&small_font,DIM,label);
    center(p,160,237,&small_font,AMBER,status->archive ? TR(lang,"Hold to return to flight details","Tieni premuto per tornare alla scheda") :
                                                       TR(lang,"Hold to return to LIVE","Tieni per tornare alla LIVE"));
}

static void replay_list(uint16_t *p, const TdeckNav *nav, const TdeckStatus *status, const char *battery) {
    TdeckLanguage lang=status->language;
    header(p,status,status->archive_name,battery);
    char summary[32];
    snprintf(summary,sizeof(summary),TR(lang,"%u %s in log","%u %s nel registro"),status->archive_count,
             TR(lang,"aircraft",status->archive_count==1 ? "aereo" : "aerei"));
    text(p,14,52,&small_font,DIM,summary);
    if (status->archive_count>3) {
        snprintf(summary,sizeof(summary),"%u / %u",nav->index+1,status->archive_count);
        right(p,306,52,&small_font,DIM,summary);
    }
    unsigned first=nav->index/3*3;
    for (unsigned i=first; i<status->archive_count && i<first+3; i++) {
        const AircraftInfo *info=&status->archive[i].info;
        char name[9], altitude[24], reading[32];
        const char *unit;
        aircraft_name(info,name);
        aircraft_altitude(lang,info,altitude,&unit);
        snprintf(reading,sizeof(reading),"%s%s%s",altitude,*unit ? " " : "",unit);
        int y=62+44*(int)(i-first);
        bool selected=i==nav->index;
        if (selected) {
            rect(p,14,y,292,44,STRIP);
            rect(p,14,y,2,44,AMBER);
        }
        rect(p,16,y,290,1,RULE);
        text(p,23,y+27,&row_font,selected ? AMBER : TEXT,name);
        right(p,217,y+26,&small_font,TEXT,reading);
        right(p,296,y+26,&small_font,DIM,info->fixed ? TR(lang,"Position","Posizione") : TR(lang,"Details >","Scheda >"));
    }
    center(p,160,227,&small_font,DIM,TR(lang,"Click: details  /  Hold: files  /  R: LIVE","Clic: scheda  /  Tieni: file  /  R: LIVE"));
}

static void replay_flight(uint16_t *p, const TdeckReplayAircraft *entry, const char *battery,
                          const TdeckStatus *status) {
    TdeckLanguage lang=status->language;
    const AircraftInfo *info=&entry->info;
    char name[9], icao[20], alt[24], feet[32], metres[32], digits[24];
    char speed[32]="--", kmh[32]="--", speed_label[20], heading[16];
    strcpy(speed_label,TR(lang,"Speed","Velocita"));
    const char *unit, *ignored;
    aircraft_name(info,name);
    header(p,status,TR(lang,"Flight details","Scheda volo"),battery);
    const char *received=*entry->received_at ? entry->received_at : TR(lang,"Time not recorded","Data e ora non registrate");
    int reception_left=306-width(&small_font,received)-8;
    const Font *name_face=&name_font;
    if (14+width(name_face,name)>reception_left) name_face=&row_font;
    if (14+width(name_face,name)>reception_left) name_face=&small_font;
    text(p,14,65,name_face,AMBER,name);
    if (*info->callsign) {
        snprintf(icao,sizeof(icao),"ICAO %06X",(unsigned)info->icao);
        text(p,14,80,&small_font,DIM,icao);
    }
    right(p,306,55,&small_font,DIM,TR(lang,"Last received","Ultima ricezione"));
    right(p,306,74,&small_font,*entry->received_at ? TEXT : DIM,received);
    right(p,215,101,&small_font,DIM,TR(lang,"Aviation","Aeronautico"));
    right(p,306,101,&small_font,DIM,TR(lang,"Metric","Metrico"));

    bool altitude=aircraft_altitude(lang,info,alt,&unit);
    bool gnss=altitude && *unit=='m';
    if (altitude) {
        long value=strtol(info->altitude+(gnss ? 5 : 4),NULL,10);
        grouped(digits,gnss ? lround(value/0.3048) : value);
        snprintf(feet,sizeof(feet),"%s ft",digits);
        grouped(digits,gnss ? value : lround(value*0.3048));
        snprintf(metres,sizeof(metres),"%s m",digits);
    } else {
        strcpy(feet,alt);
        strcpy(metres,"--");
    }
    const char *speed_value=strchr(info->speed,' ');
    if (speed_value) {
        speed_value++;
        snprintf(speed_label,sizeof(speed_label),TR(lang,"Speed %.3s","Velocita %.3s"),info->speed);
        snprintf(speed,sizeof(speed),"%s",speed_value);
        if (lang==TDECK_ITALIAN) comma(speed);
        bool minimum=!strncmp(speed_value,">=",2);
        double value=strtod(speed_value+(minimum ? 2 : 0),NULL)*1.852;
        grouped(digits,minimum ? (long)floor(value) : lround(value));
        snprintf(kmh,sizeof(kmh),"%s%.19s km/h",minimum ? ">=" : "",digits);
    }
    motion(info,&ignored,heading);
    text(p,14,124,&small_font,DIM,gnss ? TR(lang,"GNSS altitude","Quota GNSS") : TR(lang,"Altitude","Quota"));
    right(p,215,124,&row_font,TEXT,feet);
    right(p,306,124,&small_font,DIM,metres);
    text(p,14,148,&small_font,DIM,speed_label);
    right(p,215,148,&row_font,TEXT,speed);
    right(p,306,148,&small_font,DIM,kmh);
    text(p,14,171,&small_font,DIM,!strncmp(info->heading,"Hdg ",4) ? TR(lang,"Heading","Prua") : TR(lang,"Track","Rotta"));
    right(p,215,171,&row_font,TEXT,heading);
    rect(p,14,180,292,1,RULE);
    text(p,14,198,&small_font,info->fixed ? AMBER : DIM,TR(lang,"Open on map","Apri in mappa"));
    right(p,306,198,&small_font,DIM,info->fixed ? TR(lang,"Click to open >","Clic per aprire >") : TR(lang,"No position","Nessuna posizione"));
    center(p,160,227,&small_font,DIM,TR(lang,"Hold: aircraft list  /  R: LIVE","Tieni: lista aerei  /  R: LIVE"));
}

static void replay_files_page(uint16_t *p, const TdeckNav *nav, const TdeckStatus *status) {
    TdeckLanguage lang=status->language;
    char battery[24];
    battery_label(battery,status,true);
    header(p,status,TR(lang,"SD Replay","Replay SD"),battery);
    const TdeckReplayReply *files=status->replay_files;
    if (files && files->state==REPLAY_FILES) {
        unsigned start=nav->replay_index/5*5;
        for (unsigned i=start; i<files->count && i<start+5; i++) {
            int y=41+30*(i-start);
            bool selected=i==nav->replay_index;
            if (selected) rect(p,14,y,292,28,STRIP);
            text(p,24,y+20,&row_font,selected ? AMBER : TEXT,files->names[i]);
        }
        if (files->more) text(p,15,196,&small_font,DIM,TR(lang,"More files via USB","Altri file via USB"));
        center(p,160,227,&small_font,DIM,TR(lang,"Click: open  /  Hold: back  /  R: LIVE","Clic: apri  /  Tieni: indietro  /  R: LIVE"));
        return;
    }
    const char *const messages[]={
        [REPLAY_IDLE]=TR(lang,"Open a log","Apri un registro"),[REPLAY_LOADING]=TR(lang,"Reading microSD","Lettura microSD"),
        [REPLAY_NO_SD]=TR(lang,"MicroSD unavailable","MicroSD non disponibile"),[REPLAY_EMPTY]=TR(lang,"No DF17 messages","Nessun messaggio DF17"),
        [REPLAY_ERROR]=TR(lang,"Log read error","Errore lettura registro"),[REPLAY_TOO_LARGE]=TR(lang,"Log too large","Registro troppo grande")
    };
    TdeckReplayState state=files ? files->state : REPLAY_IDLE;
    center(p,160,116,&row_font,TEXT,messages[state] ? messages[state] : TR(lang,"SD Replay","Replay SD"));
    center(p,160,227,&small_font,DIM,TR(lang,"Click: refresh  /  R: LIVE","Premi: aggiorna  /  R: LIVE"));
}

static const unsigned clock_offsets[TDECK_CLOCK_DIGITS]={0,1,2,3,5,6,8,9,11,12,14,15};

void tdeck_clock_edit_begin(TdeckNav *nav, const char *iso) {
    nav->page=TDECK_CLOCK;
    strcpy(nav->clock_entry,"200001010000");
    if (iso && *iso) {
        for (unsigned i=0;i<TDECK_CLOCK_DIGITS;i++) nav->clock_entry[i]=iso[clock_offsets[i]];
    }
    nav->clock_cursor=0;
    nav->clock_error=false;
    nav->clock_storage_error=false;
}

bool tdeck_clock_edit_navigate(TdeckNav *nav, TdeckAction action) {
    unsigned cursor=nav->clock_cursor;
    if (action==TDECK_OPEN && cursor==TDECK_CLOCK_DIGITS) return true;
    switch (action) {
        case TDECK_OPEN:
        case TDECK_RIGHT:
            if (cursor<TDECK_CLOCK_DIGITS) nav->clock_cursor++;
            break;
        case TDECK_LEFT:
            if (cursor) nav->clock_cursor--;
            break;
        case TDECK_UP:
        case TDECK_DOWN:
            if (cursor<TDECK_CLOCK_DIGITS) {
                int digit=nav->clock_entry[cursor]-'0';
                nav->clock_entry[cursor]='0'+(digit+(action==TDECK_UP ? 1 : 9))%10;
                nav->clock_error=false;
            }
            break;
        default:
            break;
    }
    return false;
}

bool tdeck_clock_edit_key(TdeckNav *nav, uint8_t key) {
    if (key=='\r' || key=='\n') return true;
    if (key==8 || key==127) {
        if (nav->clock_cursor) nav->clock_cursor--;
    } else if (key>='0' && key<='9' && nav->clock_cursor<TDECK_CLOCK_DIGITS) {
        nav->clock_entry[nav->clock_cursor++]=key;
        nav->clock_error=false;
    }
    return false;
}

static void clock_page(uint16_t *p, const TdeckNav *nav, const TdeckStatus *status) {
    TdeckLanguage lang=status->language;
    char battery[24], edit[]="0000-00-00T00:00";
    battery_label(battery,status,true);
    header(p,status,TR(lang,"Date and time","Data e ora"),battery);
    bool running=status->local_time && *status->local_time;
    const char *value=running ? status->local_time : status->saved_time;
    bool available=value && *value;
    center(p,160,52,&small_font,DIM,available ?
           (running ? TR(lang,"Clock running","Orologio in corso") : TR(lang,"Last saved time","Ultimo orario salvato")) : TR(lang,"Set local time","Imposta l'ora locale"));
    if (available) center(p,160,77,&row_font,TEXT,value);
    if (nav->page==TDECK_CLOCK_CONFIRM) {
        if (nav->clock_storage_error)
            center(p,160,102,&small_font,AMBER,TR(lang,"Clock set, save failed","Ora attiva, memoria non scritta"));
        else if (!running)
            center(p,160,102,&small_font,DIM,TR(lang,"Check date and time before confirming","Verifica data e ora prima di confermare"));
        const char *options[]={nav->clock_storage_error ? TR(lang,"Retry save","Riprova a salvare") : TR(lang,"Confirm","Conferma"), TR(lang,"Edit","Modifica")};
        for (unsigned i=0;i<2;i++) {
            int y=116+36*i;
            bool selected=i==(unsigned)nav->clock_modify;
            if (selected) {
                rect(p,14,y,292,32,STRIP);
                rect(p,14,y,2,32,AMBER);
            }
            text(p,26,y+22,&row_font,selected ? AMBER : TEXT,options[i]);
        }
        center(p,160,227,&small_font,DIM,nav->clock_storage_error ?
               TR(lang,"Click: select  /  Hold: continue","Clic: seleziona  /  Tieni: continua") : TR(lang,"Roll to choose  /  Click to select","Muovi: scegli  /  Clic: seleziona"));
        return;
    }
    for (unsigned i=0;i<TDECK_CLOCK_DIGITS;i++) edit[clock_offsets[i]]=nav->clock_entry[i];
    bool save_selected=nav->clock_cursor==TDECK_CLOCK_DIGITS;
    int x=160-width(&row_font,edit)/2;
    for (unsigned i=0;edit[i];i++) {
        char character[]={edit[i],0};
        int w=width(&row_font,character);
        bool selected=!save_selected && i==clock_offsets[nav->clock_cursor];
        if (selected) {
            rect(p,x,95,w,30,STRIP);
            rect(p,x,124,w,2,AMBER);
        }
        text(p,x,118,&row_font,selected ? AMBER : TEXT,character);
        x+=w;
    }
    center(p,160,149,&small_font,nav->clock_error ? AMBER : DIM,nav->clock_error ?
           TR(lang,"Invalid date or time","Data o ora non valida") : TR(lang,"Hold sym for digits. Press Enter to save.","Tieni premuto sym per le cifre. Invio salva."));
    if (save_selected) {
        rect(p,113,160,94,30,STRIP);
        rect(p,113,160,2,30,AMBER);
    }
    center(p,160,181,&row_font,save_selected ? AMBER : TEXT,TR(lang,"Save","Salva"));
    center(p,160,219,&small_font,DIM,save_selected ? TR(lang,"Left: back to digits","Sinistra: torna alle cifre") :
           TR(lang,"Up/down: digit  /  Left/right: cursor","Su/giu: cifra  /  Sin./des.: cursore"));
    const char *click=save_selected ? TR(lang,"Click: save","Clic: salva") : TR(lang,"Click: next","Clic: avanti");
    if (!status->clock_setup || available) click=save_selected ?
        TR(lang,"Click: save  /  Hold: back","Clic: salva  /  Tieni: indietro") : TR(lang,"Click: next  /  Hold: back","Clic: avanti  /  Tieni: indietro");
    center(p,160,233,&small_font,DIM,click);
}

void tdeck_render(uint16_t pixels[TDECK_WIDTH*TDECK_HEIGHT], TdeckNav *nav,
                  const TdeckStatus *status, int64_t now) {
    TdeckLanguage lang=status->language;
    rect(pixels,0,0,320,240,BG);
    if (status->archive && nav->index<status->archive_count) {
        char battery[24];
        battery_label(battery,status,true);
        if (nav->page==TDECK_MAP) map_page(pixels,nav,status,NULL,0,now);
        else if (nav->page==TDECK_FLIGHT) replay_flight(pixels,&status->archive[nav->index],battery,status);
        else replay_list(pixels,nav,status,battery);
        return;
    }
    uint32_t addresses[CARDS];
    unsigned count=history_aircraft(status->boot,addresses);
    sync_selection(nav,addresses,count);
    if (nav->page==TDECK_REPLAY_FILES) {
        replay_files_page(pixels,nav,status);
        return;
    }
    if (nav->page==TDECK_CLOCK || nav->page==TDECK_CLOCK_CONFIRM) {
        clock_page(pixels,nav,status);
        return;
    }
    if (nav->page==TDECK_LANGUAGE) {
        header(pixels,status,TR(lang,"Language","Lingua"),"T-Deck Plus");
        const char *names[]={"English","Italiano"};
        for (unsigned i=0;i<2;i++) {
            int y=73+39*i;
            bool selected=nav->language_choice==i;
            if (selected) {
                rect(pixels,14,y,292,32,STRIP);
                rect(pixels,14,y,2,32,AMBER);
            }
            text(pixels,26,y+22,&row_font,selected ? AMBER : TEXT,names[i]);
        }
        if (nav->language_save_error)
            center(pixels,160,175,&small_font,AMBER,TR(lang,"Could not save language","Salvataggio fallito"));
        center(pixels,160,227,&small_font,DIM,TR(lang,"Click: save  /  Hold: back","Clic: salva  /  Tieni: indietro"));
        return;
    }
    if (nav->page==TDECK_RESTART) {
        header(pixels,status,TR(lang,"Restart","Riavvia"),"T-Deck Plus");
        const char *message=TR(lang,"Closing SD log","Chiusura registro SD");
        if (status->restart_error) message=TR(lang,"SD close failed","Chiusura SD fallita");
        if (status->restart_sd_warning) message=TR(lang,"SD log error","Errore registro SD");
        center(pixels,160,116,&row_font,TEXT,message);
        if (status->restart_sd_warning)
            center(pixels,160,145,&small_font,DIM,TR(lang,"Card unmounted. Restarting...","Scheda smontata. Riavvio..."));
        if (status->restart_error)
            center(pixels,160,227,&small_font,DIM,TR(lang,"Hold to return","Tieni per tornare"));
        return;
    }
    if (nav->page==TDECK_MAP) {
        map_page(pixels,nav,status,addresses,count,now);
        return;
    }
    if (nav->page==TDECK_GPS) {
        gps_page(pixels,status);
        return;
    }
    if (nav->page>=TDECK_SYSTEM) {
        system_page(pixels,nav,status);
        return;
    }
    char battery[24];
    battery_label(battery,status,true);
    if (nav->page==TDECK_RF) {
        header(pixels,status,TR(lang,"RF reception","Ricezione RF"),battery);
        rf_page(pixels,status);
        return;
    }
    char total[24];
    snprintf(total,sizeof(total),"%u %s",count,TR(lang,"aircraft",count==1 ? "aereo" : "aerei"));
    char channel[40];
    snprintf(channel,sizeof(channel),"1090 MHz  /  %s",battery);
    header(pixels,status,nav->page==TDECK_LIST ? TR(lang,"Traffic list","Lista traffico") : total,channel);
    AircraftInfo info;
    if (!count || !history_aircraft_info(status->boot,nav->selected,now,&info)) {
        empty(pixels,status,now);
    } else if (nav->page==TDECK_LIST) list_page(lang,pixels,addresses,count,nav,&info,status->boot,now);
    else flight_page(lang,pixels,&info,nav->index,count,now);
    if (nav->page==TDECK_LIST) list_buttons(lang,pixels,nav->list_focus,count>0);
}
