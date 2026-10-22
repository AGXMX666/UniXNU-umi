#include <cassert>
#include <cstdio>
#include <vector>
#include "../devices/umi/package/touch/UmiFtsSensing.h"
int main(){
 const std::vector<std::vector<uint8_t>> initial={{0xa0,0,1},{0xa2,3,0,0,0,3},{0xa0,0,0},{0xa0,0,1}};
 const unsigned errors[]={9,10,13,14};
 for(bool first:{false,true})for(unsigned fail=0;fail<=4;++fail){
  unsigned completed=99,attempts=0,sleeps=0;
  auto write=[&](const uint8_t *w,unsigned n){
   assert(attempts<(first?4U:2U));assert(std::vector<uint8_t>(w,w+n)==initial[attempts]);
   if(attempts==3)assert(sleeps==1);
   ++attempts;return attempts==fail?12:0;
  };
  auto sleep=[&](unsigned ms){assert(ms==50);assert(first&&attempts==3&&completed==3);++sleeps;};
  unsigned result=umi_fts_start_sensing(first,write,sleep,&completed),total=first?4:2;
  if(fail && fail<=total){assert(result==errors[fail-1]);assert(completed==fail-1);assert(attempts==fail);}
  else{assert(result==0);assert(completed==total);assert(attempts==total);}
  assert(sleeps==(first&&(!fail||fail==4)?1U:0U));
 }
 puts("PASS: original FOD scan init order, 50ms off interval, failed-write termination and reset-resume path");
}
