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
fixture::Signal signal(const Profile& profile,bool nonaudio,bool inverted,unsigned burst_count=2,unsigned nonaudio_from_frame=0){
 fixture::Signal result(100000000,48000,inverted);
 const unsigned first=220,total=first+(burst_count-1)*profile.period+profile.pd/32+32;
 for(unsigned frame=0;frame<total;++frame)for(unsigned channel=0;channel<2;++channel){
  const auto& c=nonaudio&&frame>=nonaudio_from_frame?nonaudio_status:pcm_status;
  uint16_t word=0;
  for(unsigned burst=0;burst<burst_count;++burst){
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
// Explicitly synthetic loss/recovery coverage. This does not represent a
// physical cable operation: remove part of a deterministic stream, resume in a
// later subframe, and insert an exactly200ms separation between retained edges.
void synthetic_reconnect_profiles(){
 using F=spdif::ContentFormat;
 constexpr uint64_t gap_samples=20000000; //100MHz *200ms, independently fixed.
 for(const auto& profile:profiles)for(bool inverted:{false,true})for(bool prior_pcm:{false,true}){
  const unsigned interrupted_start=2*(220+profile.period);
  const unsigned cut_subframe=interrupted_start+4+17;
  const unsigned resume_subframe=cut_subframe+74; //37frames later, same channel.
  const auto full=signal(profile,true,inverted,4,prior_pcm?resume_subframe/2:0);
  const auto middle=[&](unsigned index){
   const auto& bounds=full.expected[index].field_boundaries;
   return bounds[1]+(bounds[3]-bounds[1])/2;
  };
  const auto cut=std::lower_bound(full.edges.begin(),full.edges.end(),middle(cut_subframe));
  const auto resume=std::lower_bound(full.edges.begin(),full.edges.end(),middle(resume_subframe));
  check(cut!=full.edges.begin()&&resume!=full.edges.end(),"Synthetic cut/resume edges exist");
  check(*cut>full.expected[cut_subframe].field_boundaries[1]&&*cut<full.expected[cut_subframe].field_boundaries[3],"Loss begins inside audio payload bits of an unfinished BMC subframe");
  check(*resume>full.expected[resume_subframe].start&&*resume<full.expected[resume_subframe].field_boundaries[3],"Return begins mid-subframe rather than manufacturing a preamble");
  check((resume_subframe/2)%192!=0,"Return starts mid channel-status block");
  const uint64_t last_pre=*(cut-1),first_post=last_pre+gap_samples,shift=first_post-*resume;
  auto interrupted=full;
  interrupted.edges.assign(full.edges.begin(),cut);
  for(auto at=resume;at!=full.edges.end();++at)interrupted.edges.push_back(*at+shift);
  const auto split=size_t(cut-full.edges.begin());
  check(interrupted.edges[split]-interrupted.edges[split-1]==gap_samples,"Exactly200ms between adjacent actual retained edges");
  for(size_t i=0;i<split;++i)check(interrupted.edges[i]==full.edges[i],"Prefix transitions are unchanged");
  for(size_t i=split;i<interrupted.edges.size();++i)check(interrupted.edges[i]-shift==*(resume+(i-split)),"Suffix transition intervals and payloads are unchanged");
  const auto decoded=decode_transport(interrupted);
  check(decoded.begin.size()==4&&decoded.end.size()==4,"Each observed header begins and ends exactly one burst despite loss");
  check(decoded.payloads.size()==4,"Aborted payload remains a separate incomplete record");
  check(decoded.end[1].info.start==full.expected[interrupted_start].start&&decoded.end[1].declared_bits==profile.pd,"Aborted event retains exact original header timestamp and declared bit length");
  check(!decoded.end[1].info.complete&&!decoded.end[1].info.integrity&&decoded.end[1].info.end<=last_pre,"Gap terminates open burst without guessed continuation or accepted integrity");
  check(decoded.payloads[1].size()==17,"Only17 whole payload words precede the independently selected mid-word cut");
  for(unsigned i=0;i<17;++i)check(decoded.payloads[1][i]==test_word(i,profile.pc),"Aborted burst preserves its exact newly generated prefix");
  unsigned accepted=0;
  for(unsigned burst=0;burst<4;++burst){
   const auto& end=decoded.end[burst];
   if(!end.info.complete||!end.info.integrity||end.info.format!=profile.format)continue;
   ++accepted;
   check(burst!=1&&end.info.format==profile.format,"Only pre-gap and fresh post-gap complete bursts are trusted");
   check(!(end.info.start<first_post&&end.info.end>last_pre),"No accepted payload spans the missing source interval");
   check(decoded.payloads[burst].size()==profile.pd/16,"Recovered burst has exact declared payload extent");
   for(unsigned word=0;word<decoded.payloads[burst].size();++word)check(decoded.payloads[burst][word]==test_word(word,profile.pc),"Recovered payload matches independent deterministic word oracle");
   const unsigned source_frame=220+burst*profile.period;
   const uint64_t expected_start=full.expected[2*source_frame].start+(burst>1?shift:0);
   check(end.info.start==expected_start,"Accepted bursts retain exact pre/post source timestamps");
   const unsigned exclusive_end=2*source_frame+4+profile.pd/16;
   check(end.info.end+1==full.expected[exclusive_end].start+(burst>1?shift:0),"Accepted payload ends exactly at its final declared word boundary");
  }
  check(accepted==(prior_pcm?2u:3u),"Only corroborated pre-gap and two fresh post-gap bursts accepted");
  check(decoded.end[0].info.format==(prior_pcm?F::Conflict:profile.format),"Prior PCM declaration stays a conflict rather than accepted compressed content");
  const unsigned fresh_block_frame=((resume_subframe/2)/192+1)*192;
  const uint64_t fresh_start=full.expected[2*fresh_block_frame].start+shift;
  const uint64_t confirmed_at=full.expected[2*(fresh_block_frame+192)].start+shift;
  std::array<bool,2> fresh{};
  for(const auto& block:decoded.status){
   check(block.integrity&&block.c==(prior_pcm&&block.start<first_post?pcm_status:nonaudio_status),"Every completed status block retains exact parity-good pre/post consumer declaration");
   check(!(block.start<first_post&&block.end>last_pre),"No completed status block bridges the interruption");
   if(block.start==fresh_start){check(block.channel<2&&block.end+1==confirmed_at,"Fresh status block covers exactly192new frames before confirmation");fresh[block.channel]=true;}
  }
  check(fresh[0]&&fresh[1],"Both channels re-establish complete status from the first fresh B boundary");
  bool saw_pre=false,saw_pre_pcm=false,saw_post=false,saw_unknown=false,saw_recovered=false;
  uint64_t first_recovered=0;
  for(const auto& f:decoded.frames){
   check(!(f.start<first_post&&f.end>last_pre),"No decoded subframe spans the synthetic edge gap");
   check(f.parity_ok&&std::abs(f.sample_rate-48000)<960,"Reacquired subframes retain parity and48k transport timing");
   const bool post=f.start>=first_post;
   const uint64_t source_start=f.start-(post?shift:0);
   const auto expected=std::lower_bound(full.expected.begin(),full.expected.end(),source_start,[](const fixture::Expected& value,uint64_t at){return value.start<at;});
   check(expected!=full.expected.end()&&expected->start==source_start,"Every emitted subframe has a real retained encoder boundary");
   check(expected->raw==f.raw&&expected->preamble==f.preamble,"No invented or rewritten payload/control bits during recovery");
   if(!post){saw_pre=true;saw_pre_pcm|=f.content==F::Pcm;continue;}
   if(!saw_post)first_recovered=f.start;
   saw_post=true;
   check(f.content!=F::Pcm&&!f.layout.known,"Non-audio recovery never gains stale PCM trust or invented precision");
   if(f.start<confirmed_at){check(f.content==F::Unknown,"Prior non-audio/codec status is cleared until both fresh status blocks are confirmed");saw_unknown=true;}
   if(f.content==profile.format){check(f.start>=confirmed_at,"Trusted compressed identity follows fresh channel-status confirmation");saw_recovered=true;}
  }
  check(saw_pre&&saw_post&&saw_unknown&&saw_recovered,"Fixture exercises prior trust, unknown reacquisition and fresh trusted codec");
  check(saw_pre_pcm==prior_pcm,"Prior-PCM variant actually establishes PCM classification before interruption");
  check(first_recovered-first_post<8*100000000u/96000u,"Acquisition recovers within eight subframes of midstream return");
  check(std::any_of(decoded.diagnostics.begin(),decoded.diagnostics.end(),[&](const spdif::Diagnostic& d){return d.code=="sync_loss"&&d.start<=last_pre&&d.end>=last_pre;}),"Actual interior signal loss is diagnosed at the last retained edge");
  check(std::none_of(decoded.diagnostics.begin(),decoded.diagnostics.end(),[](const spdif::Diagnostic& d){return d.code=="parity"||d.code=="burst_early"||d.code=="burst_missing"||d.code=="burst_late";}),"Loss resets cadence instead of reporting guessed missing bursts or damaged retained parity");
  auto prefix=full;prefix.edges.assign(full.edges.begin(),cut);
  const auto truncated=decode_transport(prefix);
  check(truncated.end.size()==2&&truncated.end[0].info.complete&&truncated.end[0].info.integrity,"Capture-prefix oracle retains exactly one prior complete burst");
  check(!truncated.end[1].info.complete&&!truncated.end[1].info.integrity&&truncated.payloads[1].size()==17,"Quiet EOF at the same cut cannot complete the interrupted burst");
 }
}
}
int main(){try{status_literals();transport_profiles();synthetic_reconnect_profiles();std::cout<<"PASS "<<checks<<" sanitized hardware-regression checks\n";return 0;}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1;}}
