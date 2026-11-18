#include "nova_bt.h"
#include <string.h>
static int nonzero(const uint8_t *p, unsigned n)
{ unsigned v=0; while(n--) v |= *p++; return v!=0; }
static void observe(nova_bt_pad *p,uint32_t now)
{
    if(!p->reports)p->first_report_ms=now;
    p->buttons_seen|=p->buttons;p->reports++;
    if(p->report_id==0x30){
        if(p->full_gap_valid){uint32_t gap=now-p->last_full_ms;
            if(gap>p->max_full_gap_ms)p->max_full_gap_ms=gap;
            if(gap>50)p->full_gaps_50ms++;
            if(gap>250)p->full_gaps_250ms++;
        }
        p->last_full_ms=now;p->full_gap_valid=1;p->full_reports++;
    }
}
static void buttons_update(nova_bt_pad *p,uint32_t buttons,uint32_t now)
{
    uint32_t rising=buttons&~p->buttons;
    p->pending_buttons|=rising;
    for(unsigned bit=0;rising;bit++,rising>>=1)
        if(rising&1)p->pressed_ms[bit]=now;
    p->buttons=buttons;
}
int nova_bt_validate_peers(const nova_bt_peer p[2])
{
    int count=0;
    for(unsigned i=0;i<2;i++) if(p[i].valid) {
        if(!nonzero(p[i].mac,6)||!nonzero(p[i].host,6)||!nonzero(p[i].key,16)) return 0;
        if((p[i].mac[0]&1)||(p[i].host[0]&1)) return 0;
        count++;
    }
    if(count==2 && (memcmp(p[0].host,p[1].host,6)||!memcmp(p[0].mac,p[1].mac,6))) return 0;
    return count;
}
static uint32_t pairing_crc(const uint8_t *p,unsigned n)
{
    uint32_t crc=0xffffffffu;
    while(n--){crc^=*p++;for(unsigned b=0;b<8;b++)crc=(crc>>1)^(0xedb88320u & (0u-(crc&1)));}
    return ~crc;
}
int nova_bt_pairing_encode(uint8_t out[NOVA_BT_PAIRING_SIZE],const nova_bt_peer p[2])
{
    memset(out,0,NOVA_BT_PAIRING_SIZE);int count=nova_bt_validate_peers(p);if(!count)return 0;
    memcpy(out,"NJOYPAIR",8);out[8]=1;
    for(unsigned i=0;i<2;i++)if(p[i].valid){
        out[9]|=1u<<i;uint8_t *at=out+12+i*28;
        memcpy(at,p[i].mac,6);memcpy(at+6,p[i].host,6);memcpy(at+12,p[i].key,16);
    }
    uint32_t crc=pairing_crc(out,68);for(unsigned i=0;i<4;i++)out[68+i]=crc>>(i*8);
    return count;
}
int nova_bt_pairing_decode(nova_bt_peer p[2],const uint8_t *in,unsigned size)
{
    memset(p,0,2*sizeof(*p));
    if(size!=NOVA_BT_PAIRING_SIZE||memcmp(in,"NJOYPAIR",8)||in[8]!=1||!in[9]||(in[9]&~3)||in[10]||in[11])return 0;
    uint32_t crc=0;for(unsigned i=0;i<4;i++)crc|=(uint32_t)in[68+i]<<(i*8);
    if(crc!=pairing_crc(in,68))return 0;
    for(unsigned i=0;i<2;i++){
        const uint8_t *at=in+12+i*28;
        if(in[9]&(1u<<i)){p[i].valid=1;memcpy(p[i].mac,at,6);memcpy(p[i].host,at+6,6);memcpy(p[i].key,at+12,16);}
        else if(nonzero(at,28)){memset(p,0,2*sizeof(*p));return 0;}
    }
    int count=nova_bt_validate_peers(p);if(!count)memset(p,0,2*sizeof(*p));return count;
}
int nova_bt_decode(nova_bt_pad *p, unsigned side, const uint8_t *r, unsigned n, uint32_t now)
{
    /* BTstack delivers HID DATA header 0xa1 followed by the report ID. */
    if(n && r[0]==0xa1){r++;n--;}
    if(side>1||!n) return 0;
    if(r[0]==0x3f){
        if(n<12||r[3]>8)return 0;
        /* Simple Joy-Con reports, held vertically as a pair. The hat and
         * face-button orientations differ between left and right halves.
         * Protocol reference: SDL's combined simple Joy-Con handlers. */
        static const uint32_t face[2][6]={{1u<<19,1u<<16,1u<<17,1u<<18,1u<<21,1u<<20},
            {1u<<3,1u<<1,1u<<2,1u<<0,1u<<4,1u<<5}};
        uint32_t buttons=0;
        for(unsigned bit=0;bit<6;bit++)if(r[1]&(1u<<bit))buttons|=face[side][bit];
        if(!side){if(r[2]&1)buttons|=1u<<8;if(r[2]&4)buttons|=1u<<11;if(r[2]&32)buttons|=1u<<13;}
        else{if(r[2]&2)buttons|=1u<<9;if(r[2]&8)buttons|=1u<<10;if(r[2]&16)buttons|=1u<<12;}
        if(r[2]&64)buttons|=1u<<(side?6:22);
        if(r[2]&128)buttons|=1u<<(side?7:23);
        static const int8_t x[9]={1,1,0,-1,-1,-1,0,1,0};
        static const int8_t y[9]={0,-1,-1,-1,0,1,1,1,0};
        int sign=side?-1:1;
        p->x=2048+sign*x[r[3]]*1024;p->y=2048+sign*y[r[3]]*1024;
        buttons_update(p,buttons,now);p->updated=now;p->connected=1;p->report_id=0x3f;observe(p,now);
        return 1;
    }
    if(n<13||(r[0]!=0x30&&r[0]!=0x21)) return 0;
    uint32_t buttons=r[3]|((uint32_t)r[4]<<8)|((uint32_t)r[5]<<16);
    buttons_update(p,buttons & (side?0x0016ffu:0xff2900u),now);
    unsigned at=side?9:6;
    p->x=r[at]|((r[at+1]&15)<<8);p->y=(r[at+1]>>4)|(r[at+2]<<4);
    p->battery=(r[2]>>4)&0xf;p->updated=now;p->connected=1;p->report_id=r[0];observe(p,now);
    if(r[0]==0x30||(r[0]==0x21&&n>=16&&(r[13]&0x80)&&r[14]==3&&r[15]==0x30))p->mode_ack=1;
    return 1;
}
