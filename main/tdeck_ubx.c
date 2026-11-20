#include "tdeck_ubx.h"

bool tdeck_ubx_feed(UbxReceiver *r, uint8_t byte) {
    if (r->used<2) {
        if (r->used==1 && byte==0x62) {
            r->used=2;
            r->ck_a=r->ck_b=0;
        } else r->used=byte==0xb5 ? 1 : 0;
        return false;
    }

    unsigned i=r->used++-2;
    r->data[i]=byte;
    if (i==3) {
        r->length=r->data[2]+256u*byte;
        if (r->length>sizeof(r->data)-6) {
            r->used=0;
            return false;
        }
    }
    if (i<4 || i<4+r->length) {
        r->ck_a+=byte;
        r->ck_b+=r->ck_a;
        return false;
    }
    if (i==4+r->length) return false;

    bool valid=r->data[i-1]==r->ck_a && byte==r->ck_b;
    r->used=!valid && byte==0xb5 ? 1 : 0;
    return valid;
}

bool tdeck_ubx_satellites(const UbxReceiver *r, GnssSatellites *satellites) {
    if (r->data[0]!=UBX_NAV || r->data[1]!=UBX_NAV_SAT || r->length<8) return false;
    const uint8_t *payload=r->data+4;
    if (payload[4]!=1 || r->length!=8+12u*payload[5]) return false;

    *satellites=(GnssSatellites){0};
    for (unsigned i=8; i<r->length; i+=12) {
        unsigned gnss=payload[i];
        if (gnss>=GNSS_COUNT || gnss==4) continue;
        unsigned flags=payload[i+8];
        if ((flags&7)>=2) satellites->visible[gnss]++;
        if (flags&8) satellites->used[gnss]++;
    }
    return true;
}
