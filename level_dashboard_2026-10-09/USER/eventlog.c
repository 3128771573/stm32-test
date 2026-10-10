#include "eventlog.h"
#include "logger.h"
#include "w25qxx.h"
#include <string.h>

/* Two private 4KiB sectors immediately before the existing 64KiB sensor log. */
#define EVENT_BYTES 8192UL
#define SENSOR_BYTES 65536UL
#define EVENT_SLOTS 256U
#define EVENT_MAGIC 0x31545645UL /* EVT1 */
#define EVENT_COMMITTED 0xA5U
#define HEARTBEAT_MS 60000UL
static uint8_t state,pending,reset_reason,raw[32],foreign,have_latest;
static uint8_t boot_recorded;
static uint16_t scan_slot,latest_slot,write_slot,search_slot,verify_slot;
static uint16_t session_count,session_slot[EVENT_SLOTS];
static uint32_t session_seq[EVENT_SLOTS],session_boot_key[EVENT_SLOTS];
static uint32_t session_uptime[EVENT_SLOTS];
static uint32_t base,latest_seq,boot_count,deadline,next_heartbeat;
static EventLast latest;

static uint32_t get32(const uint8_t *b)
{return b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);}
static void put32(uint8_t *b,uint32_t v)
{uint8_t i;for(i=0;i<4;i++)b[i]=(uint8_t)(v>>(8*i));}
static uint32_t crc32(const uint8_t *b,uint8_t n)
{
    uint32_t crc=0xFFFFFFFFUL;uint8_t i,k;
    for(i=0;i<n;i++){crc^=b[i];for(k=0;k<8;k++)crc=(crc>>1)^((crc&1)?0xEDB88320UL:0);}
    return ~crc;
}
static uint8_t blank(const uint8_t *b)
{uint8_t i;for(i=0;i<32;i++)if(b[i]!=0xFF)return 0;return 1;}
static uint32_t address(uint16_t slot){return base+(uint32_t)slot*32UL;}
static uint8_t decode(const uint8_t *b,EventLast *event)
{
    if(get32(b)!=EVENT_MAGIC||b[31]!=EVENT_COMMITTED||
       get32(b+24)!=crc32(b,24)||
       (b[20]!=EVENT_BOOT&&b[20]!=EVENT_ALIVE&&b[20]!=3)||
       b[21]>1)return 0;
    event->sequence=get32(b+4);
    event->uptime_seconds=get32(b+12);event->boot_count=get32(b+16);
    event->kind=b[20];event->reset_code=b[22];
    return 1;
}
static uint8_t newer(uint32_t a,uint32_t b){return (uint8_t)((int32_t)(a-b)>0);}
static void add_session(uint16_t slot,uint32_t seq)
{
    uint16_t pos=0,k;
    while(pos<session_count&&(int32_t)(seq-session_seq[pos])<=0)pos++;
    if(pos==EVENT_SLOTS)return;
    if(session_count<EVENT_SLOTS)session_count++;
    for(k=session_count-1;k>pos;k--){session_seq[k]=session_seq[k-1];session_slot[k]=session_slot[k-1];}
    session_seq[pos]=seq;session_slot[pos]=slot;
}
static void update_duration(const EventLast *event)
{
    uint16_t index=(uint16_t)(event->boot_count%EVENT_SLOTS);
    if(session_boot_key[index]==event->boot_count){
        if(event->uptime_seconds>session_uptime[index])session_uptime[index]=event->uptime_seconds;
    }else if(session_boot_key[index]==0xFFFFFFFFUL||newer(event->boot_count,session_boot_key[index])){
        session_boot_key[index]=event->boot_count;session_uptime[index]=event->uptime_seconds;
    }
}
static void drop_sector(uint8_t sector)
{
    uint16_t i,n=0;
    for(i=0;i<session_count;i++)if(session_slot[i]/128U!=sector){
        session_slot[n]=session_slot[i];session_seq[n]=session_seq[i];n++;
    }
    session_count=n;
}
static void prepare(uint8_t kind,uint32_t uptime_seconds)
{
    memset(raw,0xFF,sizeof(raw));
    put32(raw,EVENT_MAGIC);put32(raw+4,latest_seq+1UL);
    put32(raw+8,0xFFFFFFFFUL);
    put32(raw+12,kind==EVENT_BOOT?0:uptime_seconds);put32(raw+16,boot_count);
    raw[20]=kind;raw[21]=0;raw[22]=reset_reason;raw[23]=0;
    put32(raw+24,crc32(raw,24));
}
void EventLog_Init(uint32_t now,uint8_t reset_code)
{
    uint16_t i;
    state=EVENT_OFFLINE;pending=foreign=have_latest=boot_recorded=0;
    scan_slot=latest_slot=write_slot=search_slot=verify_slot=session_count=0;
    latest_seq=boot_count=0;
    memset(&latest,0,sizeof(latest));
    for(i=0;i<EVENT_SLOTS;i++){session_boot_key[i]=0xFFFFFFFFUL;session_uptime[i]=0;}
    reset_reason=reset_code;next_heartbeat=now+HEARTBEAT_MS;
    if(Logger_State()!=LOG_READY||W25Q_Capacity()<SENSOR_BYTES+EVENT_BYTES)return;
    base=W25Q_Capacity()-SENSOR_BYTES-EVENT_BYTES;
    state=EVENT_SCANNING;
}
uint8_t EventLog_State(void){return state;}
uint8_t EventLog_Busy(void)
{return (uint8_t)(state==EVENT_ERASING||state==EVENT_VERIFY||
    state==EVENT_BODY||state==EVENT_COMMIT);}
uint32_t EventLog_BootCount(void){return boot_count;}
uint8_t EventLog_BootRecorded(void){return boot_recorded;}
uint16_t EventLog_SessionCount(void){return session_count;}
uint8_t EventLog_ScanProgress(void)
{return (uint8_t)((uint32_t)scan_slot*100UL/EVENT_SLOTS);}
uint8_t EventLog_ReadSession(uint8_t rank,EventLast *event)
{
    uint8_t b[32];uint16_t index;
    if(!event||rank>=session_count||state!=EVENT_READY||Logger_State()!=LOG_READY)return 1;
    if(W25Q_Read(address(session_slot[rank]),b,32)){state=EVENT_ERROR;return 1;}
    if(!decode(b,event)||event->kind!=EVENT_BOOT)return 1;
    index=(uint16_t)(event->boot_count%EVENT_SLOTS);
    if(session_boot_key[index]==event->boot_count)event->uptime_seconds=session_uptime[index];
    return 0;
}
static uint8_t choose_slot(void)
{
    uint16_t end;
    uint8_t b[32];
    if(!have_latest){
        if(search_slot==EVENT_SLOTS){state=EVENT_LOCKED;return 0;}
        end=EVENT_SLOTS;
    }else{
        end=(uint16_t)((latest_slot/128U+1U)*128U);
        if(search_slot>=end){
            write_slot=(uint16_t)(end%EVENT_SLOTS);
            return 2; /* Rotate to the other sector; keep the current sector intact. */
        }
    }
    if(W25Q_Read(address(search_slot),b,32)){state=EVENT_ERROR;return 0;}
    if(blank(b)){write_slot=search_slot;return 1;}
    search_slot++;
    if(search_slot>=end&&!have_latest)state=EVENT_LOCKED;
    return 0;
}
void EventLog_Task(uint32_t now,uint32_t uptime_seconds)
{
    uint8_t b[32],slot_state,busy,kind;
    EventLast event;
    if(state==EVENT_OFFLINE||state==EVENT_LOCKED||state==EVENT_ERROR)return;
    if(Logger_State()!=LOG_READY)return;
    busy=W25Q_Busy();
    if(busy==2){state=EVENT_ERROR;return;}
    if(state==EVENT_SCANNING){
        if(busy)return;
        if(W25Q_Read(address(scan_slot),b,32)){state=EVENT_ERROR;return;}
        if(!blank(b)){
            if(decode(b,&event)){
                uint32_t seq=get32(b+4);
                if(event.kind==EVENT_BOOT)add_session(scan_slot,seq);
                update_duration(&event);
                if(!have_latest||(int32_t)(seq-latest_seq)>0){
                    have_latest=1;latest_seq=seq;latest_slot=scan_slot;latest=event;
                }
            }else if(get32(b)!=EVENT_MAGIC)foreign=1;
        }
        if(++scan_slot==EVENT_SLOTS){
            if(foreign){state=EVENT_LOCKED;return;}
            if(have_latest)boot_count=latest.boot_count;
            search_slot=have_latest?(uint16_t)(latest_slot+1U):0;
            boot_count++;pending=EVENT_BOOT;state=EVENT_READY;
        }
        return;
    }
    if(state==EVENT_ERASING){
        if(busy){if((int32_t)(now-deadline)>=0)state=EVENT_ERROR;return;}
        verify_slot=write_slot;state=EVENT_VERIFY;return;
    }
    if(state==EVENT_VERIFY){
        if(W25Q_Read(address(verify_slot),b,32)||!blank(b)){state=EVENT_ERROR;return;}
        if(++verify_slot<write_slot+128U)return;
        drop_sector((uint8_t)(write_slot/128U));
        if(W25Q_ProgramStart(address(write_slot),raw,31)){state=EVENT_ERROR;return;}
        state=EVENT_BODY;deadline=now+50;return;
    }
    if(state==EVENT_BODY){
        if(busy){if((int32_t)(now-deadline)>=0)state=EVENT_ERROR;return;}
        if(W25Q_Read(address(write_slot),b,32)||memcmp(b,raw,31)){state=EVENT_ERROR;return;}
        b[0]=EVENT_COMMITTED;
        if(W25Q_ProgramStart(address(write_slot)+31UL,b,1)){state=EVENT_ERROR;return;}
        state=EVENT_COMMIT;deadline=now+50;return;
    }
    if(state==EVENT_COMMIT){
        if(busy){if((int32_t)(now-deadline)>=0)state=EVENT_ERROR;return;}
        if(W25Q_Read(address(write_slot),b,32)||!decode(b,&event)){state=EVENT_ERROR;return;}
        latest_seq=get32(b+4);latest_slot=write_slot;latest=event;have_latest=1;
        if(event.kind==EVENT_BOOT)boot_recorded=1;
        if(event.kind==EVENT_BOOT)add_session(write_slot,latest_seq);
        update_duration(&event);
        search_slot=(uint16_t)(latest_slot+1U);
        state=EVENT_READY;return;
    }
    if((int32_t)(now-next_heartbeat)>=0&&pending==0){
        pending=EVENT_ALIVE;next_heartbeat=now+HEARTBEAT_MS;
    }
    if(!pending||busy)return;
    kind=pending;slot_state=choose_slot();
    if(!slot_state)return;
    prepare(kind,uptime_seconds);
    if(slot_state==2){
        if(W25Q_EraseStart(address(write_slot))){state=EVENT_ERROR;return;}
        state=EVENT_ERASING;deadline=now+1500;
    }else{
        if(W25Q_ProgramStart(address(write_slot),raw,31)){state=EVENT_ERROR;return;}
        state=EVENT_BODY;deadline=now+50;
    }
    pending=0;
}
