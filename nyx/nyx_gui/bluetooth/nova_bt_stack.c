/* Cooperative Classic HID host. BTstack stays isolated from BDK headers.
 * No background discovery, key writes, pairing changes or SD key files. */
#include "nova_bt.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_event.h"
#include "btstack_chipset.h"
#include "hci.h"
#include "hci_cmd.h"
#include "l2cap.h"
#include "classic/hid_host.h"
#include <string.h>

static nova_bt_status status;
static nova_bt_peer peers[2];
static int initialized, failure;
static uint32_t started, tx_started, rx_started;
static uint32_t poll_started_us;
static uint8_t have_poll_started;
static uint8_t tx[1030], rx[1030];
static unsigned tx_len, tx_pos, rx_len, rx_need, rx_header;
static uint8_t rx_type;
static void (*receive_packet)(uint8_t,uint8_t *,uint16_t);
static btstack_packet_callback_registration_t event_registration;
static uint8_t descriptors[1024];
static struct {uint16_t cid,handle;uint8_t retries, sequence, mode_tries, profile, output[48],rf_disabled,stream_seen,nudged,recovering,simple;uint32_t next, mode_next,generation,opened,last_input;} link[2];
static struct {uint8_t active,side,kind;uint16_t opcode,handle;uint32_t generation,sent;} rf_pending;
static uint32_t rf_next,draw_last;
static uint8_t rf_slot,draw_seen,draw_busy;
#define PROFILE_BR 1
#define PROFILE_POLICY 2
#define PROFILE_ACTIVE 4
#define PROFILE_QOS 8
/* Public BTstack commands, queued only when its command buffer is available. */
static void profile_run(void)
{
    for(unsigned i=0;i<2;i++){
        if(!link[i].profile||link[i].handle==HCI_CON_HANDLE_INVALID||!hci_connection_for_handle(link[i].handle))continue;
        if(!hci_can_send_command_packet_now())return;
        uint8_t flag;int result;
        if(link[i].profile&PROFILE_BR){flag=PROFILE_BR;result=hci_send_cmd(&hci_change_connection_packet_type,link[i].handle,hci_usable_acl_packet_types());}
        else if(link[i].profile&PROFILE_POLICY){flag=PROFILE_POLICY;result=hci_send_cmd(&hci_write_link_policy_settings,link[i].handle,LM_LINK_POLICY_ENABLE_ROLE_SWITCH);}
        else if(link[i].profile&PROFILE_ACTIVE){flag=PROFILE_ACTIVE;result=gap_sniff_mode_exit(link[i].handle);}
        else{flag=PROFILE_QOS;result=gap_qos_set(link[i].handle,HCI_SERVICE_TYPE_GUARANTEED,8000,0,2500,0xffffffffu);}
        if(!result)link[i].profile&=~flag;
        return;
    }
}
static int side_for(const uint8_t *addr);
static int rf_opcode(uint16_t op)
{return op==HCI_OPCODE_HCI_READ_RSSI||op==HCI_OPCODE_HCI_READ_LINK_QUALITY||op==HCI_OPCODE_HCI_READ_TRANSMIT_POWER_LEVEL||op==HCI_OPCODE_HCI_READ_FAILED_CONTACT_COUNTER;}
static int side_handle(uint16_t handle)
{
    hci_connection_t *conn=hci_connection_for_handle(handle);
    if(conn)return side_for(conn->address);
    for(unsigned i=0;i<2;i++)if(link[i].handle==handle)return i;
    return -1;
}
static void trace_frame(int outgoing,uint8_t type,const uint8_t *p,unsigned size)
{
    if(type==HCI_COMMAND_DATA_PACKET&&outgoing&&size>=3){
        uint16_t op=little_endian_read_16(p,0);
        if(rf_opcode(op))return;
        nova_bt_trace_add(1,op,p[2],0);
        if(op==HCI_OPCODE_HCI_WRITE_PAGE_TIMEOUT&&size>=5)
            nova_bt_trace_add(20,op,little_endian_read_16(p,3),0);
        if(op==HCI_OPCODE_HCI_ACCEPT_CONNECTION_REQUEST&&size>=10){
            bd_addr_t addr;reverse_bd_addr(p+3,addr);int i=side_for(addr);
            if(i>=0)nova_bt_trace_add(14,i,op,p[9]);
        }
        return;
    }
    if(type==HCI_EVENT_PACKET&&!outgoing&&size>=2){
        if(p[0]==HCI_EVENT_COMMAND_COMPLETE&&size>=5&&rf_opcode(little_endian_read_16(p,3)))return;
        if(p[0]==HCI_EVENT_COMMAND_COMPLETE&&size>=6)nova_bt_trace_add(2,little_endian_read_16(p,3),p[5],0);
        else if(p[0]==HCI_EVENT_COMMAND_STATUS&&size>=6)nova_bt_trace_add(3,little_endian_read_16(p,4),p[2],0);
        else if(p[0]==HCI_EVENT_DISCONNECTION_COMPLETE&&(size!=6||(size>=6&&!p[5])))
            nova_bt_trace_add(19,p[0],size,p[1]);
        else if(p[0]==HCI_EVENT_CONNECTION_COMPLETE||p[0]==HCI_EVENT_DISCONNECTION_COMPLETE||
                p[0]==HCI_EVENT_AUTHENTICATION_COMPLETE||p[0]==HCI_EVENT_ENCRYPTION_CHANGE||
                p[0]==HCI_EVENT_ENCRYPTION_CHANGE_V2||p[0]==HCI_EVENT_HARDWARE_ERROR)
            nova_bt_trace_add(4,p[0],p[0]==HCI_EVENT_DISCONNECTION_COMPLETE&&size>=6?p[5]:size>=3?p[2]:0,size>=5?little_endian_read_16(p,3):0);
        return;
    }
    if(type!=HCI_ACL_DATA_PACKET||size<8)return;
    if((little_endian_read_16(p,0)&0x3000)==0x1000)return; /* continuation has no L2CAP header */
    uint16_t handle=little_endian_read_16(p,0)&0xfff;
    if(little_endian_read_16(p,6)==1&&size>=12){
        /* A single ACL packet can contain several complete signal commands.
         * Record decoded configuration fields only, never packet payloads. */
        unsigned end=8+little_endian_read_16(p,4);if(end>size)end=size;
        for(unsigned at=8;at+4<=end;){
            uint8_t code=p[at],id=p[at+1];unsigned n=little_endian_read_16(p,at+2),data=at+4;
            if(n>end-data)break;
            uint16_t value=n>=2?little_endian_read_16(p,data):0;
            if((code==3||code==5)&&n>=6)value=little_endian_read_16(p,data+4);
            nova_bt_trace_add(outgoing?5:6,handle,(code<<8)|id,value);
            if(code==4||code==5){
                unsigned options=code==4?4:6;uint16_t mtu=0;
                if(n>=options){
                    for(unsigned k=options;k+2<=n;){
                        unsigned len=p[data+k+1];if(len>n-k-2)break;
                        if((p[data+k]&0x7f)==1&&len==2)mtu=little_endian_read_16(p,data+k+2);
                        k+=2+len;
                    }
                    nova_bt_trace_add(17,little_endian_read_16(p,data),little_endian_read_16(p,data+2),mtu);
                }
            }
            if(outgoing&&code==2&&n>=4){int i=side_handle(handle);if(i>=0){status.pad[i].last_psm=value;status.pad[i].stage=value==0x11?4:value==0x13?5:2;}}
            at=data+n;
        }
    }else if(outgoing&&size>=10)nova_bt_trace_add(7,handle,p[9],size);
}

static void timer_set(btstack_timer_source_t *t,uint32_t ms){t->timeout=nova_bt_now()+ms+1;}
static void noop(void){}
static const btstack_run_loop_t run_loop={
    .init=btstack_run_loop_base_init,
    .add_data_source=btstack_run_loop_base_add_data_source,
    .remove_data_source=btstack_run_loop_base_remove_data_source,
    .enable_data_source_callbacks=btstack_run_loop_base_enable_data_source_callbacks,
    .disable_data_source_callbacks=btstack_run_loop_base_disable_data_source_callbacks,
    .set_timer=timer_set,.add_timer=btstack_run_loop_base_add_timer,
    .remove_timer=btstack_run_loop_base_remove_timer,.execute=noop,
    .dump_timer=btstack_run_loop_base_dump_timer,.get_time_ms=nova_bt_now,
    .poll_data_sources_from_irq=noop,.execute_on_main_thread=btstack_run_loop_base_add_callback,
    .trigger_exit=noop
};
static void transport_init(const void *config){(void)config;}
static int transport_open(void){return 0;}
static int transport_close(void){tx_len=rx_len=rx_need=0;return 0;}
static void transport_register(void (*cb)(uint8_t,uint8_t *,uint16_t)){receive_packet=cb;}
static int transport_ready(uint8_t type){(void)type;return tx_len==0;}
static int transport_send(uint8_t type,uint8_t *packet,int size)
{
    if(tx_len||size<0||(unsigned)size+1>sizeof(tx))return -1;
    tx[0]=type;memcpy(tx+1,packet,size);tx_len=size+1;tx_pos=0;tx_started=nova_bt_now();return 0;
}
static const hci_transport_t transport={.name="Nova UART-D H4",.init=transport_init,
    .open=transport_open,.close=transport_close,.register_packet_handler=transport_register,
    .can_send_packet_now=transport_ready,.send_packet=transport_send};

/* Broadcom Write_BD_ADDR: documented BTstack bcm chipset command. RAM only.
 * ROM startup is experimental: no proprietary firmware is redistributed. */
static void bcm_address(bd_addr_t addr,uint8_t *cmd)
{cmd[0]=1;cmd[1]=0xfc;cmd[2]=6;reverse_bd_addr(addr,cmd+3);}
static const btstack_chipset_t chipset={.name="CYW4356 ROM",.set_bd_addr_command=bcm_address};
static int side_for(const uint8_t *addr)
{for(unsigned i=0;i<2;i++)if(peers[i].valid&&!memcmp(addr,peers[i].mac,6))return i;return -1;}
static int known_connection(bd_addr_t addr,hci_link_type_t type)
{return type==HCI_LINK_TYPE_ACL && side_for(addr)>=0;}
static int key_get(bd_addr_t addr,link_key_t key,link_key_type_t *type)
{int i=side_for(addr);if(i<0)return 0;memcpy(key,peers[i].key,16);*type=UNAUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P192;return 1;}
static void key_put(bd_addr_t addr,link_key_t key,link_key_type_t type){(void)addr;(void)key;(void)type;}
static void key_delete(bd_addr_t addr){(void)addr;}
static void key_local(bd_addr_t addr){(void)addr;}
static int key_iter(btstack_link_key_iterator_t *it){(void)it;return 0;}
static int key_next(btstack_link_key_iterator_t *it,bd_addr_t addr,link_key_t key,link_key_type_t *type)
{(void)it;(void)addr;(void)key;(void)type;return 0;}
static void key_done(btstack_link_key_iterator_t *it){(void)it;}
static const btstack_link_key_db_t keys={noop,key_local,noop,key_get,key_put,key_delete,key_iter,key_next,key_done};

static void clear_pad(unsigned i){status.pad[i].connected=0;status.pad[i].buttons=status.pad[i].pending_buttons=0;status.pad[i].x=status.pad[i].y=2048;}
static int side_cid(uint16_t cid)
{for(unsigned i=0;i<2;i++)if(cid&&link[i].cid==cid)return i;return -1;}
/* Recover one silent HID stream without restarting the other Joy-Con or
 * touching the saved pairing. Times are per connection and wrap-safe. */
static void recover_stream(unsigned side,uint32_t now)
{
    nova_bt_pad *pad=&status.pad[side];
    if(!pad->hid_open||!pad->encrypted||link[side].recovering||link[side].simple)return;
    uint32_t idle=now-(link[side].stream_seen?link[side].last_input:link[side].opened);
    if(idle>=(link[side].stream_seen?4000u:8000u)){
        if(link[side].handle!=HCI_CON_HANDLE_INVALID&&!gap_disconnect(link[side].handle)){
            link[side].recovering=1;pad->recoveries++;clear_pad(side);
            nova_bt_trace_add(21,side,2,idle>65535?65535:idle);
        }
    }else if(link[side].stream_seen&&idle>=1000&&!link[side].nudged){
        /* Reuse the padded 0x03/0x30 report command and existing retry path. */
        link[side].nudged=1;link[side].mode_tries=0;link[side].mode_next=now?now:1;
        pad->mode_ack=0;pad->report_restarts++;
        nova_bt_trace_add(21,side,1,idle);
    }
}

static void rf_reply(const uint8_t *p,unsigned size)
{
    if(!rf_pending.active||size<6||little_endian_read_16(p,3)!=rf_pending.opcode)return;
    unsigned side=rf_pending.side,kind=rf_pending.kind;rf_pending.active=0;
    if(link[side].generation!=rf_pending.generation||link[side].handle!=rf_pending.handle||!status.pad[side].hid_open)return;
    nova_bt_rf *r=&status.pad[side].rf;r->error[kind]=p[5];
    if(p[5]){if(p[5]==1||p[5]==0x11)link[side].rf_disabled|=1u<<kind;return;}
    unsigned need=kind==NOVA_RF_FAILED_CONTACTS?10:9;
    if(size<need||little_endian_read_16(p,6)!=rf_pending.handle){r->error[kind]=0xff;status.rf_bad_replies++;return;}
    int32_t value=kind==NOVA_RF_FAILED_CONTACTS?little_endian_read_16(p,8):
        kind==NOVA_RF_QUALITY?p[8]:(int8_t)p[8];
    if((kind==NOVA_RF_TX_CURRENT||kind==NOVA_RF_TX_MAX)&&(value< -30||value>20)){r->error[kind]=0xfd;return;}
    if(!r->samples[kind])r->minimum[kind]=r->maximum[kind]=value;
    if(value<r->minimum[kind])r->minimum[kind]=value;
    if(value>r->maximum[kind])r->maximum[kind]=value;
    r->value[kind]=value;r->updated[kind]=nova_bt_now();r->samples[kind]++;
}
static void rf_run(void)
{
    uint32_t now=nova_bt_now();
    if(rf_pending.active){
        if((uint32_t)(now-rf_pending.sent)<=1500)return;
        unsigned i=rf_pending.side,k=rf_pending.kind;
        if(link[i].generation==rf_pending.generation){status.pad[i].rf.error[k]=0xfe;link[i].rf_disabled|=1u<<k;}
        rf_pending.active=0;
    }
    if((int32_t)(now-rf_next)<0||!hci_can_send_command_packet_now()||tx_len)return;
    for(unsigned n=0;n<10;n++){
        unsigned slot=rf_slot;rf_slot=(rf_slot+1)%10;unsigned i=slot&1,k=slot/2;
        const nova_bt_pad *p=&status.pad[i];
        if(!p->hid_open||!p->encrypted||!p->mode_ack||link[i].profile||(link[i].rf_disabled&(1u<<k))||
            link[i].handle==HCI_CON_HANDLE_INVALID||!hci_connection_for_handle(link[i].handle))continue;
        uint16_t op;int result;
        if(k==NOVA_RF_RSSI){op=HCI_OPCODE_HCI_READ_RSSI;result=hci_send_cmd(&hci_read_rssi,link[i].handle);}
        else if(k==NOVA_RF_QUALITY){op=HCI_OPCODE_HCI_READ_LINK_QUALITY;result=hci_send_cmd(&hci_read_link_quality,link[i].handle);}
        else if(k==NOVA_RF_FAILED_CONTACTS){op=HCI_OPCODE_HCI_READ_FAILED_CONTACT_COUNTER;result=hci_send_cmd(&hci_read_failed_contact_counter,link[i].handle);}
        else{op=HCI_OPCODE_HCI_READ_TRANSMIT_POWER_LEVEL;result=hci_send_cmd(&hci_read_transmit_power_level,link[i].handle,k==NOVA_RF_TX_MAX);}
        if(!result){rf_pending.active=1;rf_pending.side=i;rf_pending.kind=k;rf_pending.opcode=op;rf_pending.handle=link[i].handle;
            rf_pending.generation=link[i].generation;rf_pending.sent=now;status.rf_queries++;}
        break;
    }
    rf_next=now+500;
}
static void event(uint8_t type,uint16_t channel,uint8_t *p,uint16_t size)
{
    (void)channel;if(type!=HCI_EVENT_PACKET||size<2)return;
    bd_addr_t addr;int i;uint16_t cid;
    switch(hci_event_packet_get_type(p)){
    case HCI_EVENT_COMMAND_COMPLETE:
        rf_reply(p,size);
        if(size>=14 && little_endian_read_16(p,3)==HCI_OPCODE_HCI_READ_LOCAL_VERSION_INFORMATION && !p[5]){
            status.controller_hci_version=p[6];status.controller_revision=little_endian_read_16(p,7);
            status.controller_manufacturer=little_endian_read_16(p,10);status.controller_subversion=little_endian_read_16(p,12);
        }
        /* Verify the controller's readback, never log either address. */
        if(size>=12 && little_endian_read_16(p,3)==HCI_OPCODE_HCI_READ_BD_ADDR && !p[5]){
            reverse_bd_addr(p+6,addr);
            status.host_identity=memcmp(addr,peers[peers[0].valid?0:1].host,6)?2:1;
            nova_bt_trace_add(12,status.host_identity,0,0);
            if(status.host_identity==2)failure=9;
        }
        break;
    case HCI_EVENT_CONNECTION_COMPLETE:
        if(size<13)break;
        hci_event_connection_complete_get_bd_addr(p,addr);i=side_for(addr);if(i<0)break;
        status.pad[i].last_error=hci_event_connection_complete_get_status(p);
        if(status.pad[i].last_error)status.pad[i].stage=7;
        else{
            link[i].handle=hci_event_connection_complete_get_connection_handle(p);status.pad[i].stage=2;
            link[i].generation++;link[i].rf_disabled=0;
            link[i].profile=PROFILE_BR|PROFILE_QOS;
            status.pad[i].role=gap_get_role(link[i].handle);
            nova_bt_trace_add(13,i,link[i].handle,status.pad[i].role);
        }
        break;
    case HCI_EVENT_CONNECTION_PACKET_TYPE_CHANGED:
        if(size<7||p[2])break;
        i=side_handle(little_endian_read_16(p,3));if(i<0)break;
        status.pad[i].br_only=(little_endian_read_16(p,5)&0x3306)==0x3306;
        nova_bt_trace_add(15,link[i].handle,0x040f,little_endian_read_16(p,5));break;
    case HCI_EVENT_MODE_CHANGE:
        if(size<8||p[2])break;
        i=side_handle(little_endian_read_16(p,3));if(i<0)break;
        status.pad[i].radio_mode=p[5];status.pad[i].mode_seen=1;
        nova_bt_trace_add(16,link[i].handle,p[5],little_endian_read_16(p,6));
        if(p[5]==2)link[i].profile|=PROFILE_POLICY|PROFILE_ACTIVE;
        break;
    case HCI_EVENT_ROLE_CHANGE:
        if(size<10||hci_event_role_change_get_status(p))break;
        hci_event_role_change_get_bd_addr(p,addr);i=side_for(addr);if(i<0)break;
        status.pad[i].role=hci_event_role_change_get_role(p);
        nova_bt_trace_add(13,i,link[i].handle,status.pad[i].role);break;
    case HCI_EVENT_AUTHENTICATION_COMPLETE:
    case HCI_EVENT_ENCRYPTION_CHANGE:
    case HCI_EVENT_ENCRYPTION_CHANGE_V2:
        if(size<5)break;
        i=side_handle(little_endian_read_16(p,3));if(i<0)break;
        if(p[0]==HCI_EVENT_AUTHENTICATION_COMPLETE){status.pad[i].auth_seen=1;status.pad[i].auth_error=p[2];}
        else{
            if(size<6)break;
            status.pad[i].encryption_seen=1;status.pad[i].encryption_error=p[2];
            status.pad[i].encrypted=p[2]?0:p[5];
        }
        status.pad[i].last_error=p[2];status.pad[i].stage=p[2]?7:3;break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        if(size<6||p[2])break;
        i=side_handle(little_endian_read_16(p,3));if(i<0)break;
        status.pad[i].disconnect_reason=p[5];status.pad[i].encrypted=0;
        status.pad[i].full_gap_valid=0;
        link[i].profile=0;
        clear_pad(i);status.pad[i].hid_open=0;break;
    case BTSTACK_EVENT_STATE:
        if(btstack_event_state_get_state(p)==HCI_STATE_WORKING){
            if(status.host_identity==1)status.state=NOVA_BT_READY;
            else failure=9;
        }
        break;
    case HCI_EVENT_HARDWARE_ERROR: failure=8;break;
    case HCI_EVENT_PIN_CODE_REQUEST:
        hci_event_pin_code_request_get_bd_addr(p,addr);gap_pin_code_negative(addr);break;
    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        hci_event_user_confirmation_request_get_bd_addr(p,addr);gap_ssp_confirmation_negative(addr);break;
    case HCI_EVENT_USER_PASSKEY_REQUEST:
        hci_event_user_passkey_request_get_bd_addr(p,addr);gap_ssp_passkey_negative(addr);break;
    case HCI_EVENT_HID_META:
        if(size<3)break;
        switch(hci_event_hid_meta_get_subevent_code(p)){
        case HID_SUBEVENT_INCOMING_CONNECTION:
            hid_subevent_incoming_connection_get_address(p,addr);
            cid=hid_subevent_incoming_connection_get_hid_cid(p);i=side_for(addr);
            if(i>=0 && !hid_subevent_incoming_connection_get_status(p)){
                link[i].cid=cid;hid_host_accept_connection(cid,HID_PROTOCOL_MODE_REPORT);
            }else hid_host_decline_connection(cid);
            break;
        case HID_SUBEVENT_CONNECTION_OPENED:
            hid_subevent_connection_opened_get_bd_addr(p,addr);i=side_for(addr);if(i<0)break;
            if(hid_subevent_connection_opened_get_status(p)){
                status.pad[i].last_error=hid_subevent_connection_opened_get_status(p);status.pad[i].hid_open=0;
                status.pad[i].stage=7;nova_bt_trace_add(10,i,status.pad[i].last_error,status.pad[i].last_psm);
                link[i].cid=0;link[i].next=nova_bt_now()+(link[i].retries>=3?10000:1500);clear_pad(i);
            }else{
                status.pad[i].hid_open=1;status.pad[i].last_error=0;status.pad[i].mode_ack=0;
                status.pad[i].stage=5;nova_bt_trace_add(10,i,0,status.pad[i].last_psm);
                link[i].cid=hid_subevent_connection_opened_get_hid_cid(p);
                link[i].retries=0;
                link[i].opened=nova_bt_now();link[i].last_input=0;
                link[i].stream_seen=link[i].nudged=link[i].recovering=link[i].simple=0;
                status.pad[i].full_gap_valid=0;
                link[i].mode_tries=0;link[i].mode_next=nova_bt_now()+150;
            }break;
        case HID_SUBEVENT_REPORT:
            i=side_cid(hid_subevent_report_get_hid_cid(p));if(i<0)break;
            if(!status.pad[i].reports)nova_bt_trace_add(11,i,hid_subevent_report_get_report_len(p),0);
            if(nova_bt_decode(&status.pad[i],i,hid_subevent_report_get_report(p),hid_subevent_report_get_report_len(p),nova_bt_now())){
                if(status.pad[i].report_id==0x30){
                    link[i].last_input=nova_bt_now();link[i].stream_seen=1;link[i].nudged=link[i].simple=0;
                }else if(status.pad[i].report_id==0x3f)link[i].simple=1;
            }
            if(status.pad[i].connected)status.pad[i].stage=6;
            break;
        case HID_SUBEVENT_CONNECTION_CLOSED:
            i=side_cid(hid_subevent_connection_closed_get_hid_cid(p));if(i<0)break;
            clear_pad(i);status.pad[i].hid_open=0;
            status.pad[i].full_gap_valid=0;
            if(!status.pad[i].last_error)status.pad[i].stage=0;
            link[i].cid=0;link[i].next=nova_bt_now()+(link[i].recovering?1000:5000);
            link[i].recovering=0;break;
        default:break;
        }break;
    default:break;
    }
    if(status.state==NOVA_BT_READY)profile_run();
}
static void input_byte(uint8_t value)
{
    if(!rx_type){
        if(value==0||value==0x31||value==0x34)return;
        rx_type=value;rx_len=0;rx_started=nova_bt_now();
        switch(value){case 4:rx_header=2;break;case 2:rx_header=4;break;
        case 3:rx_header=3;break;case 7:rx_header=0;break;default:failure=5;return;}
        rx_need=rx_header?rx_header:63;return;
    }
    if(rx_len>=sizeof(rx)){failure=5;return;}
    rx[rx_len++]=value;
    if(rx_header&&rx_len==rx_header){
        unsigned payload=rx_type==4?rx[1]:rx_type==2?(rx[2]|(rx[3]<<8)):rx[2];
        rx_need=rx_header+payload;
        if(rx_need>sizeof(rx)){failure=5;return;}
    }
    if(rx_len==rx_need){
        uint8_t type=rx_type;unsigned len=rx_len;rx_type=0;rx_len=rx_need=0;
        if(type==2||type==4){status.rx_packets++;if(type==2)status.rx_acl++;trace_frame(0,type,rx,len);receive_packet(type,rx,len);}
    }
}
const nova_bt_status *nova_bt_get_status(void){return &status;}
uint32_t nova_bt_take_buttons(unsigned side)
{
    if(side>=2)return 0;
    nova_bt_pad *p=&status.pad[side];uint32_t now=nova_bt_now(),buttons=0;
    if(p->connected&&(uint32_t)(now-p->updated)<=500){
        buttons=p->buttons;
        uint32_t pending=p->pending_buttons;
        for(unsigned bit=0;pending;bit++,pending>>=1)
            if((pending&1)&&(uint32_t)(now-p->pressed_ms[bit])<=NOVA_BT_PRESS_LATCH_MS)
                buttons|=1u<<bit;
    }
    p->pending_buttons=0;
    return buttons;
}
void nova_bt_stop(void)
{
    if(initialized){
        /* Power is removed first; callbacks cannot outlive the stack pools. */
        nova_bt_radio_close();hid_host_deinit();l2cap_deinit();hci_close();btstack_memory_deinit();
        initialized=0;
    }
    memset(peers,0,sizeof(peers));memset(link,0,sizeof(link));
    memset(&rf_pending,0,sizeof(rf_pending));rf_next=rf_slot=draw_seen=draw_busy=0;
    tx_len=rx_len=rx_need=rx_type=0;receive_packet=0;
    have_poll_started=0;
    /* Release every live input but retain session evidence until next Start.
     * A second Stop/export must not turn an encryption failure into all zeros. */
    for(unsigned i=0;i<2;i++){clear_pad(i);status.pad[i].hid_open=0;status.pad[i].encrypted=0;}
    status.state=NOVA_BT_OFF;
}
int nova_bt_start(const nova_bt_peer p[2])
{
    nova_bt_stop();memset(&status,0,sizeof(status));nova_bt_trace_reset();
    for(unsigned i=0;i<2;i++){link[i].handle=HCI_CON_HANDLE_INVALID;status.pad[i].role=0xff;}
    if(!nova_bt_validate_peers(p)){status.state=NOVA_BT_ERROR;status.error=1;return 0;}
    if(!nova_bt_radio_open()){status.state=NOVA_BT_ERROR;status.error=2;return 0;}
    memcpy(peers,p,sizeof(peers));failure=0;started=nova_bt_now();
    rf_next=started;
    btstack_memory_init();btstack_run_loop_init(&run_loop);hci_init(&transport,0);initialized=1;
    hci_set_chipset(&chipset);hci_set_link_key_db(&keys);hci_set_bd_addr(peers[peers[0].valid?0:1].host);
    gap_set_security_level(LEVEL_2);gap_ssp_set_auto_accept(0);
    /* Default Classic page timeout: 5.12s, rather than BTstack's 15.36s.
     * Never let a sleeping first peer monopolize the outgoing attempts. */
    gap_set_page_timeout(0x2000);
    /* HWTest-RCM GUI host's ROM compatibility profile: no EDR packets;
     * initially accept the Joy-Con's sniff request and role changes. */
    hci_enable_acl_packet_types(ACL_PACKET_TYPES_BR);
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH|LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    /* HID host policy also used by Bluepad32: request central on reconnect.
     * This is a role preference, not a claim that the controller accepted it. */
    hci_set_master_slave_policy(HCI_ROLE_MASTER);
    gap_register_classic_connection_filter(known_connection);gap_discoverable_control(0);gap_connectable_control(1);
    l2cap_init();hid_host_init(descriptors,sizeof(descriptors));hid_host_register_packet_handler(event);
    event_registration.callback=event;hci_add_event_handler(&event_registration);
    status.state=NOVA_BT_STARTING;hci_power_control(HCI_POWER_ON);return 1;
}
static void poll_budget(uint32_t max_us,uint32_t quiet_us)
{
    if(!initialized)return;
    /* HWTest measured >300us restart latency after UART flow control.
     * The foreground drains bursts with a quiet period; drawing uses a short
     * budget without waiting for a UART restart. An empty line returns now. */
    uint32_t t0=nova_bt_now_us(),last=t0;unsigned bytes=0;int rx_yielded=0;status.rx_polls++;
    if(have_poll_started){uint32_t gap=t0-poll_started_us;if(gap>status.poll_gap_max_us)status.poll_gap_max_us=gap;}
    poll_started_us=t0;have_poll_started=1;
    for(unsigned n=0;n<2048&&!failure;){
        int value=nova_bt_radio_read();
        if(value==-2){failure=4;break;}
        if(value>=0){input_byte(value);bytes++;n++;if(n==2048)rx_yielded=1;}
        /* Include packet callbacks in the same clock sample, without adding
         * a timer read per byte. A callback cannot be preempted mid-packet. */
        uint32_t us=nova_bt_now_us();
        if(value>=0)last=us;
        else if(!bytes||(uint32_t)(us-last)>=quiet_us)break;
        if((uint32_t)(us-t0)>=max_us){status.rx_budget_hits++;rx_yielded=1;break;}
    }
    status.rx_bytes+=bytes;uint32_t elapsed=nova_bt_now_us()-t0;
    if(elapsed>status.rx_max_us)status.rx_max_us=elapsed;
    if(!failure&&tx_len){
        for(unsigned n=0;n<32&&tx_pos<tx_len;n++)if(nova_bt_radio_write(tx[tx_pos]))tx_pos++;else break;
        if(tx_pos==tx_len){
            trace_frame(1,tx[0],tx+1,tx_len-1);if(tx[0]==HCI_ACL_DATA_PACKET)status.tx_acl++;
            tx_len=0;status.tx_packets++;uint8_t sent[]={HCI_EVENT_TRANSPORT_PACKET_SENT,0};
            receive_packet(HCI_EVENT_PACKET,sent,sizeof(sent));
        }else if((uint32_t)(nova_bt_now()-tx_started)>2000)failure=6;
    }
    if(!failure){
        btstack_run_loop_base_execute_callbacks();btstack_run_loop_base_poll_data_sources();
        btstack_run_loop_base_process_timers(nova_bt_now());
    }
    if(!failure&&status.state==NOVA_BT_READY){profile_run();rf_run();}
    uint32_t now=nova_bt_now();
    if(rx_type&&(uint32_t)(now-rx_started)>2000)failure=5;
    if(status.state==NOVA_BT_STARTING&&(uint32_t)(now-started)>15000)failure=3;
    int pending=0;
    for(unsigned i=0;i<2;i++)if(link[i].cid&&!status.pad[i].hid_open)pending=1;
    for(unsigned i=0;i<2&&!failure&&status.state==NOVA_BT_READY;i++){
        if(status.pad[i].connected&&(uint32_t)(now-status.pad[i].updated)>500)clear_pad(i);
        /* Do not mistake queued UART data or a partial H4 frame for silence. */
        if(!rx_yielded&&!rx_type)recover_stream(i,now);
        if(!pending&&peers[i].valid&&!link[i].cid&&(int32_t)(now-link[i].next)>=0){
            if(link[i].retries<3)link[i].retries++;
            link[i].next=now+(link[i].retries>=3?10000:5000);
            status.pad[i].stage=1;
            uint8_t result=hid_host_connect(peers[i].mac,HID_PROTOCOL_MODE_REPORT,&link[i].cid);
            if(result){link[i].cid=0;status.pad[i].last_error=result;status.pad[i].stage=7;nova_bt_trace_add(10,i,result,0);}
            else pending=1;
        }
        if(!link[i].recovering&&link[i].cid&&link[i].mode_next&&!status.pad[i].mode_ack&&(int32_t)(now-link[i].mode_next)>=0&&link[i].mode_tries<3){
            /* Keep the output storage alive until BTstack sends it. No rumble. */
            static const uint8_t neutral[]={0,1,0x40,0x40,0,1,0x40,0x40};
            if(!link[i].mode_tries){
                link[i].output[0]=link[i].sequence&15;memcpy(link[i].output+1,neutral,8);
                link[i].output[9]=3;link[i].output[10]=0x30;
            }
            if(!hid_host_send_report(link[i].cid,1,link[i].output,sizeof(link[i].output)))link[i].mode_tries++;
            link[i].mode_next=now+1000;
        }
    }
    if(failure){int code=failure;nova_bt_stop();status.state=NOVA_BT_ERROR;status.error=code;}
    /* One extra clock read per poll covers TX, callbacks, timers and teardown. */
    uint32_t total=nova_bt_now_us()-t0;
    if(total>status.poll_max_us)status.poll_max_us=total;
    uint32_t *maximum=quiet_us?&status.poll_normal_max_us:&status.poll_fast_max_us;
    if(total>*maximum)*maximum=total;
}
/* Drain long bursts once in the foreground, not for every display rectangle. */
void nova_bt_poll(void){poll_budget(8000,2000);}
void nova_bt_poll_fast(void){poll_budget(250,0);}
void nova_bt_draw_poll(void)
{
    if(!initialized||draw_busy)return;
    uint32_t now=nova_bt_now_us();
    if(draw_seen&&(uint32_t)(now-draw_last)<2000)return;
    draw_seen=1;draw_last=now;draw_busy=1;status.draw_yields++;
    poll_budget(250,0);draw_busy=0;
}
