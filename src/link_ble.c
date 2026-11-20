#include <string.h>
#include "HAL.h"
#include "link_ble.h"
#include "link_config.h"
#include "link_ble_programmer.h"
#include "link_session.h"

#define FRAME_MAX 20u
#define MESSAGE_MAX 274u
#define START_EVENT 1u
/* Bluetooth UUIDs use little endian on air. */
static const uint8_t service_uuid[16] = {0x00,0x00,0x21,0x59,0x3c,0x2b,0x54,0x9e,0x6b,0x4b,0x7d,0x6e,0x01,0x00,0x2a,0x7e};
static const uint8_t rx_uuid[16] = {0x00,0x00,0x21,0x59,0x3c,0x2b,0x54,0x9e,0x6b,0x4b,0x7d,0x6e,0x02,0x00,0x2a,0x7e};
static const uint8_t tx_uuid[16] = {0x00,0x00,0x21,0x59,0x3c,0x2b,0x54,0x9e,0x6b,0x4b,0x7d,0x6e,0x03,0x00,0x2a,0x7e};
static gattAttrType_t service = {ATT_UUID_SIZE, (uint8_t *)service_uuid};
static uint8_t rx_props = GATT_PROP_WRITE | GATT_PROP_WRITE_NO_RSP, tx_props = GATT_PROP_NOTIFY;
static uint8_t rx_value, tx_value;
static gattCharCfg_t cccd[GATT_MAX_NUM_CONN];
static gattAttribute_t attrs[] = {
    {{ATT_BT_UUID_SIZE, primaryServiceUUID}, GATT_PERMIT_READ, 0, (uint8_t *)&service},
    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &tx_props},
    {{ATT_UUID_SIZE, tx_uuid}, 0, 0, &tx_value},
    {{ATT_BT_UUID_SIZE, clientCharCfgUUID}, GATT_PERMIT_READ | GATT_PERMIT_WRITE, 0, (uint8_t *)cccd},
    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &rx_props},
    {{ATT_UUID_SIZE, rx_uuid}, GATT_PERMIT_WRITE, 0, &rx_value}
};
static uint8_t task_id;
static uint16_t conn = INVALID_CONNHANDLE;
uint8_t link_ble_rx_frame_limit(void)
{
#if LINK_S9_FAST_CLOCK
    uint16_t mtu = conn == INVALID_CONNHANDLE ? 23u : ATT_GetMTU(conn);
    const uint8_t limit = LINK_BLE_TWO_WRITES ? LINK_BLE_WIDE_FRAME : 64u;
    if (mtu > 23u) return mtu >= limit + 3u ? limit : (uint8_t)(mtu - 3u);
#endif
    return FRAME_MAX;
}
/* Single-command transport: request_ready/executing/tx_len reject new RX.
 * programmer_start() copies WRITE data before RX ownership ends; a response
 * is assembled only after the request has been consumed. Notifications copy
 * each fragment into GATT-owned storage before the buffer can be reused. */
static union {
    uint8_t rx[MESSAGE_MAX];
    uint8_t tx[MESSAGE_MAX];
} transport_buffer;
static uint16_t rx_len, rx_total, rx_seq, last_seq, tx_len, tx_offset;
static uint8_t request_ready, executing, invalid_transport, terminating;
enum { CLOSE_NONE, CLOSE_WAIT_ACK, CLOSE_WAIT_CLEANUP, CLOSE_TERMINATING };
static uint8_t close_phase;
static uint32_t close_due, close_deadline;
static uint32_t last_activity, notify_retry_due;
/* Frozen first rejection plus live receive counters, accessible via USB 0x85. */
static uint8_t rx_error[32], rx_last_frame[8], rx_error_seen;
static uint16_t rx_request_count, rx_command_count;
static uint8_t restart_advertising, advertising, last_hci_reason, last_local_cause;
static uint8_t last_adv_status, last_terminate_status, last_role_state;
static uint8_t advertising_phase, live_role_state, live_adv_enabled, live_role_status, live_adv_query_status;
static uint32_t advertising_deadline, main_poll_count;
static uint16_t conn_interval, conn_latency, conn_timeout;
static uint32_t advertising_due, terminate_due, disconnect_count, last_disconnect_ms, advertising_attempts;


static uint16_t get16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p+2) << 16); }
static void put16(uint8_t *p,uint16_t v) { p[0]=v; p[1]=v>>8; }
static void put32(uint8_t *p,uint32_t v) { put16(p,v); put16(p+2,v>>16); }
static uint32_t crc32(const uint8_t *p,uint16_t n)
{
    uint32_t c=0xffffffffu;
    uint8_t b;
    while(n--) { c ^= *p++; for(b=0;b<8;b++) c=(c>>1)^((c&1)?0xedb88320u:0); }
    return c^0xffffffffu;
}
uint32_t link_ble_now_ms(void)
{
    static uint32_t previous, ms;
    static uint8_t fraction;
    uint32_t now=TMOS_GetSystemClock(), delta=now-previous;
    uint64_t units=(uint64_t)delta*5u+fraction;
    previous=now;
    ms+=(uint32_t)(units/8u);
    fraction=units%8u;
    return ms;
}
static void reset_transport(void)
{
    rx_len=rx_total=rx_seq=last_seq=tx_len=tx_offset=0;
    request_ready=executing=0;
    /* Connection/termination flags belong to GAP lifecycle, not CCCD frames. */
    link_ble_programmer_disconnect();
}
static bStatus_t read_attr(uint16_t handle,gattAttribute_t *a,uint8_t *p,
                          uint16_t *n,uint16_t off,uint16_t max,uint8_t method)
{
    uint16_t value;
    (void)method;
    *n=0;
    if(a!=&attrs[3]) return ATT_ERR_READ_NOT_PERMITTED;
    if(off) return ATT_ERR_INVALID_OFFSET;
    if(max<2) return ATT_ERR_INVALID_VALUE_SIZE;
    value=GATTServApp_ReadCharCfg(handle,cccd);
    put16(p,value); *n=2;
    return SUCCESS;
}
static uint8_t transport_flags(void)
{ return request_ready | (executing<<1) | (invalid_transport<<2) | (terminating<<3); }
static bStatus_t reject_rx(uint8_t method,bStatus_t status,uint8_t reason,
                          const uint8_t *frame,uint16_t n,uint16_t off,uint8_t fatal)
{
    if(!rx_error_seen) {
        memset(rx_error,0,sizeof(rx_error));
        rx_error[0]=1; rx_error[1]=reason; rx_error[2]=method; rx_error[3]=status;
        rx_error[4]=transport_flags();
        rx_error[5]=rx_len?transport_buffer.rx[0]:(n>=7 && get16(frame+2)==0?frame[6]:0);
        if(n>=6) memcpy(rx_error+6,frame,6);
        put16(rx_error+12,n); put16(rx_error+14,rx_len); put16(rx_error+16,rx_total);
        put16(rx_error+18,rx_seq); put16(rx_error+20,last_seq);
        put16(rx_error+22,tx_len); put16(rx_error+24,tx_offset); put16(rx_error+26,off);
        put32(rx_error+28,link_ble_now_ms()); rx_error_seen=1;
    }
    if(fatal || method==ATT_WRITE_CMD) invalid_transport=1;
    return status;
}
uint8_t link_ble_usb_rx_diag(uint8_t *out,uint8_t capacity)
{
    if(!out || capacity<64u) return 0;
    memset(out,0,64); out[0]=0x85; out[1]=1; out[2]=64;
    memcpy(out+4,rx_error,32);
    put16(out+36,conn); put16(out+38,rx_len); put16(out+40,rx_total);
    put16(out+42,rx_seq); put16(out+44,last_seq); put16(out+46,tx_len); put16(out+48,tx_offset);
    put16(out+50,rx_request_count); put16(out+52,rx_command_count);
    out[54]=transport_flags(); out[55]=close_phase; memcpy(out+56,rx_last_frame,8);
    return 64;
}
static bStatus_t write_attr(uint16_t handle,gattAttribute_t *a,uint8_t *p,
                           uint16_t n,uint16_t off,uint8_t method)
{
    uint16_t seq, pos, total, count;
    bStatus_t result;
    if(a==&attrs[3]) {
        if(method!=ATT_WRITE_REQ) return ATT_ERR_UNSUPPORTED_REQ;
        result=GATTServApp_ProcessCCCWriteReq(handle,a,p,n,off,GATT_CLIENT_CFG_NOTIFY);
        if(result==SUCCESS && !GATTServApp_ReadCharCfg(handle,cccd)) {
            if(close_phase || terminating) {
                rx_len=rx_total=tx_len=tx_offset=0;
                request_ready=0;
            } else reset_transport();
        }
        return result;
    }
    if(a!=&attrs[5]) return ATT_ERR_WRITE_NOT_PERMITTED;
    if(method==ATT_WRITE_REQ) rx_request_count++;
    if(method==ATT_WRITE_CMD) rx_command_count++;
    rx_last_frame[0]=method; rx_last_frame[1]=(uint8_t)n;
    memset(rx_last_frame+2,0,6); if(n>=6) memcpy(rx_last_frame+2,p,6);
    if(method!=ATT_WRITE_REQ && method!=ATT_WRITE_CMD)
        return reject_rx(method,ATT_ERR_UNSUPPORTED_REQ,1,p,n,off,0);
    if(off) return reject_rx(method,ATT_ERR_INVALID_OFFSET,2,p,n,off,0);
    if(conn!=handle || invalid_transport || terminating || close_phase)
        return reject_rx(method,ATT_ERR_UNLIKELY,3,p,n,off,0);
    if(!GATTServApp_ReadCharCfg(handle,cccd))
        return reject_rx(method,ATT_ERR_INSUFFICIENT_RESOURCES,4,p,n,off,0);
    if(n<7 || (n>FRAME_MAX && n>link_ble_rx_frame_limit())) return reject_rx(method,ATT_ERR_INVALID_VALUE_SIZE,5,p,n,off,0);
    if(request_ready || executing || tx_len)
        return reject_rx(method,ATT_ERR_INSUFFICIENT_RESOURCES,6,p,n,off,0);
    seq=get16(p); pos=get16(p+2); total=get16(p+4); count=n-6;
    if(!seq) return reject_rx(method,ATT_ERR_INVALID_VALUE,7,p,n,off,1);
    if(seq==last_seq) return reject_rx(method,ATT_ERR_INVALID_VALUE,8,p,n,off,1);
    if(total<5 || total>MESSAGE_MAX) return reject_rx(method,ATT_ERR_INVALID_VALUE,9,p,n,off,1);
    if(pos+count>total) return reject_rx(method,ATT_ERR_INVALID_VALUE,10,p,n,off,1);
    if(rx_len && (seq!=rx_seq || total!=rx_total))
        return reject_rx(method,ATT_ERR_INVALID_VALUE,11,p,n,off,1);
    if(pos!=rx_len) return reject_rx(method,ATT_ERR_INVALID_VALUE,12,p,n,off,1);
    rx_seq=seq; rx_total=total;
    memcpy(transport_buffer.rx+pos,p+6,count); rx_len+=count;
    last_activity=link_ble_now_ms();
    if(rx_len==rx_total) {
        if(crc32(transport_buffer.rx,rx_len-4)!=get32(transport_buffer.rx+rx_len-4)) {
            return reject_rx(method,ATT_ERR_INVALID_VALUE,13,p,n,off,1);
        }
        request_ready=1;
        last_seq=seq;
    }
    return SUCCESS;
}
static gattServiceCBs_t service_cbs={read_attr,write_attr,NULL};
static void state_changed(gapRole_States_t state,gapRoleEvent_t *event)
{
    last_role_state=(uint8_t)(state & GAPROLE_STATE_ADV_MASK);
    if(!event) return;
    if(event->gap.opcode==GAP_LINK_ESTABLISHED_EVENT && event->gap.hdr.status==SUCCESS) {
        reset_transport();
        invalid_transport=terminating=close_phase=0;
        rx_error_seen=0; rx_request_count=rx_command_count=0;
        memset(rx_last_frame,0,sizeof(rx_last_frame));
        conn=event->linkCmpl.connectionHandle;
        conn_interval=event->linkCmpl.connInterval;
        conn_latency=event->linkCmpl.connLatency;
        conn_timeout=event->linkCmpl.connTimeout;
        advertising=restart_advertising=advertising_phase=0;
        GATTServApp_InitCharCfg(conn,cccd);
        last_activity=link_ble_now_ms();
    } else if(event->gap.opcode==GAP_LINK_TERMINATED_EVENT) {
        if(conn==INVALID_CONNHANDLE || event->linkTerminate.connectionHandle!=conn) return;
        if(!terminating) last_local_cause=0;
        last_hci_reason=event->linkTerminate.reason;
        disconnect_count++;
        last_disconnect_ms=link_ble_now_ms();
        conn=INVALID_CONNHANDLE;
        conn_interval=conn_latency=conn_timeout=0;
        GATTServApp_InitCharCfg(INVALID_CONNHANDLE,cccd);
        reset_transport();
        invalid_transport=terminating=close_phase=0;
        /* Match the official Peripheral_LinkTerminated flow. Defer the GAP
         * call to main loop and retry only if the stack is temporarily busy. */
        advertising=last_role_state==GAPROLE_ADVERTISING;
        restart_advertising=1;
        advertising_phase=0;
        advertising_due=last_disconnect_ms+100u;
    } else if(event->gap.opcode==GAP_MAKE_DISCOVERABLE_DONE_EVENT) {
        last_adv_status=event->gap.hdr.status;
        advertising=event->gap.hdr.status==SUCCESS && last_role_state==GAPROLE_ADVERTISING;
        if(conn==INVALID_CONNHANDLE && advertising) restart_advertising=advertising_phase=0;
    }
    /* Do not restart on END_DISCOVERABLE: the stack emits it as part of a
     * normal connection. Only a confirmed link termination starts recovery. */

}
static void connection_status_changed(uint16_t handle,uint8_t change)
{
    if(handle!=LOOPBACK_CONNHANDLE &&
       (change==LINKDB_STATUS_UPDATE_REMOVED ||
        (change==LINKDB_STATUS_UPDATE_STATEFLAGS && !linkDB_Up(handle)))) {
        /* Same per-connection CCCD cleanup as official GATT Profile. GAP owns
         * the link handle and termination bookkeeping; do not fake that here. */
        GATTServApp_InitCharCfg(handle,cccd);
    }
}
static void begin_termination(uint8_t cause,uint32_t now)
{
    last_local_cause=cause;
    reset_transport();
    invalid_transport=terminating=1;
    terminate_due=now;
    last_terminate_status=0xff;
}
/* LINK_STATUS=10, pure RAM. Available again after a reconnect, preserving the
 * previous HCI disconnect reason and local protocol termination cause. */
static uint16_t build_link_status(uint8_t *out)
{
    uint8_t *p=out+6;
    memset(out,0,38);
    out[0]=0x8a;
    p[0]=1;
    p[1]=(conn!=INVALID_CONNHANDLE?1:0) | (advertising?2:0) |
         (restart_advertising?4:0) | (terminating?8:0) | (close_phase?16:0) |
         ((conn!=INVALID_CONNHANDLE && GATTServApp_ReadCharCfg(conn,cccd))?32:0);
    p[2]=last_hci_reason; p[3]=last_local_cause;
    put32(p+4,disconnect_count); put32(p+8,last_disconnect_ms);
    p[12]=last_adv_status; p[13]=last_terminate_status; p[14]=last_role_state; p[15]=advertising_phase;
    put32(p+16,advertising_attempts);
    put32(p+20,link_ble_now_ms()-last_activity);
    put16(p+24,conn); put16(p+26,conn_interval);
    put16(p+28,conn_latency); put16(p+30,conn_timeout);
    return 38;
}
/* Read-only USB debug route, independent of BLE discovery. Main-loop only.
 * 84 01, nested LINK_STATUS response (38B), live role/enable/query statuses,
 * recovery phase/flag (2B), reserved (2B), main-loop counter (4B). */
uint8_t link_ble_usb_diag(uint8_t *out,uint8_t capacity)
{
    uint32_t role=0;
    uint8_t enabled=0;
    if(!out || capacity<52u) return 0;
    out[0]=0x84; out[1]=1;
    build_link_status(out+2);
    live_role_status=GAPRole_GetParameter(GAPROLE_STATE,&role);
    live_adv_query_status=GAPRole_GetParameter(GAPROLE_ADVERT_ENABLED,&enabled);
    live_role_state=(uint8_t)(role & GAPROLE_STATE_ADV_MASK);
    live_adv_enabled=enabled;
    out[40]=live_role_state; out[41]=live_adv_enabled;
    out[42]=live_role_status; out[43]=live_adv_query_status;
    out[44]=advertising_phase; out[45]=restart_advertising;
    out[46]=conn!=INVALID_CONNHANDLE?linkDB_Up(conn):0; out[47]=close_phase;
    put32(out+48,main_poll_count);
    return 52;
}
static void parameters_changed(uint16_t handle,uint16_t interval,uint16_t latency,uint16_t timeout)
{
    if(handle==conn) { conn_interval=interval; conn_latency=latency; conn_timeout=timeout; }
}
static gapRolesCBs_t role_cbs={state_changed,NULL,parameters_changed};
static gapBondCBs_t bond_cbs={NULL,NULL,NULL};
static uint16_t process_event(uint8_t id,uint16_t events)
{
    if(events&SYS_EVENT_MSG) {
        uint8_t *message=tmos_msg_receive(id);
        if(message) tmos_msg_deallocate(message);
        return events^SYS_EVENT_MSG;
    }
    if(events&START_EVENT) {
        bStatus_t s=GAPRole_PeripheralStartDevice(task_id,&bond_cbs,&role_cbs);
        PRINT("BLE start: %u\r\n",s);
        return events^START_EVENT;
    }
    return 0;
}
void link_ble_init(void)
{
    uint8_t enable=TRUE, pair=GAPBOND_PAIRING_MODE_NO_PAIRING;
    uint16_t min=24,max=40,adv=160;
    uint8_t adv_data[]={2,GAP_ADTYPE_FLAGS,GAP_ADTYPE_FLAGS_GENERAL|GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED,
        17,GAP_ADTYPE_128BIT_COMPLETE,0x00,0x00,0x21,0x59,0x3c,0x2b,0x54,0x9e,0x6b,0x4b,0x7d,0x6e,0x01,0x00,0x2a,0x7e};
    uint8_t scan_data[]={11,GAP_ADTYPE_LOCAL_NAME_COMPLETE,'C','H','5','9','2','-','L','I','N','K'};
    bStatus_t status;
    task_id=TMOS_ProcessEventRegister(process_event);
    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED,1,&enable);
    GAPRole_SetParameter(GAPROLE_ADVERT_DATA,sizeof(adv_data),adv_data);
    GAPRole_SetParameter(GAPROLE_SCAN_RSP_DATA,sizeof(scan_data),scan_data);
    GAPRole_SetParameter(GAPROLE_MIN_CONN_INTERVAL,2,&min);
    GAPRole_SetParameter(GAPROLE_MAX_CONN_INTERVAL,2,&max);
    GAP_SetParamValue(TGAP_DISC_ADV_INT_MIN,adv);
    GAP_SetParamValue(TGAP_DISC_ADV_INT_MAX,adv);
    GAPBondMgr_SetParameter(GAPBOND_PERI_PAIRING_MODE,1,&pair);
    GGS_AddService(GATT_ALL_SERVICES);
    GATTServApp_AddService(GATT_ALL_SERVICES);
    GGS_SetParameter(GGS_DEVICE_NAME_ATT,10,"CH592-LINK");
    GATTServApp_InitCharCfg(INVALID_CONNHANDLE,cccd);
    linkDB_Register(connection_status_changed);
    status=GATTServApp_RegisterService(attrs,GATT_NUM_ATTRS(attrs),GATT_MAX_ENCRYPT_KEY_SIZE,&service_cbs);
    PRINT("BLE service: %u\r\n",status);
    if(task_id==0xff || status!=SUCCESS) while(1) {}
    link_ble_programmer_init();
    tmos_set_event(task_id,START_EVENT);
}
void link_ble_poll(void)
{
    uint32_t now=link_ble_now_ms();
    main_poll_count++;
    if(restart_advertising && conn==INVALID_CONNHANDLE && (int32_t)(now-advertising_due)>=0) {
        uint8_t enable;
        uint32_t role=0;
        live_role_status=GAPRole_GetParameter(GAPROLE_STATE,&role);
        live_role_state=(uint8_t)(role & GAPROLE_STATE_ADV_MASK);
        advertising_due=now+100u;
        if(live_role_status==SUCCESS && live_role_state==GAPROLE_ADVERTISING) {
            advertising=1;
            restart_advertising=advertising_phase=0;
        } else if(live_role_status==SUCCESS &&
                  (live_role_state==GAPROLE_WAITING || live_role_state==GAPROLE_STARTED)) {
            if(advertising_phase==0) {
                /* V1.40 TRUE->TRUE returns SUCCESS without scheduling START.
                 * Force a real disable/enable edge, but only in an idle role. */
                enable=FALSE;
                last_adv_status=GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED,1,&enable);
                if(last_adv_status==SUCCESS) { advertising_phase=1; advertising_due=now+10u; }
                else advertising_due=now+250u;
            } else if(advertising_phase==1) {
                enable=TRUE;
                advertising_attempts++;
                last_adv_status=GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED,1,&enable);
                if(last_adv_status==SUCCESS) { advertising_phase=2; advertising_deadline=now+1000u; }
                else { advertising_phase=0; advertising_due=now+250u; }
            } else if((int32_t)(now-advertising_deadline)>=0) {
                advertising_phase=0;
                advertising_due=now+250u;
            }
        }
        /* CONNECTED/CONNECTED_ADV and transient roles are observed only.
         * A delayed connection callback must never trigger an advertising API. */
    }
    if(close_phase && conn!=INVALID_CONNHANDLE && !terminating) {
        if(close_phase==CLOSE_WAIT_ACK && !tx_len) {
            uint32_t grace=((uint32_t)conn_interval*15u+3u)/4u+50u;
            if(grace<100u) grace=100u;
            if(grace>500u) grace=500u;
            close_due=now+grace;
            close_phase=CLOSE_WAIT_CLEANUP;
        }
        if((close_phase==CLOSE_WAIT_CLEANUP && (int32_t)(now-close_due)>=0 &&
            link_session_owner()!=LINK_OWNER_BLE) || (int32_t)(now-close_deadline)>=0) {
            /* The response is an acceptance ACK. Actual completion is the
             * subsequent GAP termination event, never browser-local close. */
            begin_termination(5,now);
            close_phase=CLOSE_TERMINATING;
        }
    }
    if(conn!=INVALID_CONNHANDLE && !terminating && (invalid_transport ||
       ((rx_len || tx_len || executing) && (uint32_t)(now-last_activity)>30000u))) {
        begin_termination(invalid_transport?1:rx_len?2:tx_len?3:4,now);
    }
    if(terminating && conn!=INVALID_CONNHANDLE && last_terminate_status!=SUCCESS &&
       (int32_t)(now-terminate_due)>=0) {
        last_terminate_status=GAPRole_TerminateLink(conn);
        terminate_due=now+250u;
    }
    link_ble_programmer_poll();
    if(conn==INVALID_CONNHANDLE || invalid_transport) return;
    if(request_ready && rx_len>=5 && transport_buffer.rx[0]==11u) {
        uint8_t valid=rx_len==5u;
        memset(transport_buffer.tx,0,10); transport_buffer.tx[0]=0x8b; transport_buffer.tx[1]=valid?0:1;
        tx_len=6;
        put32(transport_buffer.tx+tx_len,crc32(transport_buffer.tx,tx_len)); tx_len+=4; tx_offset=0;
        request_ready=0; rx_len=rx_total=0;
        last_activity=now; notify_retry_due=now;
        if(valid) {
            close_phase=CLOSE_WAIT_ACK;
            close_deadline=now+2000u;
            link_ble_programmer_disconnect();
        }
    }
    if(request_ready && rx_len>=5 && transport_buffer.rx[0]==10u) {
        if(rx_len==5) tx_len=build_link_status(transport_buffer.tx);
        else { memset(transport_buffer.tx,0,6); transport_buffer.tx[0]=0x8a; transport_buffer.tx[1]=1; tx_len=6; }
        request_ready=0; rx_len=rx_total=0;
        put32(transport_buffer.tx+tx_len,crc32(transport_buffer.tx,tx_len)); tx_len+=4; tx_offset=0;
        last_activity=now; notify_retry_due=now;
    }
    if(request_ready && link_ble_programmer_start(transport_buffer.rx,rx_len-4)) {
        request_ready=0; executing=1; rx_len=rx_total=0;
    }
    if(executing && link_ble_programmer_response(transport_buffer.tx,&tx_len)) {
        executing=0;
        put32(transport_buffer.tx+tx_len,crc32(transport_buffer.tx,tx_len));
        tx_len+=4; tx_offset=0;
        last_activity=now; notify_retry_due=now;
    }
    if(tx_len && (int32_t)(now-notify_retry_due)>=0) {
        uint8_t burst;
        /* Queue a bounded burst. Actual credits are owned by the BLE stack;
         * a busy/empty pool keeps tx_offset unchanged and yields to TMOS. */
        for(burst=0;burst<4u && tx_len;burst++) {
            attHandleValueNoti_t noti;
            uint16_t count=tx_len-tx_offset;
            bStatus_t status;
            if(count>14) count=14;
            noti.handle=attrs[2].handle; noti.len=count+6;
            noti.pValue=GATT_bm_alloc(conn,ATT_HANDLE_VALUE_NOTI,noti.len,NULL,0);
            if(!noti.pValue) { notify_retry_due=now+1u; break; }
            put16(noti.pValue,rx_seq); put16(noti.pValue+2,tx_offset); put16(noti.pValue+4,tx_len);
            memcpy(noti.pValue+6,transport_buffer.tx+tx_offset,count);
            status=GATT_Notification(conn,&noti,FALSE);
            if(status==SUCCESS) {
                notify_retry_due=now;
                tx_offset+=count;
                if(tx_offset==tx_len) tx_len=tx_offset=0;
            } else {
                GATT_bm_free((gattMsg_t *)&noti,ATT_HANDLE_VALUE_NOTI);
                notify_retry_due=now+1u;
                break;
            }
        }
    }
}
