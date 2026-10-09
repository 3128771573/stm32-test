#include "logger.h"
#include "w25qxx.h"
#include <string.h>

/* Last 64KiB only. 32-byte CRC records, commit byte programmed LAST.
   Unknown pre-existing bytes lock this region; no implicit chip erase. */
#define MAGIC 0x31474F4CUL
#define REGION_BYTES 65536UL
enum { WRITE_SELECT=1, WRITE_ERASE, WRITE_BODY, WRITE_COMMIT };
static uint8_t state, write_phase, raw[32], recent_count;
static uint16_t scan_slot, write_slot, latest_slot, count, skipped;
static uint16_t per_sector[16], recent_slot[LOGGER_RECENT];
static uint32_t recent_seq[LOGGER_RECENT], latest_seq, region_base, timeout;
static LogRecord pending;

static uint32_t get32(const uint8_t *b) {return b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);}
static void put32(uint8_t *b,uint32_t v) {uint8_t k;for(k=0;k<4;k++) b[k]=(uint8_t)(v>>(8*k));}
static uint16_t get16(const uint8_t *b) {return (uint16_t)(b[0]|((uint16_t)b[1]<<8));}
static void put16(uint8_t *b,uint16_t v) {b[0]=(uint8_t)v;b[1]=(uint8_t)(v>>8);}
static uint32_t crc32(const uint8_t *b,uint8_t size)
{
    uint32_t crc=0xFFFFFFFFUL; uint8_t i,k;
    for(i=0;i<size;i++) {crc^=b[i];for(k=0;k<8;k++) crc=(crc>>1)^((crc&1)?0xEDB88320UL:0);}
    return ~crc;
}
static uint8_t blank(const uint8_t *b) {uint8_t k;for(k=0;k<32;k++)if(b[k]!=0xFF)return 0;return 1;}
static uint32_t address(uint16_t slot) {return region_base+(uint32_t)slot*32;}
uint8_t Logger_Decode(const uint8_t b[32],LogRecord *r)
{
    if(get32(b)!=MAGIC || b[31]!=0xA5 || get32(b+24)!=crc32(b,24) || get32(b+20)>3) return 1;
    r->sequence=get32(b+4);r->uptime_s=get32(b+8);
    r->temperature_x10=(int16_t)get16(b+12);r->humidity_x10=get16(b+14);
    r->pitch_x10=(int16_t)get16(b+16);r->roll_x10=(int16_t)get16(b+18);r->flags=b[20];
    return 0;
}
static void encode(const LogRecord *r)
{
    memset(raw,0,sizeof(raw));put32(raw,MAGIC);put32(raw+4,r->sequence);put32(raw+8,r->uptime_s);
    put16(raw+12,(uint16_t)r->temperature_x10);put16(raw+14,r->humidity_x10);
    put16(raw+16,(uint16_t)r->pitch_x10);put16(raw+18,(uint16_t)r->roll_x10);
    put32(raw+20,r->flags&3);put32(raw+24,crc32(raw,24));raw[31]=0xFF;
}
static void add_recent(uint16_t slot,uint32_t seq)
{
    uint8_t pos=0,k;
    while(pos<recent_count && (int32_t)(seq-recent_seq[pos])<=0) pos++;
    if(pos==LOGGER_RECENT) return;
    if(recent_count<LOGGER_RECENT) recent_count++;
    for(k=recent_count-1;k>pos;k--) {recent_seq[k]=recent_seq[k-1];recent_slot[k]=recent_slot[k-1];}
    recent_seq[pos]=seq;recent_slot[pos]=slot;
}
static void drop_sector(uint8_t sector)
{
    uint8_t k,n=0;
    count-=per_sector[sector];per_sector[sector]=0;
    for(k=0;k<recent_count;k++) if(recent_slot[k]/128!=sector) {
        recent_slot[n]=recent_slot[k];recent_seq[n]=recent_seq[k];n++;
    }
    recent_count=n;
}
void Logger_Init(uint32_t now)
{
    state=LOG_OFFLINE;write_phase=recent_count=0;scan_slot=count=latest_slot=0;latest_seq=0;
    memset(per_sector,0,sizeof(per_sector));
    if(W25Q_Init() || W25Q_Capacity()<REGION_BYTES) return;
    region_base=W25Q_Capacity()-REGION_BYTES;state=LOG_SCANNING;timeout=now+2000;
}
uint8_t Logger_State(void) {return state;}
uint16_t Logger_Count(void) {return count;}
uint8_t Logger_RecentCount(void) {return recent_count;}
uint32_t Logger_LastSequence(void) {return latest_seq;}
uint8_t Logger_ScanProgress(void) {return (uint8_t)((uint32_t)scan_slot*100/LOGGER_SLOTS);}
uint8_t Logger_ReadRecent(uint8_t rank,LogRecord *record)
{
    uint8_t b[32];
    if(rank>=recent_count || state==LOG_SCANNING || W25Q_Read(address(recent_slot[rank]),b,32)) return 1;
    return Logger_Decode(b,record);
}
uint8_t Logger_Save(const LogRecord *record)
{
    if(state!=LOG_READY || !(record->flags&3)) return 1;
    pending=*record;pending.sequence=latest_seq+1;
    write_slot=count || latest_seq ? (latest_slot+1)%LOGGER_SLOTS : 0;
    skipped=0;write_phase=WRITE_SELECT;state=LOG_WRITING;encode(&pending);return 0;
}
void Logger_Task(uint32_t now)
{
    uint8_t b[32],busy;
    LogRecord record;
    if(state!=LOG_SCANNING && state!=LOG_WRITING) return;
    busy=W25Q_Busy();
    if(busy==2) {state=LOG_ERROR;return;}
    if(state==LOG_SCANNING) {
        if(busy) {if((int32_t)(now-timeout)>=0)state=LOG_ERROR;return;}
        if(W25Q_Read(address(scan_slot),b,32)) {state=LOG_ERROR;return;}
        if(!blank(b)) {
            if(get32(b)!=MAGIC) {state=LOG_FOREIGN;return;}
            if(!Logger_Decode(b,&record)) {
                if(!count || (int32_t)(record.sequence-latest_seq)>0) {latest_seq=record.sequence;latest_slot=scan_slot;}
                count++;per_sector[scan_slot/128]++;add_recent(scan_slot,record.sequence);
            }
        }
        if(++scan_slot==LOGGER_SLOTS) state=LOG_READY;
        return;
    }
    if(write_phase!=WRITE_SELECT && busy) {
        if((int32_t)(now-timeout)>=0)state=LOG_ERROR;
        return;
    }
    if(write_phase==WRITE_SELECT) {
        if(busy) return;
        if(!(write_slot%128)) {
            if(W25Q_EraseStart(address(write_slot))) {state=LOG_ERROR;return;}
            write_phase=WRITE_ERASE;timeout=now+1500;return;
        }
        if(W25Q_Read(address(write_slot),b,32)) {state=LOG_ERROR;return;}
        if(!blank(b)) {
            write_slot=(write_slot+1)%LOGGER_SLOTS;
            if(++skipped>=LOGGER_SLOTS)state=LOG_ERROR;
            return;
        }
        if(W25Q_ProgramStart(address(write_slot),raw,31)) {state=LOG_ERROR;return;}
        write_phase=WRITE_BODY;timeout=now+50;
    } else if(write_phase==WRITE_ERASE) {
        if(W25Q_Read(address(write_slot),b,32) || !blank(b)) {state=LOG_ERROR;return;}
        drop_sector((uint8_t)(write_slot/128));
        if(W25Q_ProgramStart(address(write_slot),raw,31)) {state=LOG_ERROR;return;}
        write_phase=WRITE_BODY;timeout=now+50;
    } else if(write_phase==WRITE_BODY) {
        if(W25Q_Read(address(write_slot),b,32) || memcmp(b,raw,31)) {state=LOG_ERROR;return;}
        b[0]=0xA5;
        if(W25Q_ProgramStart(address(write_slot)+31,b,1)) {state=LOG_ERROR;return;}
        write_phase=WRITE_COMMIT;timeout=now+50;
    } else {
        if(W25Q_Read(address(write_slot),b,32) || Logger_Decode(b,&record)) {state=LOG_ERROR;return;}
        latest_slot=write_slot;latest_seq=pending.sequence;count++;per_sector[write_slot/128]++;
        add_recent(write_slot,pending.sequence);state=LOG_READY;
    }
}
