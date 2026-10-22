#include <cassert>
#include <cstdio>
#include "../devices/umi/package/touch/UmiFtsStartup.h"
int main() {
    UmiTouchFrame f{};uint8_t e[8];
    f.points[1]={100,200,1};UmiTouchFrame saved=f;
    memset(e,0xff,sizeof(e));
    assert(umi_fts_startup_event(&f,e,true,0)==UMI_FTS_WAIT_READY);
    assert(umi_fts_startup_event(&f,e,true,99000000)==UMI_FTS_WAIT_READY);
    assert(!memcmp(&f,&saved,sizeof(f)));
    memset(e,0,sizeof(e));e[0]=0x13;e[1]=1;
    assert(umi_fts_startup_event(&f,e,true,2000000)==UMI_FTS_WAIT_READY);
    assert(!memcmp(&f,&saved,sizeof(f))); // Never emit pre-READY coordinates.
    e[0]=3;assert(umi_fts_startup_event(&f,e,true,4000000)==UMI_FTS_RESET);
    assert(!f.points[1].active);
    e[0]=0x13;assert(umi_fts_startup_event(&f,e,false,200000000)==UMI_FTS_POINT);
    memset(e,0xff,sizeof(e));assert(umi_fts_startup_event(&f,e,false,0)==UMI_FTS_BAD);
    e[0]=0xf3;assert(umi_fts_startup_event(&f,e,true,0)==UMI_FTS_FAULT);
    e[0]=3;saved=f;
    assert(umi_fts_startup_event(&f,e,true,100000000)==UMI_FTS_READY_TIMEOUT);
    assert(!memcmp(&f,&saved,sizeof(f)));
    assert(umi_fts_ready_expired(UINT64_MAX));
    // All unexpected first bytes are ignored only within the startup window.
    for(unsigned value=0;value<256;++value)if(value!=3&&value!=0xf3) {
        e[0]=(uint8_t)value;assert(umi_fts_startup_event(&f,e,true,99999999)==UMI_FTS_WAIT_READY);
        assert(!memcmp(&f,&saved,sizeof(f)));
    }
    puts("PASS: reset-time FF/contact/unknown bytes wait, real READY/errors, deadline boundaries and strict post-READY parser");
}
