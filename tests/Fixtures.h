#pragma once
#include "Decoder.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>
namespace fixture {
inline uint32_t raw(uint32_t payload,bool v=false,bool u=false,bool c=false) {
 uint32_t x=(payload&0xffffffu)|(uint32_t(v)<<24)|(uint32_t(u)<<25)|(uint32_t(c)<<26);
 unsigned p=0; for(unsigned b=0;b<27;++b)p^=(x>>b)&1u;
 return x|(p<<27);
}
struct Expected {uint64_t start; uint32_t raw; char preamble; std::array<uint64_t,8> field_boundaries{};};
struct Signal {
 double capture,fs,t=100; bool level=false; double jitter=0; uint32_t noise=1;
 std::vector<uint64_t> edges; std::vector<Expected> expected;
 Signal(double capture_rate,double sample_rate,bool inverted=false):capture(capture_rate),fs(sample_rate),level(inverted){}
 uint64_t stamp(double time) {noise=noise*1664525u+1013904223u; double j=(double(noise)/4294967295.0*2-1)*jitter;return uint64_t(std::llround((time+j)*capture/(128*fs)));}
 void transition(double at){level=!level;edges.push_back(stamp(at));}
 void subframe(char pre,uint32_t word) {
  double begin=t; uint64_t start=stamp(begin); // no jitter below: expected timestamps from actual first edge
  const int* runs=nullptr; static const int b[]={3,1,1,3},m[]={3,3,1,1},w[]={3,2,1,2};
  runs=pre=='B'?b:(pre=='M'?m:w); transition(t); start=edges.back();
  std::array<uint64_t,8> boundaries{}; boundaries[0]=start;
  // A subframe's exclusive P endpoint is the next actual preamble edge.
  if(!expected.empty())expected.back().field_boundaries[7]=start;
  for(unsigned i=0;i<3;++i){t+=runs[i];transition(t);} t=begin+8;
  for(unsigned bit=0;bit<28;++bit){transition(t);
   if(bit==0)boundaries[1]=edges.back();
   if(bit==4)boundaries[2]=edges.back();
   if(bit==24)boundaries[3]=edges.back();
   if(bit==25)boundaries[4]=edges.back();
   if(bit==26)boundaries[5]=edges.back();
   if(bit==27)boundaries[6]=edges.back();
   if((word>>bit)&1u)transition(t+1);
   t+=2;
  }
  expected.push_back({start,word,pre,boundaries});
 }
 void frames(unsigned count,unsigned offset=0,const std::array<uint8_t,24>* left=nullptr,const std::array<uint8_t,24>* right=nullptr) {
  for(unsigned i=0;i<count;++i){unsigned pos=(i+offset)%192;for(unsigned ch=0;ch<2;++ch){auto c=ch?right:left;bool status=c&&(((*c)[pos/8]>>(pos%8))&1);uint32_t value=uint32_t((i*0x23451u)^(ch?0xf65432u:0x800000u))&0xffffffu;subframe(ch?'W':(pos?'M':'B'),raw(value,false,(i%3)==0,status));}}
 }
};
struct Results {std::vector<spdif::Subframe> frames;std::vector<spdif::StatusBlock> blocks;std::vector<spdif::Diagnostic> diagnostics;};
inline Results decode(const Signal&s,double nominal=0,uint64_t start=0,uint64_t end=UINT64_MAX){Results r;spdif::Sink sink{[&](const spdif::Subframe& f){r.frames.push_back(f);},[&](const spdif::StatusBlock& b){r.blocks.push_back(b);},[&](const spdif::Diagnostic&d){r.diagnostics.push_back(d);},{}};spdif::Decoder d({s.capture,nominal},sink);for(auto e:s.edges)if(e>=start&&e<end)d.edge(e);d.finish(end==UINT64_MAX?(s.edges.empty()?0:s.edges.back()):end);return r;}
}
