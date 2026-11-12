#include "Fixtures.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

// Sanitized transport regression derived from measured D6/D7 status bytes,
// Pc/Pd and cadence. The payload words below are newly generated test data;
// none are copied from private captures or compressed source media.
namespace {
unsigned checks=0;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
constexpr std::array<uint8_t,24> nonaudio_status{{0x06,0x12,0x00,0x02}};
constexpr std::array<uint8_t,24> pcm_status{{0x04,0x12,0x00,0x02}};
struct Profile {uint16_t pc,pd;unsigned period;spdif::ContentFormat format;};
constexpr Profile profiles[]={{1,6144,1536,spdif::ContentFormat::Ac3},{11,8192,512,spdif::ContentFormat::Dts1}};
uint16_t test_word(unsigned index,unsigned type){return uint16_t(0x4000u|((index*73u+type*29u)&0x3fffu));}
fixture::Signal signal(const Profile& profile,bool nonaudio,bool inverted){
 fixture::Signal result(100000000,48000,inverted);
 const auto& c=nonaudio?nonaudio_status:pcm_status;
 const unsigned first=220,total=first+profile.period+profile.pd/32+32;
 for(unsigned frame=0;frame<total;++frame)for(unsigned channel=0;channel<2;++channel){
  uint16_t word=0;
  for(unsigned burst=0;burst<2;++burst){
   const unsigned begin=2*(first+burst*profile.period),position=2*frame+channel;
   if(position==begin)word=0xf872;
   else if(position==begin+1)word=0x4e1f;
   else if(position==begin+2)word=profile.pc;
   else if(position==begin+3)word=profile.pd;
   else if(position>=begin+4&&position<begin+4+profile.pd/16)word=test_word(position-begin-4,profile.pc);
  }
  const unsigned bit=frame%192;
  const bool status=(c[bit/8]>>(bit%8))&1u;
  result.subframe(channel?'W':bit?'M':'B',fixture::raw(uint32_t(word)<<8,false,false,status));
 }
 return result;
}
struct Result {
 std::vector<spdif::Subframe> frames;
 std::vector<spdif::StatusBlock> status;
 std::vector<spdif::Diagnostic> diagnostics;
 std::vector<spdif::BurstEvent> begin,end;
 std::vector<std::vector<uint16_t>> payloads;
};
Result decode_transport(const fixture::Signal& input){
 Result result;std::vector<uint16_t> pending;
 spdif::Sink sink;
 sink.subframe=[&](const spdif::Subframe& f){result.frames.push_back(f);};
 sink.status=[&](const spdif::StatusBlock& b){result.status.push_back(b);};
 sink.diagnostic=[&](const spdif::Diagnostic& d){result.diagnostics.push_back(d);};
 sink.burst=[&](const spdif::BurstEvent& b){
  if(b.phase==spdif::BurstPhase::Begin){result.begin.push_back(b);pending.clear();}
  else if(b.phase==spdif::BurstPhase::Word){check(b.valid_bits==16,"Observed profiles have whole16bit payload words");check(b.index==pending.size(),"Payload word index remains sequential");pending.push_back(b.word);}
  else{result.end.push_back(b);result.payloads.push_back(pending);}
 };
 spdif::Decoder decoder({input.capture,0},sink);
 for(auto edge:input.edges)decoder.edge(edge);
 decoder.finish(input.edges.back());return result;
}
void status_literals(){
 unsigned changes=0;
 for(unsigned byte=0;byte<24;++byte){const unsigned difference=nonaudio_status[byte]^pcm_status[byte];changes+=difference!=0;if(byte==0)check(difference==2,"Observed C-byte0 bit1 is the sole declaration difference");else check(!difference,"Observed remaining191status bits are identical");}
 check(changes==1,"Exactly one observed status byte differs");
 for(const auto& bytes:{nonaudio_status,pcm_status}){
  const auto info=spdif::status_info(bytes);
  check(info.supported&&!info.professional&&info.audio_fields_reliable,"Observed status is supported consumer evidence");
  check(info.sample_rate==48000&&info.stereo,"Observed status declares48k consumer channels");
  check(!info.layout.known&&info.word_length==0,"Observed unspecified precision must remain unknown");
  check(info.non_audio==bool(bytes[0]&2),"Literal bit1 governs audio/non-audio evidence");
 }
}
void transport_profiles(){
 using F=spdif::ContentFormat;
 for(const auto& profile:profiles)for(bool inverted:{false,true}){
  const auto a=decode_transport(signal(profile,true,inverted)),b=decode_transport(signal(profile,false,inverted));
  check(a.frames.size()==b.frames.size()&&!a.frames.empty(),"Both observed status variants retain same number of payload containers");
  for(size_t i=0;i<a.frames.size();++i){
   const auto& left=a.frames[i];const auto& right=b.frames[i];
   check(left.start==right.start&&left.end==right.end,"Sanitized scenarios share source timing");
   check((left.raw&0xffffffu)==(right.raw&0xffffffu),"Status declaration never rewrites lower24payload bits");
   check(left.parity_ok&&right.parity_ok&&!left.validity&&!right.validity&&!left.user&&!right.user,"Observed V/U0 and valid even parity retained");
   check(!left.layout.known&&!right.layout.known,"Unspecified precision never becomes known PCM precision");
   check(std::abs(left.sample_rate-48000)<960,"100MHz sampled timing recovers48k transport");
  }
  check(!a.status.empty()&&a.status.size()==b.status.size(),"Both scenarios establish complete per-channel status");
  for(size_t i=0;i<a.status.size();++i){check(a.status[i].c==nonaudio_status&&b.status[i].c==pcm_status,"Exact observed192bit C blocks roundtrip");check(a.status[i].integrity&&b.status[i].integrity,"Both status declarations are parity-good, independent of semantic conflict");}
  check(a.begin.size()==2&&b.begin.size()==2&&a.end.size()==2&&b.end.size()==2,"Two bounded repeated bursts remain observable");
  check(a.payloads==b.payloads,"Identical newly generated payloads remain available under either status declaration");
  for(unsigned burst=0;burst<2;++burst){
   check(a.begin[burst].info.pc==profile.pc&&a.begin[burst].info.pd==profile.pd,"Observed Pc/Pd remain exact");
   check(a.end[burst].info.complete&&a.end[burst].info.integrity&&a.end[burst].info.format==profile.format,"Non-audio status corroborates complete transport identification");
   check(b.end[burst].info.complete&&b.end[burst].info.format==F::Conflict,"Identical payload under PCM declaration remains explicit conflict");
   check(a.payloads[burst].size()==profile.pd/16,"Observed bit length bounds payload word count");
   for(unsigned word=0;word<a.payloads[burst].size();++word)check(a.payloads[burst][word]==test_word(word,profile.pc),"Independent deterministic payload oracle");
  }
  const auto measured=a.begin[1].info.start-a.begin[0].info.start;
  const double expected=profile.period*100000000.0/48000;
  check(std::abs(double(measured)-expected)<=1,"Nominal profile cadence maps to fractional100MHz timestamps");
  check(std::any_of(a.frames.begin(),a.frames.end(),[&](const spdif::Subframe& f){return f.content==profile.format;}),"Corroborated codec transport identity reaches subframes");
  check(std::any_of(b.frames.begin(),b.frames.end(),[](const spdif::Subframe& f){return f.content==F::Conflict;}),"Contradictory PCM declaration reaches subframe evidence");
  check(std::none_of(b.frames.begin(),b.frames.end(),[&](const spdif::Subframe& f){return f.content==profile.format;}),"PCM-conflict path never promotes trusted compressed format");
  for(const auto* result:{&a,&b})for(const auto& d:result->diagnostics)check(d.code=="incomplete_status"||d.code=="truncated","Steady sanitized transport has no parity/sync/order/cadence faults");
 }
}
}
int main(){try{status_literals();transport_profiles();std::cout<<"PASS "<<checks<<" sanitized hardware-regression checks\n";return 0;}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1;}}
