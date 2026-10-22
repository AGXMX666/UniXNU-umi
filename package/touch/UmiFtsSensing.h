// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
// umi fts_mode_handler followed by fts_init_sensing's CONFIG_TOUCHSCREEN_FOD
// branch. WAIT_AFTER_SENSEOFF is 50 ms. All commands change volatile scan state.
template<class Write, class Sleep>
static unsigned umi_fts_start_sensing(bool initial, Write write, Sleep sleep,
                                      unsigned *completed) {
    const uint8_t on[]={0xa0,0x00,0x01};
    const uint8_t reports[]={0xa2,0x03,0,0,0,0x03};
    const uint8_t off[]={0xa0,0x00,0x00};
    *completed=0;
    if(write(on,sizeof(on)))return 9;
    *completed=1;
    if(write(reports,sizeof(reports)))return 10;
    *completed=2;
    if(!initial)return 0;
    if(write(off,sizeof(off)))return 13;
    *completed=3;
    sleep(50);
    if(write(on,sizeof(on)))return 14;
    *completed=4;
    return 0;
}
