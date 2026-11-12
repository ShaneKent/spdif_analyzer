#include "Decoder.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace spdif {
namespace {
unsigned ones(uint32_t v) { unsigned n=0; for(;v;v&=v-1) ++n; return n; }
std::string hex(const std::array<uint8_t,24>& v) {
 std::ostringstream s; s<<std::hex<<std::setfill('0'); for(auto b:v)s<<std::setw(2)<<unsigned(b); return s.str();
}
}
const char* content_name(ContentFormat f) {
 switch(f){case ContentFormat::Pcm:return "PCM";case ContentFormat::NonPcm:return "Non-PCM (unidentified)";
 case ContentFormat::Ac3:return "AC-3 (Dolby Digital)";case ContentFormat::Dts1:return "DTS type I";
 case ContentFormat::Dts2:return "DTS type II";case ContentFormat::Dts3:return "DTS type III";
 case ContentFormat::Eac3:return "E-AC-3 (Dolby Digital Plus)";case ContentFormat::Null:return "IEC 61937 null";
 case ContentFormat::Pause:return "IEC 61937 pause";case ContentFormat::Unsupported:return "IEC 61937 unsupported type";
 case ContentFormat::Provisional:return "Provisional IEC 61937 (status unconfirmed)";
 case ContentFormat::Conflict:return "Conflict: PCM status / IEC 61937 header";default:return "Unknown";}
}
struct Decoder::Impl {
 Config cfg; Sink sink; std::deque<uint64_t> edges;
 bool locked=false, have_pending=false, block=false, finished=false, have_last=false, input_seen=false;
 Subframe pending{}, last{}; unsigned block_count=0; StatusBlock blocks[2];
 uint64_t last_input=0, last_noise_report=0; double half=0;
 ContentFormat content=ContentFormat::Unknown;
 int content_mode=-1; bool known_professional=false;
 std::array<std::array<uint8_t,24>,2> known_status{};
 std::array<SampleLayout,2> known_layouts{};
 unsigned burst_stage=0,remaining_words=0,burst_age=0;
 ContentInfo burst_info{}; ContentFormat burst_type=ContentFormat::Unknown;
 bool burst_valid=false,stream_open=false,payload_supported=false;
 uint32_t declared_bits=0,remaining_bits=0;uint64_t payload_index=0,word_ordinal=0,burst_ordinal=0;
 struct CadenceHistory {bool known=false;unsigned type=0;uint64_t ordinal=0,sample=0;};
 std::array<CadenceHistory,8> cadence_history{};
 uint64_t overlap_sample=0;
 unsigned overlap_stage=0;uint16_t overlap_pc=0;
 ContentFormat base_content() const {return content_mode==0?ContentFormat::Pcm:content_mode==1?ContentFormat::NonPcm:ContentFormat::Unknown;}
 void content_event(const ContentInfo& info){if(sink.content)sink.content(info);}
 void stream_event(BurstPhase phase,uint16_t word=0,uint8_t valid_bits=0) {
  if(!sink.burst)return;
  BurstEvent event;event.phase=phase;event.info=burst_info;event.index=payload_index;
  event.word=word;event.valid_bits=valid_bits;event.declared_bits=declared_bits;event.supported=payload_supported;
  sink.burst(event);
 }
 void end_stream(){if(stream_open){stream_event(BurstPhase::End);stream_open=false;}}
 std::optional<BurstEvent> pending_header() const {
  if(finished || burst_stage<1 || burst_stage>3 || stream_open)return std::nullopt;
  BurstEvent event;event.phase=BurstPhase::Begin;event.info=burst_info;
  event.info.format=ContentFormat::Unknown;event.info.burst=true;
  event.info.complete=false;event.info.integrity=false;
  event.info.evidence="Unconfirmed/incomplete IEC 61937 header candidate: known_header_words="+std::to_string(burst_stage)+" of 4; missing Pc/Pd values are undecoded placeholders; pending live continuation";
  return event;
 }
 void label(ContentFormat next,uint64_t at,const std::string& why) {
  if(next==content)return;
  content=next;ContentInfo info;info.start=info.end=at;info.format=next;info.evidence=why;
  info.integrity=content_mode>=0;content_event(info);
 }
 void reset_cadence(){cadence_history={};overlap_stage=0;}
 void cancel_burst(uint64_t at,const char* why) {
  if(burst_stage){burst_info.end=at;burst_info.complete=false;burst_info.integrity=false;
   if(!stream_open){
    // A partial header has no verified Pc/Pd extent. Preserve the forensic
    // candidate without ever guessing units, producing payload, or trusting it.
    burst_info.format=ContentFormat::Unknown;
    burst_info.evidence="Unconfirmed/incomplete IEC 61937 header candidate: known_header_words="+std::to_string(burst_stage)+" of 4; missing Pc/Pd values are undecoded placeholders; "+why;
    declared_bits=0;payload_supported=false;payload_index=0;stream_open=true;
    stream_event(BurstPhase::Begin);
   }else burst_info.evidence=why;
   end_stream();content_event(burst_info);}
  else end_stream();
  burst_stage=remaining_words=0;burst_valid=false;burst_age=0;overlap_stage=0;
 }
 void unknown_content(uint64_t at,const char* why) {
  cancel_burst(at,why);content_mode=-1;known_layouts={};known_professional=false;
  reset_cadence();label(ContentFormat::Unknown,at,why);
 }
 void confirmed_status(uint64_t at) {
  const auto a=status_info(blocks[0].c),b=status_info(blocks[1].c);
  int mode=-1;
  if(blocks[0].integrity&&blocks[1].integrity && a.supported && b.supported && a.professional==b.professional
    && a.non_audio==b.non_audio && (!a.professional || a.channel_mode==b.channel_mode))mode=a.non_audio?1:0;
  const bool changed_prof=mode>=0 && a.professional!=known_professional;
  if(mode!=content_mode || changed_prof){cancel_burst(at,"Channel status changed");reset_cadence();content_mode=mode;
   label(base_content(),at,"Confirmed complete, parity-good supported per-channel status blocks; professional CRC status is reported separately");}
  known_professional=mode>=0 && a.professional;
  if(mode>=0){known_status[0]=blocks[0].c;known_status[1]=blocks[1].c;known_layouts={a.layout,b.layout};}
  else known_layouts={};
 }
 ContentFormat burst_label() const {
  if(content_mode==0)return ContentFormat::Conflict;
  if(content_mode!=1)return ContentFormat::Provisional;
  if(known_professional)return ContentFormat::Unsupported; // SMPTE and IEC type tables differ.
  return burst_type;
 }
 static unsigned period(unsigned type) {
  switch(type){case 1:return 1536;case 11:return 512;case 12:return 1024;case 13:return 2048;case 21:return 6144;default:return 0;}
 }
 void cadence(unsigned type) {
  const unsigned stream=burst_info.pc>>13;
  const unsigned expected=type==21?0:period(type);
  if(content_mode!=1 || known_professional){reset_cadence();return;}
  auto& history=cadence_history[stream];
  if(!expected || !burst_valid){history={};return;}
  if(history.known && history.type==type){
   const uint64_t observed=(burst_ordinal-history.ordinal)/2;
   burst_info.evidence+="; cadence frames observed="+std::to_string(observed)+" nominal="+std::to_string(expected);
   if(observed<expected)diag(history.sample,burst_info.start,"burst_early","IEC 61937 burst earlier than nominal period: "+burst_info.evidence);
   else if(observed>=2*expected)diag(history.sample,burst_info.start,"burst_missing","IEC 61937 interval missed "+std::to_string(observed/expected-1)+" nominal burst periods (not proof of lost audio): "+burst_info.evidence);
   else if(observed>expected)diag(history.sample,burst_info.start,"burst_late","IEC 61937 burst later than nominal period: "+burst_info.evidence);
  }
  history.known=true;history.type=type;history.ordinal=burst_ordinal;history.sample=burst_info.start;
 }
 void overlap_probe(const Subframe& f,uint16_t word) {
  // A shaped header can legally occur inside compressed data. It is only a
  // candidate notification: never split, truncate, or reinterpret the payload.
  if(overlap_stage==1){if(f.channel==1&&word==0x4e1f){overlap_stage=2;return;}overlap_stage=0;}
  else if(overlap_stage==2){if(f.channel==0){overlap_pc=word;overlap_stage=3;return;}overlap_stage=0;}
  else if(overlap_stage==3){
   unsigned type=overlap_pc&0x7f;unsigned p=period(type);unsigned bits=type==21?unsigned(word)*8:word;
   if(f.channel==1 && p && !(overlap_pc&0x80) && bits && bits<=p*32-64 && content_mode==1 && !known_professional)
    diag(overlap_sample,f.end,"burst_overlap_candidate","Header-shaped words inside active "+std::string(content_name(burst_type))+" payload: candidate_type="+std::to_string(type)+" stream="+std::to_string(overlap_pc>>13)+"; ambiguous with legal payload data, payload boundaries unchanged");
   overlap_stage=0;
  }
  if(f.channel==0 && word==0xf872){overlap_stage=1;overlap_sample=f.start;}
 }
 void burst_word(const Subframe& f) {
  ++word_ordinal;
  if(!f.parity_ok){unknown_content(f.start,"Parity damage invalidates content evidence");return;}
  if(burst_age && ++burst_age>12288){cancel_burst(f.start,"No fresh IEC 61937 header within supported repetition window");label(base_content(),f.start,"Expired IEC 61937 format evidence");}
  const uint16_t word=uint16_t((f.raw>>8)&0xffff);
  if(burst_stage==4){
   overlap_probe(f,word);
   const uint8_t valid=uint8_t(std::min(16u,remaining_bits));
   burst_info.end=f.end;stream_event(BurstPhase::Word,word,valid);++payload_index;remaining_bits-=valid;
   if(--remaining_words==0){burst_info.end=f.end;burst_info.complete=true;
    burst_info.format=burst_label();burst_info.integrity=burst_valid;
    burst_info.evidence=std::string(content_name(burst_type))+" payload length observed; "+(content_mode==1?"non-audio status corroborates":content_mode==0?"conflicts with PCM status":"channel status unconfirmed");
    if(!burst_valid)burst_info.evidence+="; Pc error flag is set";
    end_stream();content_event(burst_info);
    label(burst_valid?burst_label():base_content(),f.start,burst_info.evidence);burst_stage=0;overlap_stage=0;
   }
   return;
  }
  if(burst_stage==1){if(f.channel==1 && word==0x4e1f){burst_info.end=f.end;burst_stage=2;return;}burst_stage=0;}
  else if(burst_stage==2){if(f.channel==0){burst_info.pc=word;burst_info.end=f.end;burst_stage=3;return;}burst_stage=0;}
  else if(burst_stage==3){
   if(f.channel!=1){burst_stage=0;return;}
   burst_info.pd=word;burst_info.end=f.end;burst_info.burst=true;burst_info.complete=false;
   const unsigned type=burst_info.pc&0x7f;
   unsigned limit_bits=0;burst_type=ContentFormat::Unsupported;
   switch(type){case 0:burst_type=ContentFormat::Null;break;case 3:burst_type=ContentFormat::Pause;break;
    case 1:burst_type=ContentFormat::Ac3;break;case 11:burst_type=ContentFormat::Dts1;break;
    case 12:burst_type=ContentFormat::Dts2;break;case 13:burst_type=ContentFormat::Dts3;break;case 21:burst_type=ContentFormat::Eac3;break;}
   if(period(type)&&!known_professional)limit_bits=period(type)*32-64;
   const unsigned bits=type==21?unsigned(word)*8:unsigned(word);
   burst_valid=(burst_info.pc&0x80)==0;
   burst_info.format=burst_label();burst_info.integrity=burst_valid;
   burst_info.evidence=std::string("IEC 61937 header: ")+content_name(burst_type)+"; Pc="+std::to_string(burst_info.pc)+"; stream="+std::to_string(burst_info.pc>>13)+"; type="+std::to_string(type)+"; payload not yet validated";
   if(!burst_valid)burst_info.evidence+="; Pc error flag is set";
   declared_bits=limit_bits?bits:0;payload_index=0;payload_supported=limit_bits!=0;stream_open=true;
   stream_event(BurstPhase::Begin);
   if(limit_bits && bits && bits<=limit_bits)cadence(type);else reset_cadence();
   content_event(burst_info);burst_age=1;
   label(content_mode==0?ContentFormat::Conflict:content_mode==1?ContentFormat::NonPcm:ContentFormat::Provisional,f.start,burst_info.evidence);
   if(!limit_bits){
    if(type==0 && word==0 && !known_professional){burst_info.complete=true;burst_info.evidence="Null burst with zero payload length";content_event(burst_info);label(burst_label(),f.start,burst_info.evidence);}
    else if(type==3)label(burst_label(),f.start,"Pause header; type-specific payload interpretation unsupported");
    else if(type!=0)label(burst_label(),f.start,"Unsupported IEC 61937 type; payload length units not assumed");
    end_stream();burst_stage=0;return;
   }
   if(!bits || bits>limit_bits){
    burst_info.integrity=false;burst_info.evidence="Rejected IEC 61937 payload length: type="+std::to_string(type)+" stream="+std::to_string(burst_info.pc>>13)+" declared_bits="+std::to_string(bits)+" capacity_bits="+std::to_string(limit_bits);
    content_event(burst_info);diag(burst_info.start,f.end,"burst_length",burst_info.evidence);
    if(bits>limit_bits)diag(burst_info.start,f.end,"burst_overlap","Declared payload would extend beyond nominal burst period capacity: "+burst_info.evidence);
    end_stream();burst_stage=0;return;
   }
   remaining_bits=bits;remaining_words=(bits+15)/16;burst_stage=4;return;
  }
  if(f.channel==0 && word==0xf872){burst_info=ContentInfo{};burst_info.start=f.start;burst_info.end=f.end;burst_info.burst=true;burst_stage=1;burst_ordinal=word_ordinal;}
 }

 explicit Impl(Config c,Sink s):cfg(c),sink(std::move(s)) {
  if(!std::isfinite(c.capture_rate)||c.capture_rate<=0||c.capture_rate>std::numeric_limits<uint32_t>::max()||!std::isfinite(c.nominal_rate)||c.nominal_rate<0)
   throw std::invalid_argument("capture rate must be positive and within the SDK uint32 range; nominal rate must be nonnegative");
 }
 void diag(uint64_t a,uint64_t b,const char* code,const std::string& message) {
  if(sink.diagnostic)sink.diagnostic({a,b,code,message});
 }
 void partial(uint64_t end,const char* reason) {
  if(!block)return;
  unknown_content(end,reason);
  for(unsigned ch=0;ch<2;++ch) {
   unsigned count=(block_count+1-ch)/2;
   if(count)diag(blocks[ch].start,end,"incomplete_status",std::string(reason)+" channel="+std::to_string(ch)+" bits="+std::to_string(count)+" C="+hex(blocks[ch].c)+" U="+hex(blocks[ch].u));
  }
  block=false; block_count=0;
 }
 void lose(uint64_t at,const char* reason) {
  if(locked||have_pending)diag(at,at,"sync_loss",reason);
  unknown_content(at,reason);partial(at,reason); locked=false; have_pending=false; have_last=false; half=0;
 }
 bool match(double duration,unsigned cells,double t)const {
  // Independent edge quantization contributes up to one capture sample.
  const double tolerance=std::min(.49*t,.25*t+1.0);
  return std::abs(duration-cells*t)<=tolerance;
 }
 bool preamble(char& p,double& t) const {
  if(edges.size()<5)return false;
  t=double(edges[4]-edges[0])/8.;
  if(t<3.99)return false;
  double rate=cfg.capture_rate/(128*t);
  // Acquisition estimate uses only eight half cells; allow edge quantization
  // and jitter at the supported 8 kHz and 192 kHz endpoints.
  if(rate<7500||rate>205000)return false;
  if(cfg.nominal_rate && std::abs(rate/cfg.nominal_rate-1)>.06)return false;
  if(!match(double(edges[1]-edges[0]),3,t))return false;
  const unsigned patterns[3][3]={{1,1,3},{3,1,1},{2,1,2}};
  const char names[3]={'B','M','W'};
  for(unsigned k=0;k<3;++k) {
   bool ok=true; for(unsigned j=0;j<3;++j)ok&=match(double(edges[j+2]-edges[j+1]),patterns[k][j],t);
   if(ok){p=names[k];return true;}
  }
  return false;
 }
 void emit(Subframe f) {
  if(have_last && (f.channel==last.channel)) {
   diag(f.start,f.end,"channel_order","Expected alternating channel A and B subframes");
   partial(f.start,"channel order error");
  }
  if(f.preamble=='B') {
   if(block) {
    if(block_count==384) {
     for(auto &b:blocks){b.end=last.end;b.crc=status_crc(b.c);b.expected_crc=(b.c[0]&1)?professional_crc(b.c):0;b.interpretation=interpret_status(b.c);if(sink.status)sink.status(b);if(b.crc==IntegrityCheck::Failed)diag(b.start,b.end,"status_crc","Professional channel-status CRCC failed; channel="+std::to_string(b.channel)+"; parity integrity remains separate");}
     confirmed_status(f.start);
    } else partial(f.start,"early block preamble");
   }
   block=true;block_count=0;
   for(unsigned ch=0;ch<2;++ch){blocks[ch]=StatusBlock{};blocks[ch].channel=ch;blocks[ch].start=f.start;}
  }
  if(block) {
   if(block_count>=384) {
    diag(f.start,f.end,"block_length","Expected block-start B after 192 stereo frames");
    partial(f.start,"missing block preamble");
   } else if(f.channel!=(block_count%2)) {
    partial(f.start,"channel order error");
   } else {
    unsigned pos=block_count/2; f.block_position=int(pos); auto& b=blocks[f.channel];
    b.c[pos/8]|=uint8_t(f.status<<(pos%8)); b.u[pos/8]|=uint8_t(f.user<<(pos%8));
    b.integrity &= f.parity_ok; ++block_count;
    // Professional addresses/other reliability groups/CRC may change while
    // bytes0..5 remain trustworthy. Consumer status stays conservative.
    const bool audio_status_bit=!known_professional || pos<48 || pos==180;
    if(content_mode>=0 && audio_status_bit && f.parity_ok && f.status!=bool((known_status[f.channel][pos/8]>>(pos%8))&1))
     unknown_content(f.start,"Current channel status differs from previous confirmed block");
   }
  }
  if(!f.parity_ok)diag(f.start,f.end,"parity","Even parity failed over slots 4 through 31; payload retained");
  burst_word(f);f.content=content;f.layout=known_layouts[f.channel];
  if(sink.subframe)sink.subframe(f);
  last=f;have_last=true;
 }
 void accept(Subframe f,double t) {
  if(locked && half && std::abs(t/half-1)>.08)
   lose(f.start,"Transport clock changed; reacquiring");
  if(!locked) {
   if(have_pending && pending.end+1==f.start && pending.channel!=f.channel && std::abs(t/half-1)<.08) {
    locked=true; emit(pending); emit(f); have_pending=false;
   } else {
    if(have_pending && pending.end+1==f.start && pending.channel==f.channel)
     diag(f.start,f.end,"channel_order","Consecutive candidate subframes use the same channel; acquisition requires A/B alternation");
    pending=f;have_pending=true;
   }
  } else emit(f);
  half=t;
 }
 void process() {
  while(edges.size()>=5) {
   char p;double t;
   if(!preamble(p,t)) {
    if(locked||have_pending)lose(edges.front(),"Malformed preamble or interrupted stream");
    edges.pop_front(); continue;
   }
   // Wait for almost a whole subframe before decoding to avoid rescanning
   // its payload on every arriving transition. The final few edges are cheap.
   if(double(edges.back()-edges.front())<60*t) {
    if(edges.size()<=192)return;
    lose(edges.front(),"Excess transitions within candidate subframe");
    edges.pop_front();continue;
   }
   size_t i=4; unsigned bit=0; uint32_t raw=0; bool invalid=false;
   std::array<uint64_t,8> boundaries{};
   boundaries[0]=edges[0];boundaries[1]=edges[4];
   while(bit<28) {
    if(i+1>=edges.size())return;
    double d=double(edges[i+1]-edges[i]);
    if(match(d,2,t)){++i;++bit;}
    else if(match(d,1,t)) {
     if(i+2>=edges.size())return;
     if(!match(double(edges[i+2]-edges[i+1]),1,t)){invalid=true;break;}
     raw|=uint32_t(1)<<bit;++bit;i+=2;
    } else {invalid=true;break;}
    if(bit==4)boundaries[2]=edges[i];
    else if(bit>=24)boundaries[bit-21]=edges[i];
   }
   if(invalid){lose(edges.front(),"Illegal BMC interval; inspect signal or capture sample rate");edges.pop_front();continue;}
   double measured=double(edges[i]-edges.front())/64.;
   if(std::abs(measured/t-1)>.06){lose(edges.front(),"Subframe clock inconsistent with preamble");edges.pop_front();continue;}
   Subframe f;f.field_boundaries=boundaries;f.start=edges.front();f.end=edges[i]-1;f.preamble=p;f.channel=(p=='W');f.raw=raw;
   uint32_t payload=raw&0xffffff;f.sample=int32_t(payload);if(payload&0x800000)f.sample-=0x1000000;
   f.validity=bool(raw&(1u<<24));f.user=bool(raw&(1u<<25));f.status=bool(raw&(1u<<26));f.parity=bool(raw&(1u<<27));
   f.parity_ok=(ones(raw)%2)==0;f.sample_rate=cfg.capture_rate/(128*measured);
   accept(f,measured);
   for(size_t j=0;j<i;++j)edges.pop_front();
  }
 }
 void edge(uint64_t sample) {
  if(finished)throw std::logic_error("edge after finish");
  if(input_seen&&sample<=last_input)throw std::invalid_argument("edge timestamps must increase strictly");
  if(!input_seen){last_noise_report=sample;input_seen=true;}
  if(half && double(sample-last_input)>8*half)lose(last_input,"Signal gap");
  last_input=sample;edges.push_back(sample);process();
  // Scanning retains at most one subframe. Noise reporting is rate-limited.
  if(!locked&&!have_pending && sample-last_noise_report>uint64_t(cfg.capture_rate)) {
   diag(sample,sample,"searching","No S/PDIF synchronization; inspect channel, rate and signal quality");last_noise_report=sample;
  }
 }
 void finish(uint64_t end) {
  if(finished)return;
  if(input_seen && end<last_input)throw std::invalid_argument("capture end precedes last edge");
  process();
  cancel_burst(end,"Capture ended before IEC 61937 burst completed");
  partial(end,"capture ended before confirming block boundary");
  if(edges.size()>1 || have_pending)diag(edges.front(),end,"truncated","Capture ends with an incomplete or unconfirmed subframe");
  finished=true;
 }
};
Decoder::Decoder(Config c,Sink s):impl(new Impl(c,std::move(s))){}
Decoder::~Decoder(){delete impl;}
void Decoder::edge(uint64_t sample){impl->edge(sample);}
void Decoder::finish(uint64_t end){impl->finish(end);}
std::optional<BurstEvent> Decoder::pending_header() const{return impl->pending_header();}
}
