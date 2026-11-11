#include "Decoder.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
// Stream one minute, avoiding fixture vectors and result retention. This measures
// core throughput, not Logic 2's frame storage or rendering overhead.
int main(){uint64_t count=0,diagnostics=0;spdif::Sink sink{[&](const spdif::Subframe&){++count;},{},[&](const spdif::Diagnostic&){++diagnostics;},{}};spdif::Decoder decoder({196608000,0},sink);auto begin=std::chrono::steady_clock::now();uint64_t half=100,edges=0;auto edge=[&](uint64_t h){decoder.edge(h*8);++edges;};for(uint32_t frame=0;frame<192000u*60u;++frame)for(unsigned ch=0;ch<2;++ch){uint64_t base=half;edge(base);if(ch){edge(base+3);edge(base+5);edge(base+6);}else if(frame%192==0){edge(base+3);edge(base+4);edge(base+5);}else{edge(base+3);edge(base+6);edge(base+7);}uint32_t raw=((frame*0x23451u)^(ch?0xf65432u:0x800000u))&0xffffffu;unsigned p=0;for(unsigned b=0;b<27;++b)p^=(raw>>b)&1;raw|=p<<27;for(unsigned bit=0;bit<28;++bit){edge(base+8+2*bit);if((raw>>bit)&1)edge(base+9+2*bit);}half+=64;}decoder.finish(half*8);double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();std::cout<<"capture_seconds=60 sample_rate=192000 edges="<<edges<<" subframes="<<count<<" diagnostics="<<diagnostics<<" wall_seconds="<<seconds<<" capture_seconds_per_wall_second="<<60/seconds<<"\n";return count>=23040000u-2?0:1;}
