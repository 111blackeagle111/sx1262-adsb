#include "tdeck_map.h"
#ifdef ESP_PLATFORM
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "esp_heap_caps.h"
#include "miniz.h"
#else
#include <zlib.h>
#endif
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const double pi=3.14159265358979323846;

static void project(TdeckPosition point, double *x, double *y) {
    double lat=fmax(-85.05112878,fmin(85.05112878,point.lat));
    *x=(point.lon+180)/360;
    *y=(1-asinh(tan(lat*pi/180))/pi)/2;
}

TdeckMapView tdeck_map_view(TdeckMap *map, TdeckPosition gps) {
    if (!map->initialized || (map->waiting_fix && gps.valid)) {
        project(gps.valid ? gps : (TdeckPosition){41.89,12.44,false},&map->x,&map->y);
        map->waiting_fix=!gps.valid;
        if (!map->zoom) map->zoom=MAP_DEFAULT_ZOOM;
        map->initialized=true;
    }
    int world=TILE_SIZE<<map->zoom;
    map->x=fmax(MAP_WIDTH/(2.0*world),fmin(1-MAP_WIDTH/(2.0*world),map->x));
    map->y=fmax(MAP_HEIGHT/(2.0*world),fmin(1-MAP_HEIGHT/(2.0*world),map->y));
    return (TdeckMapView){lround(map->x*world-MAP_WIDTH/2.0),
                         lround(map->y*world-MAP_HEIGHT/2.0),map->zoom};
}

void tdeck_map_move(TdeckMap *map, int dx, int dy) {
    map->waiting_fix=false;
    if (map->zooming) {
        int next=(int)map->zoom-dy;
        if (next>=MAP_MIN_ZOOM && next<=MAP_MAX_ZOOM) map->zoom=(unsigned)next;
    } else {
        double step=26.0/(TILE_SIZE<<map->zoom);
        map->x+=step*dx;
        map->y+=step*dy;
    }
}

bool tdeck_map_same(TdeckMapView a, TdeckMapView b) {
    return a.zoom==b.zoom && a.left==b.left && a.top==b.top;
}

bool tdeck_map_point(TdeckMapView view, TdeckPosition position, int *x, int *y) {
    if (!position.valid) return false;
    double wx, wy;
    project(position,&wx,&wy);
    int world=TILE_SIZE<<view.zoom;
    *x=lround(wx*world)-view.left;
    *y=lround(wy*world)-view.top;
    return *x>=0 && *x<MAP_WIDTH && *y>=0 && *y<MAP_HEIGHT;
}

double tdeck_map_metres_per_pixel(TdeckMapView view) {
    double world=TILE_SIZE<<view.zoom;
    double phi=atan(sinh(pi*(1-2*(view.top+MAP_HEIGHT/2.0)/world)));
    return 2*pi*6378137*cos(phi)/world;
}

static unsigned big32(const uint8_t p[4]) {
    return (unsigned)p[0]<<24 | (unsigned)p[1]<<16 | (unsigned)p[2]<<8 | p[3];
}

static uint16_t dark_map(int red, int green, int blue) {
    unsigned luma=(77*red+150*green+29*blue)/256;
    unsigned shade=21+(255-luma)*34/100;
    unsigned r=shade, g=shade+6, b=shade+11;
    if (green-red>14 && blue-red>21 && blue>green && green>142) {
        r=14; g=27; b=38;
    } else if (green-red>8 && green-blue>9 && green>130) {
        r-=4; g+=5; b-=4;
    } else if (red-green>13 && green-blue>14 && red>160) {
        r+=13; g+=7;
    }
    return (r>>3)<<11 | (g>>2)<<5 | (b>>3);
}

static unsigned paeth(int a, int b, int c) {
    int p=a+b-c, da=abs(p-a), db=abs(p-b), dc=abs(p-c);
    return da<=db && da<=dc ? a : db<=dc ? b : c;
}

bool tdeck_map_png(uint8_t *png, size_t size, uint8_t scan[TILE_SCAN_MAX], uint16_t *output) {
    static const uint8_t signature[]={137,80,78,71,13,10,26,10};
    if (size<33 || size>TILE_PNG_MAX || memcmp(png,signature,sizeof(signature)) ||
        big32(png+8)!=13 || memcmp(png+12,"IHDR",4) ||
        big32(png+16)!=TILE_SIZE || big32(png+20)!=TILE_SIZE ||
        png[26] || png[27] || png[28]) return false;
    unsigned depth=png[24], type=png[25];
    unsigned channels=type==0 || type==3 ? 1 : type==2 ? 3 : type==4 ? 2 : type==6 ? 4 : 0;
    if (!channels || !(depth==8 || ((type==0 || type==3) &&
        (depth==1 || depth==2 || depth==4)))) return false;
    uint16_t palette[256];
    unsigned colors=0;
    size_t compressed=0;
    bool ended=false;
    for (size_t pos=33; size-pos>=12;) {
        size_t length=big32(png+pos);
        const uint8_t *tag=png+pos+4, *data=tag+4;
        if (length>size-pos-12) return false;
        if (!memcmp(tag,"PLTE",4)) {
            if (compressed || length%3 || !length || length>768) return false;
            colors=length/3;
            for (unsigned i=0; i<colors; i++)
                palette[i]=dark_map(data[3*i],data[3*i+1],data[3*i+2]);
        } else if (!memcmp(tag,"IDAT",4)) {
            memmove(png+compressed,data,length);
            compressed+=length;
        } else if (!memcmp(tag,"IEND",4)) {
            ended=length==0;
            break;
        }
        pos+=12+length;
    }
    if (!ended || !compressed || (type==3 && !colors)) return false;
    unsigned bytes=(TILE_SIZE*depth*channels+7)/8, stride=bytes+1;
    unsigned bpp=(depth*channels+7)/8;
    size_t inflated=stride*TILE_SIZE;
#ifdef ESP_PLATFORM
    /* The inflater's Huffman tables are larger than the SD task's stack. */
    tinfl_decompressor *inflater=heap_caps_malloc(sizeof(*inflater),MALLOC_CAP_SPIRAM);
    if (!inflater) return false;
    tinfl_init(inflater);
    size_t consumed=compressed, decoded=inflated;
    tinfl_status rc=tinfl_decompress(inflater,png,&consumed,scan,scan,&decoded,
        TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(inflater);
    if (rc!=TINFL_STATUS_DONE || decoded!=inflated) return false;
#else
    uLongf decoded=inflated;
    if (uncompress(scan,&decoded,png,compressed)!=Z_OK || decoded!=inflated) return false;
#endif
    uint16_t grayscale[256];
    if (type==0 || type==4)
        for (unsigned i=0; i<(1U<<depth); i++) {
            int value=i*255/((1U<<depth)-1);
            grayscale[i]=dark_map(value,value,value);
        }
    for (unsigned y=0; y<TILE_SIZE; y++) {
        uint8_t *row=scan+y*stride+1;
        const uint8_t *up=y ? row-stride : NULL;
        unsigned filter=row[-1];
        if (filter>4) return false;
        for (unsigned x=0; filter && x<bytes; x++) {
            unsigned a=x>=bpp ? row[x-bpp] : 0, b=up ? up[x] : 0;
            unsigned c=up && x>=bpp ? up[x-bpp] : 0;
            unsigned predictor=filter==1 ? a : filter==2 ? b : filter==3 ? (a+b)/2 : paeth(a,b,c);
            row[x]+=predictor;
        }
        for (unsigned x=0; x<TILE_SIZE; x++) {
            if (type==2 || type==6) {
                const uint8_t *rgb=row+x*channels;
                *output++=dark_map(rgb[0],rgb[1],rgb[2]);
            } else {
                unsigned index=depth==8 ? row[x*channels] :
                    (row[x*depth/8]>>(8-depth-x*depth%8))&((1U<<depth)-1);
                if (type==3 && index>=colors) return false;
                *output++=type==3 ? palette[index] : grayscale[index];
            }
        }
    }
    return true;
}
