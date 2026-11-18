#include "nova_bt.h"
/* Bounded session trace. Labels are decoded by the SD report writer. */
#define TRACE_COUNT 128
static nova_bt_trace records[TRACE_COUNT];
static unsigned count,next;
static uint32_t epoch;
void nova_bt_trace_reset(void){count=next=0;epoch=nova_bt_now();}
void nova_bt_trace_add(uint16_t kind,uint16_t a,uint16_t b,uint16_t c)
{
    records[next]=(nova_bt_trace){nova_bt_now()-epoch,kind,a,b,c};
    next=(next+1)%TRACE_COUNT;if(count<TRACE_COUNT)count++;
}
unsigned nova_bt_trace_count(void){return count;}
int nova_bt_trace_get(unsigned index,nova_bt_trace *record)
{
    if(!record||index>=count)return 0;
    *record=records[(next+TRACE_COUNT-count+index)%TRACE_COUNT];return 1;
}
