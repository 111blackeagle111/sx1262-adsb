/* Replay CRC-checked recorded packets through the same view and renderer as the LCD. */
#include "tdeck_ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while (0)

static uint16_t pixels[TDECK_WIDTH*TDECK_HEIGHT];

static void render(const char *name, const char *dir, TdeckNav *nav, const TdeckStatus *status, int64_t now) {
    tdeck_render(pixels,nav,status,now);
    CHECK(pixels[0]==0x2125);
    CHECK(pixels[13*TDECK_WIDTH+14]==0xfe2d);
    if (!dir) return;
    char path[512];
    int length=snprintf(path,sizeof(path),"%s/%s.ppm",dir,name);
    CHECK(length>=0 && (size_t)length<sizeof(path));
    FILE *f=fopen(path,"wb");
    CHECK(f);
    fprintf(f,"P6\n%d %d\n255\n",TDECK_WIDTH,TDECK_HEIGHT);
    for (unsigned i=0; i<TDECK_WIDTH*TDECK_HEIGHT; i++) {
        unsigned r=pixels[i]>>11, g=(pixels[i]>>5)&63, b=pixels[i]&31;
        uint8_t rgb[]={(r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2)};
        CHECK(fwrite(rgb,1,3,f)==3);
    }
    CHECK(fclose(f)==0);
}

/* The caption uses DIM, so the only amber pixels on these rows are the RX light. */
static bool rx_light_visible(TdeckNav *nav, TdeckStatus *status, int64_t now) {
    tdeck_render(pixels,nav,status,now);
    for (unsigned i=176*TDECK_WIDTH;i<182*TDECK_WIDTH;i++)
        if (pixels[i]==0xfe2d) return true;
    return false;
}

static void read_bytes(uint8_t *out, size_t bytes, const char *hex) {
    CHECK(strlen(hex)==2*bytes);
    for (size_t i=0; i<bytes; i++) CHECK(sscanf(hex+2*i,"%2hhx",out+i)==1);
}

static bool archive_progress(void *context) {
    (void)context;
    return true;
}

static void check_button_highlight(TdeckListFocus selected) {
    const unsigned left[]={14,113,212};
    const TdeckListFocus focus[]={TDECK_FOCUS_MAP,TDECK_FOCUS_REPLAY,TDECK_FOCUS_SYSTEM};
    for (unsigned i=0;i<3;i++)
        CHECK((pixels[204*TDECK_WIDTH+left[i]+2]==0xfe2d)==(focus[i]==selected));
}

/* The persistent return label is the only amber text on the last line of the map. */
static unsigned back_label_ink(void) {
    unsigned count=0;
    for (unsigned i=227*TDECK_WIDTH;i<TDECK_HEIGHT*TDECK_WIDTH;i++)
        if (pixels[i]==0xfe2d) count++;
    CHECK(count>0);
    return count;
}

static unsigned test_live_menu(TdeckNav nav, TdeckStatus status, int64_t now, const char *out) {
    status.replay=false;
    uint32_t aircraft=nav.selected;
    CHECK(nav.page==TDECK_LIST && nav.list_focus==TDECK_FOCUS_AIRCRAFT);

    tdeck_navigate(&nav,TDECK_LEFT,status.boot);
    CHECK(nav.page==TDECK_LIST && nav.list_focus==TDECK_FOCUS_MAP);
    render(aircraft ? "lista-menu-mappa" : "in-ascolto-menu-mappa",out,&nav,&status,now);
    check_button_highlight(TDECK_FOCUS_MAP);
    tdeck_navigate(&nav,TDECK_OPEN,status.boot);
    CHECK(nav.page==TDECK_MAP && !nav.map.zooming);
    status.map_view=tdeck_map_view(&nav.map,(TdeckPosition){0});
    render("mappa-live-sposta",aircraft ? out : NULL,&nav,&status,now);
    unsigned live_hint=back_label_ink();
    tdeck_navigate(&nav,TDECK_OPEN,status.boot);
    CHECK(nav.page==TDECK_MAP && nav.map.zooming);
    render("mappa-live-zoom",aircraft ? out : NULL,&nav,&status,now);
    CHECK(back_label_ink()==live_hint);
    tdeck_navigate(&nav,TDECK_MENU,status.boot);
    CHECK(nav.page==TDECK_LIST && nav.list_focus==TDECK_FOCUS_AIRCRAFT && nav.selected==aircraft);

    /* The first horizontal roll selects Mappa from either direction. */
    tdeck_navigate(&nav,TDECK_RIGHT,status.boot);
    CHECK(nav.page==TDECK_LIST && nav.list_focus==TDECK_FOCUS_MAP);
    tdeck_navigate(&nav,TDECK_LEFT,status.boot);
    CHECK(nav.page==TDECK_LIST && nav.list_focus==TDECK_FOCUS_SYSTEM);
    render("lista-menu-sistema",aircraft ? out : NULL,&nav,&status,now);
    check_button_highlight(TDECK_FOCUS_SYSTEM);
    TdeckNav system=nav;
    tdeck_navigate(&system,TDECK_OPEN,status.boot);
    CHECK(system.page==TDECK_SYSTEM);
    tdeck_navigate(&nav,TDECK_RIGHT,status.boot);
    CHECK(nav.page==TDECK_LIST && nav.list_focus==TDECK_FOCUS_MAP);
    tdeck_navigate(&nav,TDECK_RIGHT,status.boot);
    CHECK(nav.page==TDECK_LIST && nav.list_focus==TDECK_FOCUS_REPLAY);
    render("lista-menu-replay",aircraft ? out : NULL,&nav,&status,now);
    check_button_highlight(TDECK_FOCUS_REPLAY);
    TdeckNav picker=nav;
    tdeck_navigate(&picker,TDECK_OPEN,status.boot);
    CHECK(picker.page==TDECK_REPLAY_FILES);

    /* Leave the buttons without opening or changing the current aircraft. */
    tdeck_navigate(&nav,TDECK_UP,status.boot);
    CHECK(nav.page==TDECK_LIST && nav.list_focus==TDECK_FOCUS_AIRCRAFT && nav.selected==aircraft);
    tdeck_navigate(&nav,TDECK_PICK_REPLAY,status.boot);
    CHECK(nav.page==TDECK_LIST && nav.list_focus==TDECK_FOCUS_REPLAY);
    tdeck_navigate(&nav,TDECK_DOWN,status.boot);
    CHECK(nav.page==TDECK_LIST && nav.list_focus==TDECK_FOCUS_AIRCRAFT && nav.selected==aircraft);
    tdeck_navigate(&nav,TDECK_OPEN,status.boot);
    CHECK(nav.page==(aircraft ? TDECK_FLIGHT : TDECK_LIST));
    return live_hint;
}

static void test_clock_setup(const char *out) {
    TdeckNav nav={0};
    TdeckStatus status={.clock_setup=true,.ready=true,.battery_mv=3890};
    tdeck_clock_edit_begin(&nav,NULL);
    render("orologio-primo-avvio",out,&nav,&status,0);
    CHECK(!strcmp(nav.clock_entry,"200001010000"));
    CHECK(!tdeck_clock_edit_navigate(&nav,TDECK_LEFT) && nav.clock_cursor==0);
    CHECK(!tdeck_clock_edit_navigate(&nav,TDECK_OPEN) && nav.clock_cursor==1);

    const char *saved="2026-11-20T23:59:58";
    status.saved_time=saved;
    nav.page=TDECK_CLOCK_CONFIRM;
    nav.clock_modify=true;
    render("orologio-dopo-reset-fisico",out,&nav,&status,0);

    TdeckClock clock={0};
    char time[TDECK_TIME_LENGTH];
    CHECK(!tdeck_clock_format(&clock,1000,time) && !*time);
    CHECK(!tdeck_clock_set_iso(&clock,"2026-02-29T23:00:00",1000));
    CHECK(!tdeck_clock_set_iso(&clock,"2026-11-20T23:59:99",1000));
    CHECK(tdeck_clock_set_iso(&clock,saved,1000));
    CHECK(tdeck_clock_format(&clock,1000,time) && !strcmp(time,saved));
    CHECK(tdeck_clock_format(&clock,5000,time) && !strcmp(time,"2026-11-21T00:00:02"));
    status.local_time=time;
    nav.clock_modify=false;
    render("orologio-dopo-riavvio-software",out,&nav,&status,5000);

    tdeck_clock_edit_begin(&nav,saved);
    CHECK(!strcmp(nav.clock_entry,"202611202359"));
    render("orologio-trackball-cifra",out,&nav,&status,5000);
    for (unsigned i=0;i<TDECK_CLOCK_DIGITS-1;i++)
        CHECK(!tdeck_clock_edit_navigate(&nav,TDECK_RIGHT));
    CHECK(!tdeck_clock_edit_navigate(&nav,TDECK_UP) && nav.clock_entry[11]=='0');
    CHECK(!tdeck_clock_edit_navigate(&nav,TDECK_DOWN) && nav.clock_entry[11]=='9');
    CHECK(!tdeck_clock_edit_navigate(&nav,TDECK_DOWN) && nav.clock_entry[11]=='8');
    CHECK(!tdeck_clock_edit_navigate(&nav,TDECK_OPEN) && nav.clock_cursor==TDECK_CLOCK_DIGITS);
    render("orologio-trackball-salva",out,&nav,&status,5000);
    CHECK(!tdeck_clock_edit_navigate(&nav,TDECK_UP) && !strcmp(nav.clock_entry,"202611202358"));
    CHECK(tdeck_clock_edit_navigate(&nav,TDECK_OPEN));
    CHECK(tdeck_clock_set(&clock,nav.clock_entry,6000));
    CHECK(tdeck_clock_format(&clock,6000,time) && !strcmp(time,"2026-11-20T23:58:00"));
    CHECK(!tdeck_clock_edit_navigate(&nav,TDECK_LEFT) && nav.clock_cursor==TDECK_CLOCK_DIGITS-1);

    tdeck_clock_edit_begin(&nav,NULL);
    for (const char *digit="202702281945"; *digit; digit++)
        CHECK(!tdeck_clock_edit_key(&nav,*digit));
    CHECK(!tdeck_clock_edit_key(&nav,8) && !tdeck_clock_edit_key(&nav,'6'));
    CHECK(tdeck_clock_edit_key(&nav,'\r') && !strcmp(nav.clock_entry,"202702281946"));
    CHECK(tdeck_clock_set(&clock,nav.clock_entry,7000));
    CHECK(tdeck_clock_format(&clock,7000,time) && !strcmp(time,"2027-02-28T19:46:00"));
}

static void test_archive(const char *out, unsigned live_hint) {
    TdeckReplayFrame frames[]={
        {.card={.ms=1000,.length=14},.local_time="2026-11-20T11:04:30"},
        {.card={.ms=3000,.length=14},.local_time="2026-11-20T11:04:32"},
        {.card={.ms=93000,.length=14},.local_time="2026-11-20T11:06:02"},
    };
    read_bytes(frames[0].card.word,14,"8d4baa79580db4913fe0a3be306b");
    read_bytes(frames[1].card.word,14,"8d4baa79580db0fbd3f6267aeda9");
    read_bytes(frames[2].card.word,14,"8d4baa79990c8d821830023908f2");
    TdeckReplayAircraft aircraft[5];
    CHECK(tdeck_replay_aircraft(frames,1,&aircraft[0],archive_progress,NULL));
    CHECK(!aircraft[0].info.fixed && !aircraft[0].position_at[0]);
    CHECK(tdeck_replay_aircraft(frames,3,&aircraft[4],archive_progress,NULL));
    CHECK(aircraft[4].info.fixed && !strcmp(aircraft[4].info.altitude,"Alt 1675 ft"));
    CHECK(!strcmp(aircraft[4].received_at,"2026-11-20T11:06:02"));
    CHECK(!strcmp(aircraft[4].position_at,"2026-11-20T11:04:32"));
    for (unsigned i=1;i<4;i++) aircraft[i]=(TdeckReplayAircraft){.info={.icao=i}};

    TdeckStatus status={.replay=true,.archive=aircraft,.archive_count=5,.archive_name="CPRDEMO.JSL",
                        .battery_mv=3890};
    TdeckNav nav={0};
    tdeck_navigate_replay(&nav,TDECK_OPEN,aircraft,5);
    CHECK(nav.page==TDECK_FLIGHT && nav.index==0);
    tdeck_navigate_replay(&nav,TDECK_OPEN,aircraft,5);
    CHECK(nav.page==TDECK_FLIGHT);
    render("replay-scheda-senza-posizione",out,&nav,&status,95000);
    tdeck_navigate_replay(&nav,TDECK_MENU,aircraft,5);
    CHECK(nav.page==TDECK_LIST);
    for (unsigned i=0;i<4;i++) tdeck_navigate_replay(&nav,TDECK_DOWN,aircraft,5);
    CHECK(nav.index==4);
    render("replay-lista-seconda-pagina",out,&nav,&status,95000);
    tdeck_navigate_replay(&nav,TDECK_OPEN,aircraft,5);
    CHECK(nav.page==TDECK_FLIGHT && nav.index==4);
    render("replay-scheda-storica",out,&nav,&status,95000);
    tdeck_navigate_replay(&nav,TDECK_OPEN,aircraft,5);
    CHECK(nav.page==TDECK_MAP && nav.index==4);
    status.map_view=tdeck_map_view(&nav.map,(TdeckPosition){0});
    int x,y;
    CHECK(tdeck_map_point(status.map_view,
          (TdeckPosition){aircraft[4].info.lat,aircraft[4].info.lon,true},&x,&y));
    CHECK(abs(x-MAP_WIDTH/2)<=1 && abs(y-MAP_HEIGHT/2)<=1);
    render("replay-posizione-storica",out,&nav,&status,95000);
    unsigned archive_hint=back_label_ink();
    CHECK(archive_hint!=live_hint);
    tdeck_navigate_replay(&nav,TDECK_OPEN,aircraft,5);
    CHECK(nav.page==TDECK_MAP && nav.map.zooming);
    render("replay-posizione-zoom",out,&nav,&status,95000);
    CHECK(back_label_ink()==archive_hint);
    tdeck_navigate_replay(&nav,TDECK_MENU,aircraft,5);
    CHECK(nav.page==TDECK_FLIGHT && nav.index==4);
    tdeck_navigate_replay(&nav,TDECK_MENU,aircraft,5);
    CHECK(nav.page==TDECK_LIST && nav.index==4);
    tdeck_navigate_replay(&nav,TDECK_MENU,aircraft,5);
    CHECK(nav.page==TDECK_REPLAY_FILES);
}

static GnssSatellites satellite_report(void) {
    uint8_t records[][12]={
        {[0]=GNSS_GPS,[1]=1,[8]=4|8}, {[0]=GNSS_GPS,[1]=2,[8]=2},
        {[0]=GNSS_GPS,[1]=3,[8]=1}, {[0]=GNSS_GALILEO,[1]=4,[8]=5|8},
        {[0]=GNSS_BEIDOU,[1]=5,[8]=3}, {[0]=GNSS_QZSS,[1]=6,[8]=0}
    };
    uint8_t packet[16+sizeof(records)]={0xb5,0x62,UBX_NAV,UBX_NAV_SAT,8+sizeof(records),
                                        [10]=1,[11]=sizeof(records)/12};
    memcpy(packet+14,records,sizeof(records));
    for (size_t i=2; i<sizeof(packet)-2; i++) {
        packet[sizeof(packet)-2]+=packet[i];
        packet[sizeof(packet)-1]+=packet[sizeof(packet)-2];
    }
    UbxReceiver receiver={0};
    for (size_t i=0; i<sizeof(packet); i++)
        CHECK(tdeck_ubx_feed(&receiver,packet[i])==(i==sizeof(packet)-1));
    GnssSatellites sat;
    CHECK(tdeck_ubx_satellites(&receiver,&sat));
    CHECK(sat.visible[GNSS_GPS]==2 && sat.used[GNSS_GPS]==1);
    CHECK(sat.visible[GNSS_GALILEO]==1 && sat.used[GNSS_GALILEO]==1);
    CHECK(sat.visible[GNSS_BEIDOU]==1 && sat.used[GNSS_BEIDOU]==0);
    CHECK(sat.visible[GNSS_QZSS]==0 && sat.used[GNSS_QZSS]==0);
    packet[sizeof(packet)-1]^=1;
    for (size_t i=0; i<sizeof(packet); i++) CHECK(!tdeck_ubx_feed(&receiver,packet[i]));
    return sat;
}

int main(int argc, char **argv) {
    CHECK(argc==3 || argc==4);
    const char *out=argc==4 ? argv[3] : NULL;
    FILE *frames=fopen(argv[1],"r");
    FILE *trace_file=fopen(argv[2],"r");
    CHECK(frames && trace_file);
    char line[640], hex[513];
    unsigned block, floor;
    long long first, last;
    Capture trace={.dial_hz=1089800000};
    CHECK(fgets(line,sizeof(line),trace_file));
    CHECK(sscanf(line,"%u %lld %lld %u %512s",&block,&first,&last,&floor,hex)==5);
    CHECK(floor<=255);
    trace.block=block; trace.first_ms=first; trace.last_ms=last; trace.floor=floor;
    read_bytes(trace.raw,SAMPLES,hex);
    fclose(trace_file);

    TdeckNav nav={0};
    TdeckStatus status={.boot=1,.dial=1089800000,.ready=true,.replay=true,
                        .battery_mv=3890,.brightness=12,.requested=1089800000};
    history_set_boot(1);
    TdeckStatus live=status;
    live.replay=false;
    live.radio_polled=true;
    for (unsigned lang=TDECK_ENGLISH;lang<=TDECK_ITALIAN;lang++) {
        live.language=lang;
        CHECK(rx_light_visible(&nav,&live,1000));
        CHECK(!rx_light_visible(&nav,&live,1500));
    }
    live.radio_polled=false;
    CHECK(!rx_light_visible(&nav,&live,2000));
    live.radio_polled=true;
    live.ready=false;
    CHECK(!rx_light_visible(&nav,&live,2000));
    live.ready=true;
    live.replay=true;
    CHECK(!rx_light_visible(&nav,&live,2000));
    render("lista-vuota",out,&nav,&status,0);
    uint16_t initial_counter[40*TDECK_WIDTH];
    memcpy(initial_counter,pixels+128*TDECK_WIDTH,sizeof(initial_counter));
    status.bursts=190;
    render("lista-in-ascolto",out,&nav,&status,0);
    CHECK(memcmp(initial_counter,pixels+128*TDECK_WIDTH,sizeof(initial_counter))!=0);
    status.bursts=4294967295u;
    render("lista-contatore-massimo",out,&nav,&status,0);
    CHECK(memcmp(initial_counter,pixels+128*TDECK_WIDTH,sizeof(initial_counter))!=0);
    status.bursts=0;
    tdeck_navigate(&nav,TDECK_OPEN,1);
    CHECK(nav.page==TDECK_LIST);
    test_live_menu(nav,status,0,out);

    /* The first row is the odd half of the recorded 4BAA79 CPR pair. */
    Card card={.boot=1,.length=14};
    CHECK(fgets(line,sizeof(line),frames));
    CHECK(sscanf(line,"%lld %28s",&first,hex)==2);
    card.ms=first;
    read_bytes(card.word,14,hex);
    CHECK(card_valid(&card));
    history_add(card);
    AircraftInfo info;
    CHECK(history_aircraft_info(1,0x4baa79,card.ms,&info));
    CHECK(!info.fixed && !strcmp(info.altitude,"Alt 1675 ft"));
    tdeck_navigate(&nav,TDECK_OPEN,1);
    CHECK(nav.page==TDECK_FLIGHT && nav.selected==0x4baa79);
    render("scheda-quota",out,&nav,&status,card.ms);

    unsigned count=1;
    while (fgets(line,sizeof(line),frames)) {
        CHECK(sscanf(line,"%lld %28s",&last,hex)==2);
        card.ms=last;
        read_bytes(card.word,14,hex);
        CHECK(card_valid(&card));
        history_add(card);
        count++;
    }
    CHECK(!ferror(frames));
    fclose(frames);
    uint32_t addresses[CARDS];
    CHECK(history_aircraft(1,addresses)==3);
    CHECK(history_aircraft(2,addresses)==0);
    CHECK(history_aircraft_info(1,0x4baa79,card.ms,&info));
    CHECK(info.fixed && fabs(info.lat-37.47551)<0.00002 && fabs(info.lon-15.17177)<0.00002);
    CHECK(!strcmp(info.altitude,"Alt 1675 ft"));
    CHECK(history_aircraft_info(1,0x4baa79,card.ms+65000,&info) && !info.fixed && !info.altitude[0]);
    status.df17=count;
    status.bursts=count;
    status.trace=&trace;
    render("scheda-volo",out,&nav,&status,card.ms+2000);
    CHECK(nav.selected==0x4baa79);
    tdeck_navigate(&nav,TDECK_OPEN,1);
    CHECK(nav.page==TDECK_LIST);
    render("lista-traffico",out,&nav,&status,card.ms+2000);
    check_button_highlight(TDECK_FOCUS_AIRCRAFT);
    unsigned live_hint=test_live_menu(nav,status,card.ms+2000,out);
    tdeck_navigate(&nav,TDECK_DOWN,1);
    CHECK(nav.selected!=0x4baa79);
    tdeck_navigate(&nav,TDECK_MENU,1);
    CHECK(nav.page==TDECK_LIST && nav.list_focus==TDECK_FOCUS_SYSTEM);
    tdeck_navigate(&nav,TDECK_OPEN,1);
    CHECK(nav.page==TDECK_SYSTEM);
    status.gps=GPS_NO_FIX;
    status.satellites=0;
    render("sistema",out,&nav,&status,card.ms+2000);
    status.gps=GPS_FIX;
    status.satellites=12;
    status.usb_power=true;
    status.battery_mv=0;
    render("sistema-usb",out,&nav,&status,card.ms+2000);
    status.usb_power=false;
    tdeck_navigate(&nav,TDECK_OPEN,1);
    CHECK(nav.page==TDECK_RF);
    render("ricezione-rf",out,&nav,&status,card.ms+2000);
    tdeck_navigate(&nav,TDECK_OPEN,1);
    CHECK(nav.page==TDECK_SYSTEM);
    tdeck_navigate(&nav,TDECK_NEXT,1);
    tdeck_navigate(&nav,TDECK_OPEN,1);
    CHECK(nav.page==TDECK_BRIGHTNESS);
    render("luminosita",out,&nav,&status,card.ms+2000);
    tdeck_navigate(&nav,TDECK_OPEN,1);
    tdeck_navigate(&nav,TDECK_NEXT,1);
    tdeck_navigate(&nav,TDECK_OPEN,1);
    CHECK(nav.page==TDECK_TUNING);
    render("sintonia",out,&nav,&status,card.ms+2000);
    tdeck_navigate(&nav,TDECK_OPEN,1);
    tdeck_navigate(&nav,TDECK_NEXT,1);
    tdeck_navigate(&nav,TDECK_OPEN,1);
    CHECK(nav.page==TDECK_GPS);
    status.gps_model="MIA-M10Q";
    status.gps_baud=38400;
    status.gps_sentences=124;
    status.battery_mv=3890;
    GnssSatellites sat=satellite_report();
    status.gps_constellations=&sat;
    status.satellites=2;
    render("ricezione-gps",out,&nav,&status,card.ms+2000);
    status.gps_constellations=NULL;
    render("ricezione-gps-in-attesa",out,&nav,&status,card.ms+2000);
    tdeck_navigate(&nav,TDECK_OPEN,1);
    CHECK(nav.page==TDECK_SYSTEM);
    TdeckNav system={.page=TDECK_SYSTEM};
    tdeck_navigate(&system,TDECK_PREV,1);
    tdeck_navigate(&system,TDECK_OPEN,1);
    CHECK(system.page==TDECK_RESTART);
    system.page=TDECK_SYSTEM;
    tdeck_navigate(&system,TDECK_PREV,1);
    tdeck_navigate(&system,TDECK_OPEN,1);
    CHECK(system.page==TDECK_SCREEN_OFF);
    system.page=TDECK_SYSTEM;
    tdeck_navigate(&system,TDECK_PREV,1);
    tdeck_navigate(&system,TDECK_OPEN,1);
    CHECK(system.page==TDECK_LANGUAGE && system.language_choice==TDECK_ENGLISH);
    render("language-english",out,&system,&status,0);
    uint16_t english_title[32*TDECK_WIDTH];
    memcpy(english_title,pixels,sizeof(english_title));
    tdeck_navigate(&system,TDECK_DOWN,1);
    CHECK(system.language_choice==TDECK_ITALIAN && system.page==TDECK_LANGUAGE);
    tdeck_navigate(&system,TDECK_MENU,1);
    CHECK(system.page==TDECK_SYSTEM && status.language==TDECK_ENGLISH);
    tdeck_navigate(&system,TDECK_OPEN,1);
    status.language=TDECK_ITALIAN;
    render("lingua-italiano",out,&system,&status,0);
    CHECK(memcmp(english_title,pixels,sizeof(english_title))!=0);
    tdeck_navigate(&system,TDECK_OPEN,1);
    CHECK(system.page==TDECK_SYSTEM);
    test_clock_setup(out);
    test_archive(out,live_hint);
    return 0;
}
