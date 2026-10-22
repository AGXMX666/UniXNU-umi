// SPDX-License-Identifier: BSD-2-Clause-Patent
// Mi 10 ST FTM5 @ I2C 0x49. Dedicated worker owns transport, power and FIFO.
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <stdarg.h>
#include "UmiTouchPlatform.h"
#include "UmiFtsCore.h"
#include "UmiFtsStartup.h"
#include "UmiFtsRetry.h"
#include "UmiFtsSensing.h"
#include "UmiFtsSysInfo.h"
#include "UmiFtsFrameSample.h"
#include "UmiFtsFrameProbe.h"
#include "UmiFtsLogLimits.h"
#include "UmiTouchLog.h"
extern "C" {
volatile uint32_t native_touch_seq,native_touch_count,native_touch_hid_reports,native_touch_loop_step;
volatile int native_touch_xs[12],native_touch_ys[12],native_touch_ids[12];
volatile unsigned native_touch_stage_req;
char native_touch_rows[4][120];
extern char boot_progress_ext[8][48];
extern volatile uint32_t native_hid_stage,native_hid_resource_found,native_hid_input_events[8],native_touch_edge_stats[4];
extern volatile int native_hid_load_status,native_hid_matching_load_status,native_hid_input_results[8];
}
static uint64_t epoch;
static unsigned stage,failure,rawCount,pointCount,resetCount,chipErrors,queueDrops,transportErrors,lastResult,lastAck;
static uint8_t lastRaw[8];
static unsigned sensingRuns,sensingCompleted,sensingFailure,irqLowPolls,irqHighPolls,fifoEmpty,fifoNonempty;
static uint64_t sensingElapsedNs,maxReadNs;
static unsigned irqTransitions;static bool irqObserved,previousIrq;
static UmiFtsSysInfo controllerInfo;
static UmiFtsFrameSample frameSample;
static unsigned seconds(void){return frequency?(unsigned)((GetPerformanceCounter()-epoch)/frequency):0;}
static UmiTouchLog debugLog;
static void touchLogLine(const char *line){IOLog("%s",line);}
static void touchFlush(void) {
    debugLog.flush(ml_get_interrupts_enabled() && !ml_at_interrupt_context(),touchLogLine);
}
static void touchDebug(unsigned,const char *format,...) {
    va_list a;va_start(a,format);debugLog.append(format,a);va_end(a);touchFlush();
}
struct Region {uintptr_t physical;unsigned length;IOMemoryMap *mapping;};
static Region regions[]={
 {0x00a94000,0x1000,nullptr},{0x0f500000,0x46000,nullptr},
 {0x0c440000,0x2000,nullptr},{0x0c40a000,0x1000,nullptr},
 {0x0e600000,0x10000,nullptr},{0x0c3f0000,0x1000,nullptr},
 {0x80860000,0x20000,nullptr},{0x18220000,0x3000,nullptr}};
static uintptr_t virtualAddress(uintptr_t physical,unsigned bytes) {
    for(auto &r:regions)if(r.mapping && physical>=r.physical && physical-r.physical<=r.length-bytes)
        return r.mapping->getVirtualAddress()+physical-r.physical;
    panic("umi FTS unqualified MMIO %lx",(unsigned long)physical);
}
static uint32_t MmioRead32(uintptr_t address) {
    uint64_t value;uintptr_t mapped=virtualAddress(address,4);
    __asm__ volatile("ldr %w0,[%1]\n\tuxtw %0,%w0":"=&r"(value):"r"(mapped):"memory");return (uint32_t)value;
}
static uint16_t MmioRead16(uintptr_t address) {return (uint16_t)(MmioRead32(address&~(uintptr_t)3)>>((address&2)*8));}
static void MmioWrite32(uintptr_t address,uint32_t value) {
    uintptr_t mapped=virtualAddress(address,4);
    __asm__ volatile("dsb sy\n\tstr %w0,[%1]\n\tdsb sy"::"r"(value),"r"(mapped):"memory");
}
namespace spmi {
#include "UmiSpmi.inc"
}
using spmi::UmiSpmiRead8;
namespace rpmh {
#include "UmiRpmh.inc"
}
using rpmh::UmiRpmhTouchEnable;using rpmh::UmiRpmhTouchDisable;using rpmh::UmiRpmhTouchOutstanding;
namespace bus {
#include "UmiFtsBus.inc"
}
// SPSC frame queue: producer publishes a complete frame; HID consumes in order.
// Overflow stops the controller and queues cancellation after all accepted frames.
static UmiTouchFrame frames[64],state;
static unsigned producer,consumer,cancelPending;
static UmiTouchFrame cancelFrame;
static bool queue(const UmiTouchFrame &f) {
    unsigned p=__atomic_load_n(&producer,__ATOMIC_RELAXED),c=__atomic_load_n(&consumer,__ATOMIC_ACQUIRE);
    if(p-c>=64){++queueDrops;return false;}
    frames[p%64]=f;__atomic_store_n(&producer,p+1,__ATOMIC_RELEASE);return true;
}
extern "C" bool umi_touch_next(UmiTouchFrame *frame) {
    unsigned c=__atomic_load_n(&consumer,__ATOMIC_RELAXED),p=__atomic_load_n(&producer,__ATOMIC_ACQUIRE);
    if(c==p){if(!__atomic_exchange_n(&cancelPending,0,__ATOMIC_ACQUIRE))return false;*frame=cancelFrame;return true;}
    *frame=frames[c%64];__atomic_store_n(&consumer,c+1,__ATOMIC_RELEASE);return true;
}
static bool publish(void) {
    ++state.sequence;
    uint32_t seq=__atomic_load_n(&native_touch_seq,__ATOMIC_RELAXED);
    __atomic_store_n(&native_touch_seq,seq+1,__ATOMIC_SEQ_CST);
    unsigned n=0;for(unsigned i=0;i<10;++i)if(state.points[i].active){
        native_touch_ids[n]=i;native_touch_xs[n]=state.points[i].x;native_touch_ys[n]=state.points[i].y;++n;
    }native_touch_count=n;__atomic_store_n(&native_touch_seq,seq+2,__ATOMIC_RELEASE);
    return queue(state);
}
// Bounded logs: up to48 protocol records (one reserved stop) and64 contacts,
// plus periodic summaries. Separate quotas keep startup status from hiding fingers.
static uint64_t logs[UMI_FTS_LOG_LIMIT][16];static unsigned logPublished,logConsumed,rawQuota,pointQuota;
static void record(const uint8_t e[8],unsigned result,bool diagnostic=false) {
    bool point=!diagnostic && (e[0]==0x13||e[0]==0x23||e[0]==0x33);
    if((!e[0]&&!diagnostic) || (point?pointQuota>=UMI_FTS_POINT_LIMIT:rawQuota>=(diagnostic?UMI_FTS_STOP_LIMIT:UMI_FTS_PROTOCOL_LIMIT)))return;
    unsigned i=logPublished;if(i>=UMI_FTS_LOG_LIMIT)return;
    point?++pointQuota:++rawQuota;
    uint64_t *v=logs[i];v[0]=1;v[1]=i+1;v[2]=seconds();v[3]=result;
    memcpy(&v[4],e,8);v[5]=lastAck;v[6]=stage;v[7]=state.sequence;v[8]=native_touch_count;
    for(unsigned id=0;id<10;++id)if(state.points[id].active)v[9]|=1U<<id;
    unsigned id=e[1]>>4;if(id<10){v[10]=id;v[11]=state.points[id].x;v[12]=state.points[id].y;}
    v[13]=transportErrors;v[14]=queueDrops;v[15]=native_touch_hid_reports;
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
extern "C" unsigned umi_touch_log_peek(uint64_t values[16]) {
    unsigned n=__atomic_load_n(&logPublished,__ATOMIC_ACQUIRE);
    if(logConsumed>=n)return 0;memcpy(values,logs[logConsumed],sizeof(logs[0]));return logConsumed+1;
}
extern "C" void umi_touch_log_commit(unsigned id) {if(id==logConsumed+1)++logConsumed;}
static void recordBus(unsigned result,unsigned failures,bool retry,bool cancelled) {
    if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
    unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
    v[0]=2;v[1]=i+1;v[2]=seconds();v[3]=result;v[4]=bus::mLastPhase;v[5]=bus::mLastByte;
    v[6]=bus::mLastElapsedNs/1000;v[7]=bus::mLastStopResult;v[8]=bus::mLastLines;v[9]=lastAck;
    v[10]=bus::mLastGeni;v[11]=bus::mLastPinsValid;v[12]=failures;v[13]=cancelled;
    v[14]=bus::mLastMaxGapNs/1000;v[15]=retry;
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
static void recordRecovery(unsigned failures,bool retry,bool cancelled) {
    if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
    unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
    v[0]=3;v[1]=i+1;v[2]=seconds();v[3]=bus::mRecoveryResult;v[4]=bus::mRecoveryPulses;
    v[5]=bus::mRecoveryElapsedNs/1000;v[6]=bus::mLastLines;v[7]=bus::mRecoveryLines;
    v[8]=bus::mLastPhase;v[9]=bus::mLastByte;v[10]=bus::High(39);
    v[11]=bus::mLastPinsValid;v[12]=bus::mLastGeni;v[13]=failures;v[14]=cancelled;v[15]=retry;
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
static void recordSensing(void) {
    if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
    unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
    v[0]=4;v[1]=i+1;v[2]=seconds();v[3]=sensingRuns;v[4]=sensingCompleted;v[5]=sensingFailure;
    v[6]=sensingElapsedNs/1000;v[7]=bus::High(39);v[8]=irqLowPolls;v[9]=irqHighPolls;
    v[10]=fifoEmpty;v[11]=fifoNonempty;v[12]=bus::mLastElapsedNs/1000;v[13]=maxReadNs/1000;
    v[14]=MmioRead32(GPIO_CTL(39));v[15]=MmioRead32(GPIO_IO(39));
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
    if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
    i=logPublished;++rawQuota;v=logs[i];
    v[0]=7;v[1]=i+1;v[2]=seconds();v[3]=irqTransitions;v[4]=irqLowPolls;v[5]=irqHighPolls;
    v[6]=bus::High(39);v[7]=bus::mSavedIrqCtl;v[8]=bus::mActiveIrqCtl;
    v[9]=MmioRead32(GPIO_CTL(39));v[10]=MmioRead32(SE13_BASE+0x40);v[11]=bus::PowerReady();
    v[12]=native_touch_loop_step;v[13]=fifoEmpty;v[14]=fifoNonempty;v[15]=maxReadNs/1000;
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
static void recordController(const UmiFtsControllerRead &r,uint64_t elapsedNs) {
    // Retain only complete successful chunks; never log a partially read tail.
    for(unsigned offset=0;offset<r.bytesRead;offset+=88){
        if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
        unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
        unsigned length=r.bytesRead-offset;if(length>88)length=88;
        v[0]=5;v[1]=i+1;v[2]=seconds();v[3]=offset;v[4]=length;
        memcpy(v+5,r.bytes+offset,length);
        __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
    }
    if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
    UmiFtsSysInfo info=umi_fts_sysinfo(r.bytes,r.bytesRead);
    unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
    v[0]=6;v[1]=i+1;v[2]=seconds();
    v[3]=(r.bytesRead==208?1U:0U)|(info.valid?2U:0U)|(!r.crcResult?4U:0U);
    v[4]=r.sysResult;v[5]=r.bytesRead;v[6]=r.chunkMask;v[7]=r.crcResult;
    v[8]=r.crcResult?UINT64_MAX:r.crc;v[9]=bus::mSavedIrqCtl;
    v[10]=MmioRead32(GPIO_CTL(39));v[11]=irqTransitions;
    v[12]=info.valid?((uint64_t)info.config<<16)|info.fw:UINT64_MAX;
    v[13]=info.valid?info.svn:UINT64_MAX;
    v[14]=info.valid?((unsigned)info.panelAfe<<16)|((unsigned)info.cxAfe<<8)|info.cfgAfe:UINT64_MAX;
    v[15]=elapsedNs/1000;
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
static void recordFrameSample(unsigned number,uint64_t elapsedNs) {
    if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
    unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
    v[0]=8;v[1]=i+1;v[2]=seconds();v[3]=number;v[4]=frameSample.result;v[5]=frameSample.validMask;
    for(unsigned n=0;n<3;++n){
        v[6+n]=frameSample.addresses[n];
        if(frameSample.validMask&(1U<<n))memcpy(v+9+n*2,frameSample.data[n],16);
    }
    v[15]=elapsedNs/1000;
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
static void recordBringup(unsigned phase,unsigned result,uint64_t elapsedNs) {
    if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
    uint8_t value[4]={};unsigned valid=0;
    const uint16_t address[]={0x4046,0x4008,0x4042,0x4043};
    // Keep SPMI diagnostics out of the reset-release -> READY deadline.
    if(phase!=2)for(unsigned n=0;n<4;++n)if(!UmiSpmiRead8(bus::mSupplySid,address[n],value+n))valid|=1U<<n;
    unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
    v[0]=9;v[1]=i+1;v[2]=seconds();v[3]=phase;v[4]=resetCount;v[5]=result;v[6]=elapsedNs/1000;
    v[7]=valid;v[8]=value[0];v[9]=value[1];v[10]=unsigned(value[2])|(unsigned(value[3])<<8);
    v[11]=MmioRead32(GPIO_CTL(38));v[12]=MmioRead32(GPIO_IO(38));
    v[13]=MmioRead32(GPIO_CTL(69));v[14]=MmioRead32(GPIO_IO(69));v[15]=MmioRead32(GPIO_IO(39));
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
static void recordFrameProbe(unsigned number,const UmiFtsFrameProbe &r,uint64_t elapsedNs) {
    if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
    unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
    v[0]=10;v[1]=i+1;v[2]=seconds();v[3]=number;v[4]=r.mode;v[5]=r.flags;
    v[6]=r.startResult;v[7]=r.offResult;v[8]=r.frameResult;
    v[9]=r.restoreOnResult;v[10]=r.restoreGestureResult;v[11]=frameSample.validMask;
    v[12]=elapsedNs/1000;v[13]=(r.flags&64)!=0;v[14]=bus::PowerReady();
    v[15]=MmioRead32(SE13_BASE+0x40);__atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
extern "C" void umi_touch_snapshot(uint64_t v[32]) {
    memset(v,0,32*sizeof(*v));v[0]=1;v[1]=stage;v[2]=failure;v[3]=rawCount;v[4]=pointCount;v[5]=resetCount;
    v[6]=chipErrors;v[7]=transportErrors;v[8]=lastResult;v[9]=lastAck;memcpy(v+10,lastRaw,8);
    v[11]=native_touch_count;v[12]=native_touch_seq;v[13]=queueDrops;v[14]=native_hid_stage;
    v[15]=(uint32_t)native_hid_load_status;v[16]=(uint32_t)native_hid_matching_load_status;v[17]=native_hid_resource_found;
    v[18]=native_hid_input_events[2];v[19]=native_hid_input_events[6];v[20]=(uint32_t)native_hid_input_results[6];
    v[21]=native_touch_hid_reports;v[22]=native_touch_loop_step;v[23]=producer-consumer;
    for(unsigned i=0;i<4;++i)v[24+i]=native_touch_edge_stats[i];
    v[28]=state.sequence;v[29]=logPublished;v[30]=seconds();v[31]=native_hid_input_events[7];
    snprintf(boot_progress_ext[4],48,"FTS stage=%u err=%u raw=%u pt=%u",stage,failure,rawCount,pointCount);
    snprintf(boot_progress_ext[5],48,"TOUCH n=%u q=%u drop=%u ack=%x",native_touch_count,producer-consumer,queueDrops,lastAck);
    snprintf(boot_progress_ext[6],48,"HID st=%u start=%u rpt=%u rc=%x",native_hid_stage,native_hid_input_events[2],native_touch_hid_reports,(unsigned)native_hid_input_results[6]);
    snprintf(boot_progress_ext[7],48,"EDGE on=%u start=%u edge=%u",native_touch_edge_stats[0],native_touch_edge_stats[1],native_touch_edge_stats[2]);
    snprintf(native_touch_rows[0],120,"FTS stage=%u failure=%u raw=%u points=%u I2C errors=%u last=%02x/%02x",stage,failure,rawCount,pointCount,transportErrors,lastRaw[0],lastRaw[1]);
}
static bool isUmi(void) {
    IORegistryEntry *root=IORegistryEntry::fromPath("/product",gIODTPlane);if(!root)return false;
    OSData *model=OSDynamicCast(OSData,root->getProperty("model"));const char expected[]="Qualcomm Technologies, Inc. xiaomi umi";
    bool match=model && model->getLength()==sizeof(expected) && !memcmp(model->getBytesNoCopy(),expected,sizeof(expected));root->release();return match;
}
static int transfer(const uint8_t *w,unsigned wn,uint8_t *r,unsigned rn) {
    // Only this worker drives the bus. Do not hold an IRQ mask over I2C.
    int rc=bus::Transfer(w,wn,r,rn);lastResult=rc;lastAck=bus::mAckMask;if(rc)++transportErrors;return rc;
}
static void stop(unsigned why) {
    failure=why;stage=90;touchDebug(0,"UMI FTS stopped reason=%u transfer=%u ack=%x\n",why,lastResult,lastAck);
    // Preserve the actual last FIFO bytes even when no FIFO read succeeded.
    // Result 0x100|reason denotes a driver stop, not a fabricated controller event.
    record(lastRaw,0x100|why,true);
    for(auto &p:state.points)p.active=0;
    // A separate terminal mailbox survives a full queue and a late HID attach.
    // Consumer drains accepted frames first, then the explicit all-up state.
    if(!publish()){cancelFrame=state;__atomic_store_n(&cancelPending,1,__ATOMIC_RELEASE);}
    bus::ReleasePins();bus::RestorePower();
    touchFlush();
}
static void worker(void *,wait_result_t) {
    thread_set_thread_name(current_thread(),"umi_fts");stage=2;
    if(!isUmi()){failure=1;stage=90;return;}
    for(auto &r:regions){IODeviceMemory *mem=IODeviceMemory::withRange(r.physical,r.length);
        if(mem){r.mapping=mem->map(kIOMapAnywhere|kIOMapInhibitCache);mem->release();}
        if(!r.mapping){failure=2;stage=90;return;}}
    stage=3;bus::Snapshot();int rc=bus::AcquirePins();lastResult=rc;touchFlush();
    if(rc){stop(3);return;}stage=4;
    uint8_t command=0x86,e[8];bool scan=false;unsigned consecutiveFailures=0,slowRecords=0;
    unsigned healthRecord=0;const unsigned healthTimes[]={10,60,120,180,240};
    unsigned sampleNumber=0;const unsigned sampleTimes[]={5,30,90};uint64_t sampleStarted=0;
    for(;;){
        unsigned sleepMs=2;
        native_touch_loop_step=native_touch_loop_step+1;
        bool irq=bus::High(39);
        if(irqObserved && irq!=previousIrq)++irqTransitions;irqObserved=true;previousIrq=irq;
        if(irq)++irqHighPolls;else ++irqLowPolls;
        // At most eight records per pass. Keep the complete FIFO order and
        // yield between batches; a busy touch source cannot monopolize a CPU.
        // Original low-level IRQ handler reads FIFO only when events are ready.
        // Polling the GPIO keeps the I2C bus quiet while IRQ is high.
        bool readFifo=stage!=6 || !irq;
        for(unsigned n=0;readFifo && n<8;++n){
            if(stage==4 && umi_fts_ready_expired(GetTimeInNanoSecond(GetPerformanceCounter()-bus::mReadyStart))){stop(11);return;}
            rc=transfer(&command,1,e,8);
            if(rc){
                if(stage==4 && rc==EFI_NO_RESPONSE &&
                   !umi_fts_ready_expired(GetTimeInNanoSecond(GetPerformanceCounter()-bus::mReadyStart)))break;
                ++consecutiveFailures;
                bool recoveryAttempted=stage==6 && consecutiveFailures<3 && bus::RecoveryEligible(rc);
                bool recovered=recoveryAttempted && bus::RecoverRead(rc)==EFI_SUCCESS;
                bool retry=stage==6 && umi_fts_retry_read(rc,consecutiveFailures,
                    recovered || bus::mLastStopResult==EFI_SUCCESS,recovered || bus::mLastLines==3,
                    bus::mLastPinsValid,(bus::mLastGeni & GENI_BUSY_MASK)==0);
                bool cancelled=false;
                if(retry) {
                    // A partial FIFO read may consume a contact edge. Release
                    // existing contacts before accepting any subsequent event.
                    for(auto &point:state.points)if(point.active){point.active=0;cancelled=true;}
                    if(cancelled && !publish()){recordBus(rc,consecutiveFailures,false,true);stop(6);return;}
                }
                recordBus(rc,consecutiveFailures,retry,cancelled);
                if(recoveryAttempted)recordRecovery(consecutiveFailures,retry,cancelled);
                if(retry){sleepMs=2;break;}
                stop(4);return;
            }
            consecutiveFailures=0;
            if(bus::mLastElapsedNs>maxReadNs)maxReadNs=bus::mLastElapsedNs;
            if(e[0])++fifoNonempty;else ++fifoEmpty;
            // Preserve a few successful transactions above the old 8 ms limit.
            if(bus::mLastElapsedNs>8000000 && slowRecords<4){recordBus(0,0,false,false);++slowRecords;}
            memcpy(lastRaw,e,8);if(e[0])++rawCount;
            unsigned decoded=umi_fts_startup_event(&state,e,stage==4,
                GetTimeInNanoSecond(GetPerformanceCounter()-bus::mReadyStart));
            if(decoded==UMI_FTS_READY_TIMEOUT){record(e,decoded);stop(11);return;}
            if(decoded==UMI_FTS_WAIT_READY){record(e,decoded);break;}
            if(decoded==UMI_FTS_RESET){++resetCount;if(resetCount>3){record(e,decoded);stop(5);return;}scan=true;stage=5;publish();}
            if(decoded==UMI_FTS_POINT){++pointCount;if(!publish()){record(e,decoded);stop(6);return;}}
            if(decoded==UMI_FTS_FAULT){++chipErrors;record(e,decoded);stop(7);return;}
            if(decoded==UMI_FTS_BAD){record(e,decoded);stop(8);return;}
            record(e,decoded);
            if(scan){
                if(resetCount==1){
                    uint64_t started=GetPerformanceCounter();
                    UmiFtsControllerRead controller=umi_fts_read_controller(
                        [](const uint8_t *w,unsigned wn,uint8_t *r,unsigned rn){return transfer(w,wn,r,rn);});
                    controllerInfo=umi_fts_sysinfo(controller.bytes,controller.bytesRead);
                    recordController(controller,GetTimeInNanoSecond(GetPerformanceCounter()-started));
                    if(bus::mLastLines!=3){stop(15);return;}
                    // Original fts_fw_update does a post-CRC hardware reset.
                    // A second actual READY is required before the first sense-on.
                    recordBringup(1,0,0);started=GetPerformanceCounter();
                    unsigned resetResult=bus::ResetController();
                    recordBringup(2,resetResult,GetTimeInNanoSecond(GetPerformanceCounter()-started));
                    if(resetResult){stop(16);return;}
                    scan=false;stage=4;break;
                }
                if(!sensingRuns)recordBringup(3,0,GetTimeInNanoSecond(GetPerformanceCounter()-bus::mReadyStart));
                ++sensingRuns;uint64_t started=GetPerformanceCounter();
                sensingFailure=umi_fts_start_sensing(sensingRuns==1,
                    [](const uint8_t *w,unsigned n){return transfer(w,n,nullptr,0);},
                    [](unsigned ms){IOSleep(ms);},&sensingCompleted);
                sensingElapsedNs=GetTimeInNanoSecond(GetPerformanceCounter()-started);
                recordSensing();
                if(sensingFailure){stop(sensingFailure);return;}scan=false;stage=6;
            }
            if(!e[0] || !(e[7]&31))break;
        }
        // Only when no contact has ever arrived, compare source-defined normal
        // and locked fresh-frame snapshots. Both restore normal scan immediately.
        // Otherwise use the original non-blocking live read, with input priority.
        if(stage==6 && !native_touch_count && bus::High(39) && sampleNumber<ARRAY_SIZE(sampleTimes)){
            bool ready=false;
            if(!frameSample.active && seconds()>=sampleTimes[sampleNumber]){
                sampleStarted=GetPerformanceCounter();
                if(sampleNumber<2 && !pointCount && !native_touch_hid_reports){
                    UmiFtsFrameProbe probe=umi_fts_probe_frame(controllerInfo,frameSample,sampleNumber?3:0,
                        [](const uint8_t *w,unsigned n){return transfer(w,n,nullptr,0);},
                        [](const uint8_t *w,unsigned wn,uint8_t *r,unsigned rn){return transfer(w,wn,r,rn);},
                        [](unsigned ms){IOSleep(ms);},[](){return bus::ControlReady();});
                    recordFrameProbe(sampleNumber+1,probe,GetTimeInNanoSecond(GetPerformanceCounter()-sampleStarted));
                    if(((probe.flags&128) && !(probe.flags&1)) || ((probe.flags&1) && !probe.restored())){
                        recordFrameSample(++sampleNumber,GetTimeInNanoSecond(GetPerformanceCounter()-sampleStarted));
                        stop(17);return;
                    }
                    ready=true;
                }else if(!frameSample.begin(controllerInfo))ready=true;
            }
            if(frameSample.active)ready=frameSample.step(
                [](const uint8_t *w,unsigned wn,uint8_t *r,unsigned rn){return transfer(w,wn,r,rn);});
            if(ready){recordFrameSample(++sampleNumber,GetTimeInNanoSecond(GetPerformanceCounter()-sampleStarted));
                if(bus::mLastLines!=3){stop(15);return;}}
        }
        if(stage==6 && healthRecord<ARRAY_SIZE(healthTimes) && seconds()>=healthTimes[healthRecord]){
            recordSensing();++healthRecord;
        }
        IOSleep(sleepMs);
    }
}
extern "C" void native_touch_probe(void) {
    unsigned enabled=0;if(!PE_parse_boot_argn("umi-fts",&enabled,sizeof(enabled)) || enabled!=1 || stage)return;
    __asm__ volatile("mrs %0,cntfrq_el0":"=r"(frequency));if(!frequency)return;epoch=GetPerformanceCounter();
    native_touch_stage_req=1;stage=1;thread_t t;
    if(kernel_thread_start(worker,nullptr,&t)==KERN_SUCCESS)thread_deallocate(t);else{stage=90;failure=12;}
}
