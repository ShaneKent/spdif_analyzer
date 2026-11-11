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
struct Decoder::Impl {
 Config cfg; Sink sink; std::deque<uint64_t> edges;
 bool locked=false, have_pending=false, block=false, finished=false, have_last=false;
 Subframe pending{}, last{}; unsigned block_count=0; StatusBlock blocks[2];
 uint64_t last_input=0, last_noise_report=0; double half=0;
 explicit Impl(Config c,Sink s):cfg(c),sink(std::move(s)) {
  if(!std::isfinite(c.capture_rate)||c.capture_rate<=0||!std::isfinite(c.nominal_rate)||c.nominal_rate<0)
   throw std::invalid_argument("capture rate must be positive and nominal rate nonnegative");
 }
 void diag(uint64_t a,uint64_t b,const char* code,const std::string& message) {
  if(sink.diagnostic)sink.diagnostic({a,b,code,message});
 }
 void partial(uint64_t end,const char* reason) {
  if(!block)return;
  for(unsigned ch=0;ch<2;++ch) {
   unsigned count=(block_count+1-ch)/2;
   if(count)diag(blocks[ch].start,end,"incomplete_status",std::string(reason)+" channel="+std::to_string(ch)+" bits="+std::to_string(count)+" C="+hex(blocks[ch].c)+" U="+hex(blocks[ch].u));
  }
  block=false; block_count=0;
 }
 void lose(uint64_t at,const char* reason) {
  if(locked||have_pending)diag(at,at,"sync_loss",reason);
  partial(at,reason); locked=false; have_pending=false; have_last=false; half=0;
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
   }
  }
  if(!f.parity_ok)diag(f.start,f.end,"parity","Even parity failed over slots 4 through 31; payload retained");
  if(sink.subframe)sink.subframe(f);
  last=f;have_last=true;
 }
 void accept(Subframe f,double t) {
  if(locked && half && std::abs(t/half-1)>.08)
   lose(f.start,"Transport clock changed; reacquiring");
  if(!locked) {
   if(have_pending && pending.end+1==f.start && pending.channel!=f.channel && std::abs(t/half-1)<.08) {
    locked=true; emit(pending); emit(f); have_pending=false;
   } else {pending=f;have_pending=true;}
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
   while(bit<28) {
    if(i+1>=edges.size())return;
    double d=double(edges[i+1]-edges[i]);
    if(match(d,2,t)){++i;++bit;}
    else if(match(d,1,t)) {
     if(i+2>=edges.size())return;
     if(!match(double(edges[i+2]-edges[i+1]),1,t)){invalid=true;break;}
     raw|=uint32_t(1)<<bit;++bit;i+=2;
    } else {invalid=true;break;}
   }
   if(invalid){lose(edges.front(),"Illegal BMC interval; inspect signal or capture sample rate");edges.pop_front();continue;}
   double measured=double(edges[i]-edges.front())/64.;
   if(std::abs(measured/t-1)>.06){lose(edges.front(),"Subframe clock inconsistent with preamble");edges.pop_front();continue;}
   Subframe f;f.start=edges.front();f.end=edges[i]-1;f.preamble=p;f.channel=(p=='W');f.raw=raw;
   uint32_t payload=raw&0xffffff;f.sample=int32_t(payload);if(payload&0x800000)f.sample-=0x1000000;
   f.validity=bool(raw&(1u<<24));f.user=bool(raw&(1u<<25));f.status=bool(raw&(1u<<26));f.parity=bool(raw&(1u<<27));
   f.parity_ok=(ones(raw)%2)==0;f.sample_rate=cfg.capture_rate/(128*measured);
   accept(f,measured);
   for(size_t j=0;j<i;++j)edges.pop_front();
  }
 }
 void edge(uint64_t sample) {
  if(finished)throw std::logic_error("edge after finish");
  if(!edges.empty()&&sample<=last_input)throw std::invalid_argument("edge timestamps must increase strictly");
  if(half && double(sample-last_input)>8*half)lose(last_input,"Signal gap");
  last_input=sample;edges.push_back(sample);process();
  // Scanning retains at most one subframe. Noise reporting is rate-limited.
  if(!locked&&!have_pending && sample-last_noise_report>uint64_t(cfg.capture_rate)) {
   diag(sample,sample,"searching","No S/PDIF synchronization; inspect channel, rate and signal quality");last_noise_report=sample;
  }
 }
 void finish(uint64_t end) {
  if(finished)return;
  process();
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
