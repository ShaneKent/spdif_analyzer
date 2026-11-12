#include "Analysis.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace spdif {
namespace {
uint64_t after(uint64_t n){if(n==UINT64_MAX)throw std::invalid_argument("sample endpoint overflow");return n+1;}
void fault(ChannelHealth& h,uint64_t first,uint64_t last){if(!h.has_fault){h.has_fault=true;h.first_fault=first;h.last_fault=last;}else{h.first_fault=std::min(h.first_fault,first);h.last_fault=std::max(h.last_fault,last);}}
void finalize(HealthWindow& w){const auto duration=w.end-w.start;w.unknown_stream_samples=duration-std::min(duration,w.decoded_stream_samples);for(auto& c:w.channels)c.unknown_coverage_samples=duration-std::min(duration,c.decoded_coverage_samples);}
bool layout_equal(const SampleLayout&a,const SampleLayout&b){return a.known==b.known&&a.bits==b.bits&&a.stereo==b.stereo&&a.professional==b.professional;}
std::map<std::string,std::string> fields(const std::array<uint8_t,24>& c){
 auto info=status_info(c);std::map<std::string,std::string> result;
 const auto number=[](unsigned n){return std::to_string(n);};
 result["professional"]=number(info.professional);
 if(!info.professional&&!info.supported){result["mode_code"]=number(c[0]>>6);return result;}
 result["non_audio"]=number(info.non_audio);
 result["emphasis_code"]=number(info.emphasis_code);
 result["audio_content"]=info.non_audio?"non-audio":"audio";
 const auto emphasis=info.emphasis_code;
 result["emphasis"]=(!info.professional&&emphasis==0)||(info.professional&&emphasis==1)?"none":
  ((!info.professional&&emphasis==1)||(info.professional&&emphasis==3)?"50/15 us":
  (info.professional&&emphasis==7?"CCITT J.17":info.professional&&emphasis==0?"not indicated":"unknown (code="+number(emphasis)+")"));
 result["sample_rate_hz"]=info.sample_rate?number(info.sample_rate):"unknown";
 result["word_length_bits"]=info.word_length?number(info.word_length):"not indicated or unsupported";
 if(info.professional){
  result["channel_mode_code"]=number(c[1]&15);result["sample_rate_code"]=number(c[0]>>6);
  result["word_length_code"]=number((c[2]>>3)&7);
 }else{
  result["mode_code"]=number(c[0]>>6);result["copyright_not_asserted"]=number((c[0]>>2)&1);
  result["category_code"]=number(c[1]&127);result["category_L"]=number(c[1]>>7);
  result["source_number"]=number(c[2]&15);result["channel_number"]=number(c[2]>>4);
  result["sample_rate_code"]=number(c[3]&15);result["clock_accuracy_code"]=number((c[3]>>4)&3);
  result["word_length_code"]=number(c[4]&15);result["original_rate_code"]=number(c[4]>>4);
 }
 return result;
}
}
struct Analysis::Impl {
 AnalysisConfig config;AnalysisSink sink;uint64_t width=0,next_window=0,observed_end=0;bool finished=false;
 HealthWindow total{};std::map<uint64_t,HealthWindow> windows;
 bool have_global=false,have_coverage=false;uint64_t coverage_end=0;Subframe last_global{};std::array<bool,2> have_channel{};std::array<Subframe,2> previous{};
 std::array<bool,2> have_parity_fault{};std::array<uint64_t,2> last_parity_fault{};
 std::array<bool,2> have_status{};std::array<StatusBlock,2> baseline{};
 struct PcmAccumulator {
  bool present=false;PcmWindow result{};SampleLayout layout{};ContentFormat content=ContentFormat::Unknown;
  double rate=0;long double sum=0,squares=0;uint64_t full_run=0;
 };
 std::array<PcmAccumulator,2> pcm{};uint64_t next_segment=0;
 Impl(AnalysisConfig c,AnalysisSink s):config(c),sink(std::move(s)),next_window(c.origin),observed_end(c.origin){
  if(!std::isfinite(c.capture_rate)||c.capture_rate<=0||c.capture_rate>UINT32_MAX||!std::isfinite(c.window_seconds)||c.window_seconds<=0||
     (c.layout_override_bits && (c.layout_override_bits<16||c.layout_override_bits>24)))throw std::invalid_argument("invalid analysis configuration");
  const long double samples=static_cast<long double>(c.capture_rate)*c.window_seconds;
  if(samples<1||samples>static_cast<long double>(UINT64_MAX)/2)throw std::invalid_argument("analysis window outside supported sample range");
  // Convert through the unsigned range: llround is signed and can overflow
  // when a valid near-half-U64 window rounds to 2^63.
  width=uint64_t(std::floor(samples+0.5L));total.start=c.origin;total.end=c.origin;
 }
 void running() const {if(finished)throw std::logic_error("analysis event after finish");}
 uint64_t window_start(uint64_t sample) const {return config.origin+((sample-config.origin)/width)*width;}
 uint64_t window_end(uint64_t start) const {return start>UINT64_MAX-width?UINT64_MAX:start+width;}
 HealthWindow& bucket(uint64_t at){
  bool late=false;if(at<next_window){at=next_window;++total.late_events;late=true;}
  auto start=window_start(at);auto& b=windows[start];b.start=start;b.end=window_end(start);b.late_events+=late;return b;
 }
 void close_until(uint64_t watermark){
  while(window_end(next_window)<=watermark){
   auto found=windows.find(next_window);HealthWindow b;
   if(found!=windows.end()){b=found->second;windows.erase(found);}else{b.start=next_window;b.end=window_end(next_window);}
   finalize(b);if(sink.health)sink.health(b);next_window=b.end;if(next_window==UINT64_MAX)break;
  }
 }
 void metadata_observed(uint64_t at){
  observed_end=std::max(observed_end,after(at));total.end=observed_end;
  // A callback may precede delivery of an already decoded subframe, and a
  // searching diagnostic can occur while the next candidate is incomplete.
  // Retain a complete two-channel cycle at the lowest acquisition rate.
  const auto guard=uint64_t(std::ceil(config.capture_rate/7500.0));
  uint64_t watermark=at>guard?at-guard:config.origin;
  if(have_global){
   uint64_t pending=last_global.start;
   for(unsigned ch=0;ch<2;++ch)if(have_channel[ch])pending=std::min(pending,previous[ch].start);
   watermark=std::min(watermark,pending);
  }
  close_until(watermark);
 }
 template<typename Fn>void interval(uint64_t begin,uint64_t end,Fn update){
  begin=std::max(begin,config.origin);if(end<=begin)return;
  if(begin<next_window)begin=next_window; // Published windows are immutable.
  while(begin<end){auto& b=bucket(begin);auto stop=std::min(end,b.end);update(b,stop-begin);begin=stop;}
 }
 PcmWindow pcm_result(const PcmAccumulator& p) const {
  auto result=p.result;
  if(result.accepted){
   result.mean=double(p.sum/result.accepted);result.rms=std::sqrt(double(p.squares/result.accepted));
   const double full=std::ldexp(1.0,int(result.bits)-1);
   result.peak_normalized=double(result.peak)/full;result.rms_normalized=result.rms/full;
  }
  return result;
 }
 std::vector<PcmWindow> current_pcm() const {
  std::vector<PcmWindow> result;
  for(const auto& p:pcm)if(p.present)result.push_back(pcm_result(p));
  return result;
 }
 void pcm_flush(unsigned channel){auto& p=pcm[channel];if(!p.present)return;
  if(sink.pcm)sink.pcm(pcm_result(p));
  p=PcmAccumulator{};
 }
 void clear_continuity(){have_global=false;have_channel={false,false};have_status={false,false};for(unsigned ch=0;ch<2;++ch)pcm_flush(ch);}
 void pcm_add(const Subframe& f){
  const unsigned ch=f.channel;auto layout=f.layout;bool override=false;
  if(!layout.known && config.layout_override_bits){layout.bits=config.layout_override_bits;layout.known=true;override=true;}
  enum Exclusion {None,Parity,Invalid,Unknown,NonPcm,Layout};Exclusion excluded=None;
  if(!f.parity_ok)excluded=Parity;else if(f.validity)excluded=Invalid;
  else if(f.content==ContentFormat::Unknown||f.content==ContentFormat::Provisional||f.content==ContentFormat::Conflict)excluded=Unknown;
  else if(f.content!=ContentFormat::Pcm)excluded=NonPcm;
  else if(!layout.known||layout.bits<1||layout.bits>24||!std::isfinite(f.sample_rate)||f.sample_rate<=0)excluded=Layout;
  auto& p=pcm[ch];const auto start=window_start(f.start);
  if(p.present && (p.result.window_start!=start || p.content!=f.content ||
     p.result.layout_override!=override ||
     !layout_equal(p.layout,layout)||std::abs(p.rate-f.sample_rate)>std::max(1.0,p.rate*.02)))pcm_flush(ch);
  if(!p.present){p.present=true;p.layout=layout;p.content=f.content;p.rate=f.sample_rate;
   p.result.channel=ch;p.result.window_start=start;p.result.window_end=window_end(start);p.result.start=f.start;
   p.result.end=after(f.end);p.result.segment=++next_segment;p.result.bits=layout.known&&layout.bits>=1&&layout.bits<=24?layout.bits:0;p.result.layout_override=override;
  }
  p.result.end=after(f.end);
  // Bad samples contribute exclusion counts to this window but must not join
  // digital full-scale runs on either side of a missing/invalid observation.
  if(excluded!=None)p.full_run=0;
  switch(excluded){case Parity:++p.result.excluded_parity;return;case Invalid:++p.result.excluded_invalid;return;
   case Unknown:++p.result.excluded_unknown;return;case NonPcm:++p.result.excluded_non_pcm;return;case Layout:++p.result.excluded_layout;return;default:break;}
  const int32_t value=pcm_value(f.raw,layout);++p.result.accepted;
  if(p.result.accepted==1)p.result.minimum=p.result.maximum=value;
  else {p.result.minimum=std::min(p.result.minimum,value);p.result.maximum=std::max(p.result.maximum,value);}
  const uint64_t magnitude=value<0?uint64_t(-int64_t(value)):uint64_t(value);
  p.result.peak=std::max(p.result.peak,magnitude);p.sum+=value;p.squares+=static_cast<long double>(value)*value;
  if(!value){++p.result.silence_samples;p.result.silence_seconds+=1.0/f.sample_rate;}
  const int32_t minimum=-(int32_t(1)<<(layout.bits-1)),maximum=(int32_t(1)<<(layout.bits-1))-1;
  if(value==minimum||value==maximum){++p.result.full_scale_samples;if(!p.full_run)++p.result.full_scale_runs;
   ++p.full_run;p.result.max_full_scale_run=std::max(p.result.max_full_scale_run,p.full_run);
  }else p.full_run=0;
 }
 void add(const Subframe& f){running();if(f.channel>1||f.end<f.start)throw std::invalid_argument("invalid subframe interval/channel");
  if(f.start<config.origin)return;
  const auto end=after(f.end);observed_end=std::max(observed_end,end);total.end=observed_end;
  const bool continuous=have_global && last_global.end!=UINT64_MAX && last_global.end+1==f.start && last_global.channel!=f.channel;
  if(have_global&&!continuous)clear_continuity();
  if(have_channel[f.channel] && continuous){const auto begin=previous[f.channel].start;
   interval(begin,f.start,[&](HealthWindow& w,uint64_t n){w.channels[f.channel].decoded_coverage_samples+=n;});
   total.channels[f.channel].decoded_coverage_samples+=f.start-begin;
  }
  // Global decoded coverage is a union even if an external caller repeats an overlapping frame.
  const auto begin=have_coverage?std::max(f.start,coverage_end):f.start;
  if(end>begin){interval(begin,end,[](HealthWindow&w,uint64_t n){w.decoded_stream_samples+=n;});total.decoded_stream_samples+=end-begin;}
  coverage_end=std::max(coverage_end,end);have_coverage=true;
  auto& b=bucket(f.start);
  for(auto* h:{&b.channels[f.channel],&total.channels[f.channel]}){
   ++h->decoded_subframes;h->parity_failures+=!f.parity_ok;h->v_invalid+=f.validity;
   if(!f.parity_ok)fault(*h,f.start,f.end);
   if(std::isfinite(f.sample_rate)&&f.sample_rate>0){++h->rate_measurements;h->mean_sample_rate+=(f.sample_rate-h->mean_sample_rate)/h->rate_measurements;
    if(h->min_sample_rate==0)h->min_sample_rate=f.sample_rate;else h->min_sample_rate=std::min(h->min_sample_rate,f.sample_rate);
    h->max_sample_rate=std::max(h->max_sample_rate,f.sample_rate);}
  }
  if(!f.parity_ok){have_status[f.channel]=false;have_parity_fault[f.channel]=true;last_parity_fault[f.channel]=f.start;}
  pcm_add(f);previous[f.channel]=f;have_channel[f.channel]=true;last_global=f;have_global=true;
  const auto watermark=have_channel[0]&&have_channel[1]?std::min(previous[0].start,previous[1].start):f.start;
  close_until(watermark);
 }
 void add(const StatusBlock& block){running();if(block.channel>1||block.end<block.start)throw std::invalid_argument("invalid status block");
  const auto at=block.end;auto& b=bucket(std::max(at,config.origin));
  auto& channel=b.channels[block.channel];auto& all=total.channels[block.channel];
  if(block.integrity){++channel.status_good;++all.status_good;}else{++channel.status_damaged;++all.status_damaged;
   // Do not replace a precise observed parity location with its entire status
   // block interval. Status-only inputs still expose the known affected range.
   if(!have_parity_fault[block.channel]||last_parity_fault[block.channel]<block.start||last_parity_fault[block.channel]>block.end){fault(channel,block.start,block.end);fault(all,block.start,block.end);}
   have_status[block.channel]=false;pcm_flush(block.channel);metadata_observed(at);return;}
  const auto ch=block.channel;
  if(have_status[ch] && baseline[ch].end!=UINT64_MAX && baseline[ch].end+1==block.start && baseline[ch].c!=block.c){
   StatusChange change;change.channel=ch;change.previous_start=baseline[ch].start;change.previous_end=after(baseline[ch].end);
   change.start=block.start;change.end=after(block.end);change.previous=baseline[ch].c;change.current=block.c;
   for(size_t i=0;i<24;++i)change.changed_mask[i]=change.previous[i]^change.current[i];
   const auto old_fields=fields(change.previous),new_fields=fields(change.current);
   std::map<std::string,std::string> names=old_fields;names.insert(new_fields.begin(),new_fields.end());
   for(const auto& field:names){auto old=old_fields.find(field.first),now=new_fields.find(field.first);
    const auto old_value=old==old_fields.end()?"not applicable":old->second;
    const auto new_value=now==new_fields.end()?"not applicable":now->second;
    if(old_value!=new_value)change.fields.push_back({field.first,old_value,new_value});}
   for(size_t i=0;i<24;++i)if(change.changed_mask[i])change.fields.push_back({"C_byte_"+std::to_string(i),std::to_string(change.previous[i]),std::to_string(change.current[i])});
   if(sink.status_change)sink.status_change(change);
  }
  if(have_status[ch] && baseline[ch].c!=block.c)pcm_flush(ch);
  baseline[ch]=block;have_status[ch]=true;metadata_observed(at);
 }
 void add(const Diagnostic& d){running();auto& b=bucket(std::max(d.end,config.origin));
  if(d.code=="sync_loss"){++b.sync_losses;++total.sync_losses;for(unsigned ch=0;ch<2;++ch){++b.channels[ch].sync_losses;++total.channels[ch].sync_losses;}}
  if(d.code!="searching"&&d.code!="parity"&&d.code!="incomplete_status"&&d.code!="truncated"&&d.code!="burst_overlap_candidate"&&d.code!="notification_summary"){for(unsigned ch=0;ch<2;++ch){fault(b.channels[ch],d.start,d.end);fault(total.channels[ch],d.start,d.end);}}
  if(d.code=="sync_loss"||d.code=="channel_order")clear_continuity();
  else if(d.code=="block_length"){have_status={false,false};for(unsigned ch=0;ch<2;++ch)pcm_flush(ch);}
  else if(d.code=="incomplete_status"){
   const auto channel=d.message.find("channel=");
   if(channel!=std::string::npos && channel+8<d.message.size() && (d.message[channel+8]=='0'||d.message[channel+8]=='1')){
    const unsigned ch=unsigned(d.message[channel+8]-'0');have_status[ch]=false;pcm_flush(ch);
   }else{have_status={false,false};for(unsigned ch=0;ch<2;++ch)pcm_flush(ch);}
  }
  metadata_observed(d.end);
 }
 void add(const ContentInfo& c){running();if(!c.burst){for(unsigned ch=0;ch<2;++ch)if(pcm[ch].present&&pcm[ch].content!=c.format)pcm_flush(ch);}}
 HealthWindow summary()const{auto copy=total;copy.end=observed_end;finalize(copy);return copy;}
 void finish(uint64_t end){if(finished)return;if(end<observed_end)throw std::invalid_argument("analysis end before observed data");
  observed_end=end;total.end=end;for(unsigned ch=0;ch<2;++ch)pcm_flush(ch);
  close_until(end);
  if(next_window<end){auto found=windows.find(next_window);HealthWindow b;
   if(found!=windows.end())b=found->second;
   b.start=next_window;b.end=end;finalize(b);if(sink.health)sink.health(b);}
  windows.clear();finished=true;
 }
};
Analysis::Analysis(AnalysisConfig c,AnalysisSink s):impl(new Impl(c,std::move(s))){}
Analysis::~Analysis()=default;
void Analysis::add(const Subframe&f){impl->add(f);}void Analysis::add(const StatusBlock&b){impl->add(b);}
void Analysis::add(const Diagnostic&d){impl->add(d);}void Analysis::add(const ContentInfo&c){impl->add(c);}
void Analysis::finish(uint64_t end){impl->finish(end);}HealthWindow Analysis::summary()const{return impl->summary();}
std::vector<PcmWindow> Analysis::current_pcm()const{return impl->current_pcm();}
}
