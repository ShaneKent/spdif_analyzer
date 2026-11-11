#pragma once
#include "Fixtures.h"
#include <functional>
namespace fixture {
struct ContentResults {
 std::vector<spdif::ContentInfo> content;
 std::vector<spdif::Subframe> frames;
 std::vector<spdif::Diagnostic> diagnostics;
};
inline ContentResults decode_content(const Signal& signal) {
 ContentResults result;
 spdif::Sink sink;
 sink.subframe=[&](const spdif::Subframe& f){result.frames.push_back(f);};
 sink.diagnostic=[&](const spdif::Diagnostic& d){result.diagnostics.push_back(d);};
 sink.content=[&](const spdif::ContentInfo& c){result.content.push_back(c);};
 spdif::Decoder decoder({signal.capture,0},sink);
 for(auto edge:signal.edges)decoder.edge(edge);
 decoder.finish(signal.edges.empty()?0:signal.edges.back());
 return result;
}
// Synthetic transport fixtures exercise burst framing and payload extent. They
// do not claim compressed payload CRC/codec validity, which is outside scope.
inline Signal content_signal(uint16_t pc,uint16_t pd,unsigned payload_words,
                             int status_mode=1,unsigned start_frame=220,
                             unsigned total_frames=400,bool wrong_order=false,bool bad_header_parity=false,unsigned pcm_switch_frame=UINT32_MAX) {
 Signal signal(100000000,48000);
 for(unsigned frame=0;frame<total_frames;++frame)for(unsigned channel=0;channel<2;++channel){
  unsigned index=2*frame+channel;
  unsigned start=2*start_frame;
  uint16_t word=0;
  if(index==start)word=wrong_order?0x4e1f:0xf872;
  else if(index==start+1)word=wrong_order?0xf872:0x4e1f;
  else if(index==start+2)word=pc;
  else if(index==start+3)word=pd;
  else if(index>=start+4&&index<start+4+payload_words)
   word=index==start+4?0x0b77:uint16_t((index-start)*0x1234u);
  bool nonaudio=(status_mode==1||(status_mode==2&&channel==0))&&frame<pcm_switch_frame;
  bool c=nonaudio&&frame%192==1;
  uint32_t encoded=raw(uint32_t(word)<<8,false,false,c);
  if(bad_header_parity&&index==start+2)encoded^=1u<<27;
  signal.subframe(channel?'W':frame%192?'M':'B',encoded);
 }
 return signal;
}
template<typename Check> void content_tests(Check check) {
 using F=spdif::ContentFormat;
 const auto has=[&](const ContentResults&r,F format,bool complete=true){return std::any_of(r.content.begin(),r.content.end(),[&](const spdif::ContentInfo&c){return c.format==format&&(!complete||c.complete);});};
 const auto codec=[](F f){return f==F::Ac3||f==F::Dts1||f==F::Dts2||f==F::Dts3||f==F::Eac3;};
 const uint16_t types[]={1,0x0b,0x0c,0x0d};
 const F formats[]={F::Ac3,F::Dts1,F::Dts2,F::Dts3};
 for(unsigned n=0;n<4;++n){auto signal=content_signal(types[n],1024,64);auto r=decode_content(signal);
  check(has(r,formats[n]),"recognized complete IEC61937 AC3/DTS burst");
  auto found=std::find_if(r.content.begin(),r.content.end(),[&](const spdif::ContentInfo&c){return c.format==formats[n]&&c.complete;});
  check(found!=r.content.end()&&found->burst&&found->integrity,"complete burst integrity");
  check(found->pc==types[n]&&found->pd==1024,"Pc and bit length preserved");
  check(!found->evidence.empty(),"classification evidence available");
  check(found->end>=found->start,"content event extent monotonic");
  check(std::any_of(r.frames.begin(),r.frames.end(),[&](const spdif::Subframe& f){return f.content==formats[n];}),"recognized codec reaches subframe labels");
 }
 auto bsmod=decode_content(content_signal(0x101,1024,64));
 check(has(bsmod,F::Ac3),"AC3 Pc bit8 is type-dependent metadata, not error");
 auto error=decode_content(content_signal(0x81,1024,64));
 check(!std::any_of(error.content.begin(),error.content.end(),[&](const spdif::ContentInfo&c){return codec(c.format)&&c.complete&&c.integrity;}),"Pc bit7 error prevents trusted codec claim");
 auto parity=decode_content(content_signal(1,1024,64,1,220,400,false,true));
 check(!std::any_of(parity.content.begin(),parity.content.end(),[&](const spdif::ContentInfo&c){return codec(c.format)&&c.complete&&c.integrity;}),"corrupted header parity prevents trusted codec");
 auto nullburst=decode_content(content_signal(0,0,0));
 check(has(nullburst,F::Null,false),"null burst identified without claiming codec");
 auto pause=decode_content(content_signal(3,0,0));
 check(has(pause,F::Pause,false),"pause burst identified without claiming codec");
 check(!std::any_of(error.frames.begin(),error.frames.end(),[&](const spdif::Subframe&f){return codec(f.content);}),"error burst cannot label subframes as trusted codec");
 check(!std::any_of(parity.frames.begin(),parity.frames.end(),[&](const spdif::Subframe&f){return codec(f.content);}),"bad parity header cannot label subframes as trusted codec");
 auto unknown=decode_content(content_signal(0x33,1024,64));
 check(has(unknown,F::Unsupported,false),"extended unknown datatype preserved as unsupported");
 auto eac3=decode_content(content_signal(0x15,128,64));
 check(has(eac3,F::Eac3),"EAC3 Pd byte count handled separately");
 auto zero=decode_content(content_signal(1,0,0));
 check(!has(zero,F::Ac3),"zero length AC3 not a complete identified burst");
 auto odd=decode_content(content_signal(1,1023,64));
 check(has(odd,F::Ac3),"non-word-aligned Pd uses ceil(bits/16) transport words");
 auto oversized=decode_content(content_signal(0x0b,65535,64));
 check(!has(oversized,F::Dts1),"DTS-I oversized payload cannot become complete");
 auto truncated=decode_content(content_signal(1,1024,10,1,220,228));
 check(!has(truncated,F::Ac3),"truncated payload not labeled complete");
 check(std::any_of(truncated.content.begin(),truncated.content.end(),[](const spdif::ContentInfo&c){return c.burst&&!c.complete;}),"truncated burst evidence retained");
 auto early=decode_content(content_signal(1,1024,64,1,20,100));
 check(has(early,F::Provisional,false),"header before verified channel status is provisional");
 check(!has(early,F::Ac3),"unverified status does not produce trusted codec");
 auto pcm=decode_content(content_signal(1,1024,64,0));
 check(has(pcm,F::Pcm,false),"verified PCM status identified");
 check(has(pcm,F::Conflict,false),"PCM status versus complete burst header diagnosed as conflict");
 check(!has(pcm,F::Ac3),"PCM lookalike cannot become trusted AC3");
 check(!std::any_of(pcm.frames.begin(),pcm.frames.end(),[&](const spdif::Subframe&f){return codec(f.content);}),"PCM conflict never labels subframes as compressed codec");
 check(std::any_of(pcm.frames.begin(),pcm.frames.end(),[](const spdif::Subframe&f){return f.content==F::Pcm;}),"confirmed PCM reaches subframe label");
 auto damaged_pcm_signal=content_signal(1,1024,64,0,220,400,true,true);
 auto damaged_pcm=decode_content(damaged_pcm_signal);
 uint64_t parity_at=damaged_pcm_signal.expected[2*221].start;
 check(std::any_of(damaged_pcm.frames.begin(),damaged_pcm.frames.end(),[](const spdif::Subframe& f){return f.content==F::Pcm;}),"PCM established before parity damage");
 check(std::none_of(damaged_pcm.frames.begin(),damaged_pcm.frames.end(),[&](const spdif::Subframe& f){return f.start>=parity_at&&f.content==F::Pcm;}),"parity damage clears stale PCM until fresh complete status");
 auto changed=decode_content(content_signal(1,1024,64,1,220,600,false,false,384));
 check(has(changed,F::Ac3),"format change fixture initially identifies AC3");
 check(!changed.frames.empty()&&changed.frames.back().content==F::Pcm,"complete changed block identifies PCM");
 auto changed_signal=content_signal(1,1024,64,1,220,600,false,false,384);
 uint64_t conflict_start=changed_signal.expected[2*385].start;
 check(std::none_of(changed.frames.begin(),changed.frames.end(),[&](const spdif::Subframe&f){return f.start>=conflict_start&&codec(f.content);}),"current status mismatch clears stale codec immediately");
 auto expired=decode_content(content_signal(1,1024,64,1,220,7000));
 check(has(expired,F::Ac3),"expiry fixture initially identifies AC3");
 check(!expired.frames.empty()&&expired.frames.back().content==F::NonPcm,"missing fresh bursts expires stale codec to nonPCM");
 auto swapped=decode_content(content_signal(1,1024,64,1,220,400,true));
 check(!has(swapped,F::Ac3),"Pa/Pb swapped channel order rejected");
 auto disagree=decode_content(content_signal(1,1024,64,2));
 check(!has(disagree,F::Ac3),"disagreeing channel status cannot produce trusted codec");
 // A gap must invalidate prior classification/status evidence before reacquiring.
 auto before=content_signal(1,1024,64);auto after=content_signal(0,0,0,0,20,100,true);
 auto combined=before;uint64_t shift=before.edges.back()+1000000-after.edges.front();
 for(auto e:after.edges)combined.edges.push_back(e+shift);
 auto reset=decode_content(combined);
 check(has(reset,F::Ac3),"pre-gap AC3 identified");
 check(!reset.frames.empty()&&reset.frames.back().content!=F::Ac3,"gap clears stale codec label");
}
}
