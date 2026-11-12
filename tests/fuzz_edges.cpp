// libFuzzer target: no files, SDK, clock, random seed or retained result history.
#include "core/Decoder.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
namespace {
void require(bool condition){if(!condition)std::abort();}
struct Result {uint64_t digest=1469598103934665603ull,frames=0,events=0;bool rejected=false;};
Result run(const uint8_t* data,size_t size){
 Result result;if(!size)return result;
 const uint32_t rates[]={49152000,100000000,196608000};const auto capture=rates[(data[0]>>2)%3];
 uint64_t at=(data[0]&16)?(uint64_t(1)<<54):100,last_edge=0,last_frame_end=0;bool have_edge=false,have_frame=false,stream=false;uint64_t word_index=0;
 const auto mix=[&](uint64_t n){result.digest^=n;result.digest*=1099511628211ull;};
 spdif::Sink sink;
 sink.subframe=[&](const spdif::Subframe& f){
  require(++result.events<100000);require(f.end>=f.start&&f.end!=UINT64_MAX&&f.channel<2&&f.raw<=0xfffffff);
  require(!have_frame||f.start>last_frame_end);have_frame=true;last_frame_end=f.end;
  require(f.field_boundaries[0]==f.start&&f.field_boundaries[7]==f.end+1);
  for(size_t i=1;i<8;++i)require(f.field_boundaries[i]>f.field_boundaries[i-1]);
  require(std::isfinite(f.sample_rate)&&f.sample_rate>0);
  const auto payload=f.raw&0xffffff;const int32_t expected=payload&0x800000?int32_t(payload)-0x1000000:int32_t(payload);require(expected==f.sample);
  unsigned ones=0;for(unsigned bit=0;bit<28;++bit)ones+=(f.raw>>bit)&1;require(f.parity_ok==((ones&1)==0));
  ++result.frames;mix(f.start);mix(f.raw);mix(uint64_t(f.content));
 };
 sink.status=[&](const spdif::StatusBlock& b){require(++result.events<100000);require(b.channel<2&&b.end>=b.start);mix(b.start);mix(b.end);};
 sink.diagnostic=[&](const spdif::Diagnostic& d){require(++result.events<100000);require(d.end>=d.start);mix(d.start);mix(d.end);for(unsigned char c:d.code)mix(c);};
 sink.content=[&](const spdif::ContentInfo& c){require(++result.events<100000);require(c.end>=c.start);mix(uint64_t(c.format));mix(c.pc);mix(c.pd);};
 sink.burst=[&](const spdif::BurstEvent& b){require(++result.events<100000);
  if(b.phase==spdif::BurstPhase::Begin){require(!stream);stream=true;word_index=0;}
  else if(b.phase==spdif::BurstPhase::Word){require(stream&&b.index==word_index++&&b.valid_bits>0&&b.valid_bits<=16);}
  else {require(stream);stream=false;}
  mix(uint64_t(b.phase));mix(b.word);mix(b.valid_bits);
 };
 spdif::Decoder decoder({double(capture),0},sink);
 const auto edge=[&](uint64_t n){
  const bool invalid=have_edge&&n<=last_edge;
  try{decoder.edge(n);require(!invalid);}catch(const std::invalid_argument&){require(invalid);result.rejected=true;return false;}
  last_edge=n;have_edge=true;return true;
 };
 const unsigned mode=data[0]&3;
 if(mode==0||mode==2){
  edge(at);
  for(size_t i=1;i<size;++i){const auto delta=mode==2?data[i]:unsigned(data[i])+1;at+=delta;if(!edge(at))return result;}
 }else{
  // Four input bytes produce one complete wire subframe. This compression lets
  // <=4096 bytes reach full status blocks and transport headers, rather than
  // spending the entire fuzz budget searching random edge noise.
  size_t frame=0;
  for(size_t i=1;i+3<size;i+=4,++frame){
   uint32_t raw=(uint32_t(data[i])|(uint32_t(data[i+1])<<8)|(uint32_t(data[i+2])<<16)|(uint32_t(data[i+3]&7)<<24));
   unsigned ones=0;for(unsigned bit=0;bit<27;++bit)ones+=(raw>>bit)&1;raw|=((ones&1)^unsigned(bool(data[i+3]&128)))<<27;
   const unsigned ch=unsigned(frame%2),position=unsigned((frame/2)%192);
   const unsigned b[]={0,3,4,5},m[]={0,3,6,7},w[]={0,3,5,6};const auto* pre=ch?w:position?m:b;
   const uint64_t base=at;for(unsigned j=0;j<4;++j)edge(base+8*pre[j]);
   for(unsigned bit=0;bit<28;++bit){
    if(!(mode==3&&bit==3&&(data[i+3]&64)))edge(base+8*(8+2*bit));
    if((raw>>bit)&1)edge(base+8*(9+2*bit));
   }
   at+=512;
  }
  edge(at);
 }
 decoder.finish(have_edge?last_edge+1:at);require(!stream);return result;
}
}
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data,size_t size){
 if(size>4096)return 0;
 const auto first=run(data,size),second=run(data,size);
 require(first.digest==second.digest&&first.frames==second.frames&&first.events==second.events&&first.rejected==second.rejected);
 return 0;
}
