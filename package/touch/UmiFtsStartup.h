// SPDX-License-Identifier: BSD-2-Clause-Patent
// Same 100 ms reset/READY window as umi's fts_521 GENERAL_TIMEOUT.
#pragma once
#include "UmiFtsCore.h"
enum { UMI_FTS_WAIT_READY=5, UMI_FTS_READY_TIMEOUT=6 };
static inline bool umi_fts_ready_expired(uint64_t elapsedNs) {
    return elapsedNs>=100000000ULL;
}
static inline unsigned umi_fts_startup_event(UmiTouchFrame *frame,const uint8_t event[8],
                                             bool awaitingReady,uint64_t elapsedNs) {
    if(awaitingReady) {
        if(umi_fts_ready_expired(elapsedNs))return UMI_FTS_READY_TIMEOUT;
        // Until READY, never interpret reset-time bytes as contacts or as a
        // normal-mode protocol violation. Real controller errors still fail.
        if(event[0]!=0x03 && event[0]!=0xf3)return UMI_FTS_WAIT_READY;
    }
    return umi_fts_parse(frame,event);
}
