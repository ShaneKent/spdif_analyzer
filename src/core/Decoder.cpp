#include "Decoder.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <iomanip>
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
 int content_mode=-1; std::array<uint8_t,2> known_c0{};
 unsigned burst_stage=0,remaining_words=0,burst_age=0;
 ContentInfo burst_info{}; ContentFormat burst_type=ContentFormat::Unknown;
 bool burst_valid=false;
 ContentFormat base_content() const {return content_mode==0?ContentFormat::Pcm:content_mode==1?ContentFormat::NonPcm:ContentFormat::Unknown;}
 void content_event(const ContentInfo& info){if(sink.content)sink.content(info);}
 void label(ContentFormat next,uint64_t at,const std::string& why) {
  if(next==content)return;
  content=next;ContentInfo info;info.start=info.end=at;info.format=next;info.evidence=why;
  info.integrity=content_mode>=0;content_event(info);
 }
 void cancel_burst(uint64_t at,const char* why) {
  if(burst_stage){burst_info.end=at;burst_info.complete=false;burst_info.integrity=false;
   burst_info.evidence=why;content_event(burst_info);}
  burst_stage=remaining_words=0;burst_valid=false;burst_age=0;
 }
 void unknown_content(uint64_t at,const char* why) {
  cancel_burst(at,why);content_mode=-1;label(ContentFormat::Unknown,at,why);
 }
 void confirmed_status(uint64_t at) {
  int mode=-1;
  if(blocks[0].integrity&&blocks[1].integrity && !(blocks[0].c[0]&0xc1) && !(blocks[1].c[0]&0xc1)
    && ((blocks[0].c[0]^blocks[1].c[0])&2)==0)mode=(blocks[0].c[0]>>1)&1;
  if(mode!=content_mode){cancel_burst(at,"Channel status changed");content_mode=mode;label(base_content(),at,"Confirmed complete per-channel consumer status blocks");}
  known_c0={blocks[0].c[0],blocks[1].c[0]};
 }
 ContentFormat burst_label() const {
  if(content_mode==0)return ContentFormat::Conflict;
  if(content_mode!=1)return ContentFormat::Provisional;
  return burst_type;
 }
 void burst_word(const Subframe& f) {
  if(!f.parity_ok){unknown_content(f.start,"Parity damage invalidates content evidence");return;}
  if(burst_age && ++burst_age>12288){cancel_burst(f.start,"No fresh IEC 61937 header within supported repetition window");label(base_content(),f.start,"Expired IEC 61937 format evidence");}
  const uint16_t word=uint16_t((f.raw>>8)&0xffff);
  if(burst_stage==4){
   if(--remaining_words==0){burst_info.end=f.end;burst_info.complete=true;
    burst_info.format=burst_label();burst_info.integrity=burst_valid;
    burst_info.evidence=std::string(content_name(burst_type))+" payload length observed; "+(content_mode==1?"non-audio status corroborates":content_mode==0?"conflicts with PCM status":"channel status unconfirmed");
    if(!burst_valid)burst_info.evidence+="; Pc error flag is set";
    content_event(burst_info);
    label(burst_valid?burst_label():base_content(),f.start,burst_info.evidence);burst_stage=0;
   }
   return;
  }
  if(burst_stage==1){if(f.channel==1 && word==0x4e1f){burst_stage=2;return;}burst_stage=0;}
  else if(burst_stage==2){if(f.channel==0){burst_info.pc=word;burst_stage=3;return;}burst_stage=0;}
  else if(burst_stage==3){
   if(f.channel!=1){burst_stage=0;return;}
   burst_info.pd=word;burst_info.end=f.end;burst_info.burst=true;burst_info.complete=false;
   const unsigned type=burst_info.pc&0x7f;
   unsigned limit_bits=0;burst_type=ContentFormat::Unsupported;
   switch(type){case 0:burst_type=ContentFormat::Null;break;case 3:burst_type=ContentFormat::Pause;break;
    case 1:burst_type=ContentFormat::Ac3;limit_bits=1536*32-64;break;
    case 11:burst_type=ContentFormat::Dts1;limit_bits=512*32-64;break;
    case 12:burst_type=ContentFormat::Dts2;limit_bits=1024*32-64;break;
    case 13:burst_type=ContentFormat::Dts3;limit_bits=2048*32-64;break;
    case 21:burst_type=ContentFormat::Eac3;limit_bits=6144*32-64;break;}
   const unsigned bits=type==21?unsigned(word)*8:unsigned(word);
   burst_valid=(burst_info.pc&0x80)==0;
   burst_info.format=burst_label();burst_info.integrity=burst_valid;
   burst_info.evidence=std::string("IEC 61937 header: ")+content_name(burst_type)+"; Pc="+std::to_string(burst_info.pc)+"; stream="+std::to_string(burst_info.pc>>13)+"; payload not yet validated";
   if(!burst_valid)burst_info.evidence+="; Pc error flag is set";
   content_event(burst_info);burst_age=1;
   label(content_mode==0?ContentFormat::Conflict:content_mode==1?ContentFormat::NonPcm:ContentFormat::Provisional,f.start,burst_info.evidence);
   if(!limit_bits){
    if(type==0 && word==0){burst_info.complete=true;burst_info.evidence="Null burst with zero payload length";content_event(burst_info);label(burst_label(),f.start,burst_info.evidence);}
    else if(type==3)label(burst_label(),f.start,"Pause header; type-specific payload interpretation unsupported");
    else if(type!=0)label(burst_label(),f.start,"Unsupported IEC 61937 type; payload length units not assumed");
    burst_stage=0;return;
   }
   if(!bits || bits>limit_bits){
    burst_info.integrity=false;burst_info.evidence="Rejected IEC 61937 payload length outside type-specific burst capacity";
    content_event(burst_info);diag(burst_info.start,f.end,"burst_length",burst_info.evidence);burst_stage=0;return;
   }
   remaining_words=(bits+15)/16;burst_stage=4;return;
  }
  if(f.channel==0 && word==0xf872){burst_info=ContentInfo{};burst_info.start=f.start;burst_info.burst=true;burst_stage=1;}
 }

 explicit Impl(Config c,Sink s):cfg(c),sink(std::move(s)) {
  if(!std::isfinite(c.capture_rate)||c.capture_rate<=0||!std::isfinite(c.nominal_rate)||c.nominal_rate<0)
   throw std::invalid_argument("capture rate must be positive and nominal rate nonnegative");
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
     for(auto &b:blocks){b.end=last.end;b.interpretation=interpret_status(b.c);if(sink.status)sink.status(b);}
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
    if(content_mode>=0 && pos<8 && f.parity_ok && f.status!=bool((known_c0[f.channel]>>pos)&1))
     unknown_content(f.start,"Current channel status differs from previous confirmed block");
   }
  }
  if(!f.parity_ok)diag(f.start,f.end,"parity","Even parity failed over slots 4 through 31; payload retained");
  burst_word(f);f.content=content;
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
}
