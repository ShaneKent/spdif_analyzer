#include "Exports.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
namespace spdif::exports {
namespace fs=std::filesystem;
namespace {
std::string quote(const std::string& s){std::ostringstream o;o.imbue(std::locale::classic());o<<'"';for(unsigned char c:s){switch(c){case '"':o<<"\\\"";break;case '\\':o<<"\\\\";break;case '\n':o<<"\\n";break;case '\r':o<<"\\r";break;case '\t':o<<"\\t";break;default:if(c<32)o<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<unsigned(c)<<std::dec;else o<<char(c);}}o<<'"';return o.str();}
void checked(std::ostream& o){if(!o)throw std::runtime_error("Export write failed");}
void new_dir(const fs::path& p){if(fs::exists(p)||!fs::create_directories(p))throw std::runtime_error("Export requires a new directory: "+p.string());std::ofstream marker(p/"INCOMPLETE");marker.imbue(std::locale::classic());marker<<"Export is incomplete until this marker is removed.\n";checked(marker);}
void number16(std::ostream& o,uint16_t n){o.put(char(n&255));o.put(char(n>>8));}
void number32(std::ostream& o,uint32_t n){number16(o,uint16_t(n));number16(o,uint16_t(n>>16));}
std::string filename(const char* prefix,uint64_t n,const char* ext){std::ostringstream s;s.imbue(std::locale::classic());s<<prefix<<std::setw(6)<<std::setfill('0')<<n<<ext;return s.str();}
bool codec(ContentFormat c){return c==ContentFormat::Ac3||c==ContentFormat::Dts1||c==ContentFormat::Dts2||c==ContentFormat::Dts3||c==ContentFormat::Eac3;}
void wav_header(std::ostream& out,unsigned rate,unsigned bits,uint32_t bytes){
 out.write("RIFF",4);number32(out,60+bytes);out.write("WAVEfmt ",8);number32(out,40);
 number16(out,0xfffe);number16(out,2);number32(out,rate);number32(out,rate*6);number16(out,6);number16(out,24);number16(out,22);number16(out,uint16_t(bits));number32(out,3);
 const unsigned char pcm_guid[]={1,0,0,0,0,0,16,0,128,0,0,170,0,56,155,113};out.write(reinterpret_cast<const char*>(pcm_guid),16);out.write("data",4);number32(out,bytes);
}
}
struct WavWriter::Impl {
 fs::path dir;WavConfig cfg;std::ofstream index,wav;bool done=false,active=false,pending=false,have_previous=false;
 Subframe left{};uint64_t previous_end=0,observed_end_exclusive=0,segment_number=0,frames=0,start=0,end=0;
 unsigned rates[2]{},declared_rates[2]{},rate=0,bits=0;bool professional=false,overridden_bits=false;
 std::string part_name,final_name;
 Impl(const fs::path& p,WavConfig c):dir(p),cfg(c){
  if(!(cfg.capture_rate>0)||!std::isfinite(cfg.capture_rate)||cfg.capture_rate>double(UINT32_MAX))throw std::invalid_argument("WAV requires a finite capture rate in (0, UINT32_MAX]");
  if(cfg.bits_override && (cfg.bits_override<16||cfg.bits_override>24))throw std::invalid_argument("WAV bit override must be 16..24");
  if(cfg.rate_override && (cfg.rate_override<8000||cfg.rate_override>192000))throw std::invalid_argument("WAV rate override must be 8000..192000");
  new_dir(dir);index.open(dir/"index.jsonl",std::ios::binary);index.imbue(std::locale::classic());
  index<<"{\"type\":\"configuration\",\"schema_version\":1,\"container_bits\":24,\"capture_rate\":"<<std::setprecision(17)<<cfg.capture_rate<<",\"bits_override\":"<<cfg.bits_override<<",\"rate_override\":"<<cfg.rate_override<<",\"assume_stereo\":"<<(cfg.assume_stereo?"true":"false")<<",\"timestamp_encoding\":\"decimal strings, inclusive capture samples\"}\n";checked(index);
 }
 ~Impl(){try{if(!done){close("writer_not_finished");index<<"{\"type\":\"finish\",\"complete\":false}\n";index.flush();}}catch(...){}}
 void exclusion(const Subframe& f,const std::string& reason){index<<"{\"type\":\"excluded\",\"start_sample\":"<<quote(std::to_string(f.start))<<",\"end_sample\":"<<quote(std::to_string(f.end))<<",\"channel\":"<<f.channel<<",\"reason\":"<<quote(reason)<<"}\n";checked(index);}
 void close(const std::string& reason){
  if(!active)return;
  const auto bytes=uint32_t(frames*6);wav.seekp(0);wav_header(wav,rate,bits,bytes);wav.flush();checked(wav);wav.close();checked(wav);fs::rename(dir/part_name,dir/final_name);
  index<<"{\"type\":\"segment\",\"file\":"<<quote(final_name)<<",\"start_sample\":"<<quote(std::to_string(start))<<",\"end_sample\":"<<quote(std::to_string(end))<<",\"frames\":"<<frames<<",\"rate\":"<<rate<<",\"valid_bits\":"<<bits<<",\"container_bits\":24,\"bits_overridden\":"<<(overridden_bits?"true":"false")<<",\"rate_overridden\":"<<(cfg.rate_override?"true":"false")<<",\"source_declared_rate_A\":"<<declared_rates[0]<<",\"source_declared_rate_B\":"<<declared_rates[1]<<",\"close_reason\":"<<quote(reason)<<"}\n";checked(index);active=false;
 }
 void clear_pending(const std::string& reason){if(pending){exclusion(left,reason);pending=false;}}
 std::string eligibility(const Subframe& f,SampleLayout& layout,unsigned& selected_rate){
  if(f.content!=ContentFormat::Pcm)return "not_confirmed_pcm";
  if(!f.parity_ok)return "parity_error";
  if(f.validity)return "validity_invalid";
  layout=f.layout;
  if(!layout.stereo && !cfg.assume_stereo)return "stereo_layout_unknown";
  if(!layout.known && cfg.bits_override){layout.known=true;layout.bits=cfg.bits_override;}
  if(!layout.known || layout.bits<16||layout.bits>24)return "word_length_unknown";
  selected_rate=cfg.rate_override?cfg.rate_override:rates[f.channel];
  if(!selected_rate)return "sample_rate_unknown";
  return {};
 }
 void sample(const Subframe& f){
  if(done)throw std::logic_error("WAV writer already finished");
  if(f.channel>1||f.end<f.start||f.end==UINT64_MAX)throw std::invalid_argument("Invalid or unrepresentable subframe interval/channel");
  if(f.start<observed_end_exclusive)throw std::invalid_argument("WAV source timestamps must remain monotonic across discontinuities");
  observed_end_exclusive=f.end+1;
  if(have_previous && f.start<=previous_end)throw std::invalid_argument("WAV requires monotonic nonoverlapping subframes");
  if(have_previous && (previous_end==UINT64_MAX || f.start!=previous_end+1)){close("gap");clear_pending("unpaired_before_gap");}
  previous_end=f.end;have_previous=true;
  SampleLayout layout;unsigned r=0;const auto reason=eligibility(f,layout,r);
  if(!reason.empty()){close(reason);clear_pending("unpaired_before_excluded");exclusion(f,reason);return;}
  if(f.channel==0){if(pending){close("unpaired_channel_A");clear_pending("unpaired_channel_A");}left=f;pending=true;return;}
  if(!pending){close("unpaired_channel_B");exclusion(f,"unpaired_channel_B");return;}
  SampleLayout l;unsigned lr=0;const auto left_reason=eligibility(left,l,lr);
  if(!left_reason.empty()||lr!=r||l.bits!=layout.bits||l.professional!=layout.professional||left.end==UINT64_MAX||left.end+1!=f.start){
   close("incompatible_pair");clear_pending("incompatible_pair");exclusion(f,"incompatible_pair");return;
  }
  const bool override_bits=!f.layout.known||!left.layout.known;
  if(active && (rate!=r||bits!=layout.bits||professional!=layout.professional||overridden_bits!=override_bits||declared_rates[0]!=rates[0]||declared_rates[1]!=rates[1]))close("rate_or_layout_change");
  if(active && frames>=(uint64_t(UINT32_MAX)-60)/6)close("riff_size_limit");
  if(!active){final_name=filename("segment-",++segment_number,".wav");part_name=final_name+".part";wav.open(dir/part_name,std::ios::binary);wav.imbue(std::locale::classic());rate=r;declared_rates[0]=rates[0];declared_rates[1]=rates[1];bits=layout.bits;professional=layout.professional;overridden_bits=override_bits;frames=0;start=left.start;wav_header(wav,rate,bits,0);checked(wav);active=true;}
  const Subframe* pair[]={&left,&f};
  for(const auto* s:pair){
   const int64_t value=int64_t(pcm_value(s->raw,layout))*(int64_t(1)<<(24-layout.bits));
   const auto encoded=uint32_t(value)&0xffffff;wav.put(char(encoded&255));wav.put(char((encoded>>8)&255));wav.put(char((encoded>>16)&255));
  }
  checked(wav);++frames;end=f.end;pending=false;
 }
};
WavWriter::WavWriter(const fs::path& p,WavConfig c):impl(new Impl(p,c)){}
WavWriter::~WavWriter()=default;
void WavWriter::status(const StatusBlock& b){if(b.channel>1)throw std::invalid_argument("Invalid status channel");const auto info=status_info(b.c);impl->rates[b.channel]=(b.integrity&&info.supported&&!info.non_audio)?info.sample_rate:0;}
void WavWriter::subframe(const Subframe& f){impl->sample(f);}
void WavWriter::diagnostic(const Diagnostic& d){if(d.code=="sync_loss"||d.code=="channel_order"||d.code=="truncated"||d.code=="block_length"){impl->close(d.code);impl->clear_pending("unpaired_"+d.code);impl->rates[0]=impl->rates[1]=0;impl->have_previous=false;}}
void WavWriter::finish(uint64_t end_exclusive){if(impl->done)return;if(end_exclusive<impl->observed_end_exclusive)throw std::invalid_argument("WAV exclusive end precedes observed subframes");impl->close("end_of_input");impl->clear_pending("unpaired_end_of_input");impl->index<<"{\"type\":\"finish\",\"complete\":true,\"end_exclusive_sample\":"<<quote(std::to_string(end_exclusive))<<"}\n";impl->index.flush();checked(impl->index);impl->index.close();checked(impl->index);impl->done=true;fs::remove(impl->dir/"INCOMPLETE");}
struct BurstWriter::Impl {
 fs::path dir;std::ofstream index,payload;bool done=false,active=false,receiving=false,malformed=false;
 BurstEvent begin{};uint64_t number=0,words=0,bits=0,last_end=0;std::string part_name,final_name;
 explicit Impl(const fs::path& p):dir(p){new_dir(dir);index.open(dir/"index.jsonl",std::ios::binary);index.imbue(std::locale::classic());index<<"{\"type\":\"configuration\",\"schema_version\":1,\"word_order\":\"big-endian 16-bit transport words in received A/B order\",\"partial_word\":\"valid bits are high bits; unused low bits are zero, final word retained as two bytes\",\"timestamp_encoding\":\"decimal strings, inclusive capture samples\"}\n";checked(index);}
 ~Impl(){try{if(!done){if(active){auto previous=begin.info;previous.end=last_end;end(previous,false,"writer_not_finished");}index<<"{\"type\":\"finish\",\"complete\":false}\n";index.flush();}}catch(...){}}
 void end(const ContentInfo& info,bool good,const std::string& reason){
  if(!active)return;
  if(payload.is_open()){payload.flush();checked(payload);payload.close();checked(payload);}
  const bool accepted=good&&!(begin.info.pc&128)&&receiving&&!malformed&&info.complete&&info.integrity&&codec(info.format)&&bits==begin.declared_bits;
  if(accepted)fs::rename(dir/part_name,dir/final_name);else if(!part_name.empty())fs::remove(dir/part_name);
  index<<"{\"type\":\"burst\",\"accepted\":"<<(accepted?"true":"false")<<",\"file\":"<<(accepted?quote(final_name):"null")<<",\"start_sample\":"<<quote(std::to_string(begin.info.start))<<",\"end_sample\":"<<quote(std::to_string(info.end))<<",\"format\":"<<quote(content_name(info.format))<<",\"stream_number\":"<<((begin.info.pc>>13)&7)<<",\"Pc\":"<<begin.info.pc<<",\"Pd\":"<<begin.info.pd<<",\"declared_bits\":"<<begin.declared_bits<<",\"observed_bits\":"<<bits<<",\"payload_words\":"<<words<<",\"final_word_valid_bits\":"<<(bits?((bits-1)%16+1):0)<<",\"complete\":"<<(info.complete?"true":"false")<<",\"integrity\":"<<(info.integrity?"true":"false")<<",\"reason\":"<<quote(accepted?"confirmed_complete_payload":reason)<<",\"evidence\":"<<quote(info.evidence)<<"}\n";checked(index);active=false;receiving=false;part_name.clear();
 }
 void event(const BurstEvent& e){
  if(done)throw std::logic_error("Burst writer already finished");
  if(e.phase==BurstPhase::Begin){
   if(active){auto previous=begin.info;previous.end=last_end;end(previous,false,"new_header_before_payload_end");}
   begin=e;last_end=e.info.end;active=true;words=bits=0;malformed=false;part_name.clear();
   const unsigned type=e.info.pc&127;
   const bool known=type==1||type==11||type==12||type==13||type==21;
   const uint32_t expected=type==21?uint32_t(e.info.pd)*8:uint32_t(e.info.pd);
   receiving=e.supported&&known&&expected>0&&expected==e.declared_bits;
   final_name=filename("burst-",++number,".bin");
   if(receiving){part_name=final_name+".part";payload.open(dir/part_name,std::ios::binary);payload.imbue(std::locale::classic());checked(payload);}
  }else if(e.phase==BurstPhase::Word){
   if(!active)return;
   if(!receiving)return;
   last_end=std::max(last_end,e.info.end);
   const uint64_t remaining=bits<begin.declared_bits?begin.declared_bits-bits:0;
   const unsigned expected=unsigned(std::min<uint64_t>(remaining,16));
   if(e.index!=words||e.valid_bits!=expected||!expected){malformed=true;return;}
   const uint16_t word=uint16_t(e.word & uint16_t(0xffffu<<(16-expected)));
   payload.put(char(word>>8));payload.put(char(word&255));checked(payload);++words;bits+=expected;
  }else if(active){
   const bool identity=e.info.pc==begin.info.pc&&e.info.pd==begin.info.pd;
   end(e.info,e.supported&&identity,"unconfirmed_incomplete_or_damaged_payload");
  }
 }
};
BurstWriter::BurstWriter(const fs::path& p):impl(new Impl(p)){}
BurstWriter::~BurstWriter()=default;
void BurstWriter::burst(const BurstEvent& e){impl->event(e);}
void BurstWriter::finish(){if(impl->done)return;if(impl->active){auto info=impl->begin.info;info.end=impl->last_end;impl->end(info,false,"end_of_input_before_confirmation");}impl->index<<"{\"type\":\"finish\",\"complete\":true}\n";impl->index.flush();checked(impl->index);impl->index.close();checked(impl->index);impl->done=true;fs::remove(impl->dir/"INCOMPLETE");}
namespace {
const char* dataset_names[]={"subframes.csv","channel-status.csv","diagnostics.csv","content.csv"};
void manifest(const fs::path& dir,const BundleSpec& spec,const std::array<CsvSource,4>& sources,const BundleResult& result,const std::string& reason){
 const auto temp=dir/"manifest.json.tmp";
 std::ofstream out(temp,std::ios::binary);out.imbue(std::locale::classic());
 out<<"{\n  \"schema_version\":"<<quote(spec.schema_version)<<",\"analyzer_version\":"<<quote(spec.analyzer_version)<<",\"input_channel\":"<<quote(spec.input_channel)<<",\"capture_rate\":"<<std::setprecision(17)<<spec.capture_rate<<",\"settings\":"<<spec.settings_json<<",\"coverage_start_sample\":"<<quote(std::to_string(spec.coverage_start))<<",\"coverage_end_sample\":"<<quote(std::to_string(spec.coverage_end))<<",\"complete\":"<<(result.complete?"true":"false")<<",\"state\":"<<quote(reason)<<",\"datasets\":[";
 for(size_t i=0;i<4;++i){if(i)out<<',';out<<"{\"file\":"<<quote(dataset_names[i])<<",\"snapshot_rows\":"<<sources[i].row_count<<",\"written_rows\":"<<result.rows_written[i]<<'}';}
 out<<"]\n}\n";out.flush();checked(out);out.close();checked(out);
 // INCOMPLETE remains throughout replacement. A consumer must require both a
 // complete manifest and absence of this marker; failures cannot look complete.
 if(fs::exists(dir/"manifest.json"))fs::remove(dir/"manifest.json");
 fs::rename(temp,dir/"manifest.json");
}
}
BundleResult write_bundle(const fs::path& dir,const BundleSpec& spec,const std::array<CsvSource,4>& sources,std::function<bool()> cancelled){
 if(!(spec.capture_rate>0)||!std::isfinite(spec.capture_rate)||spec.coverage_end<spec.coverage_start)throw std::invalid_argument("Invalid bundle capture metadata");
 if(spec.settings_json.empty()||spec.settings_json.front()!='{'||spec.settings_json.back()!='}')throw std::invalid_argument("settings_json must be a JSON object");
 for(const auto& s:sources)if(s.row_count&&!s.write_row)throw std::invalid_argument("Missing bundle source cursor");
 new_dir(dir);BundleResult result;
 manifest(dir,spec,sources,result,"incomplete");
 try{
  for(size_t kind=0;kind<4;++kind){
   std::ofstream out(dir/dataset_names[kind],std::ios::binary);out.imbue(std::locale::classic());out<<sources[kind].header<<'\n';checked(out);
   for(uint64_t row=0;row<sources[kind].row_count;++row){
    if(cancelled&&cancelled()){out.flush();checked(out);manifest(dir,spec,sources,result,"cancelled");return result;}
    sources[kind].write_row(row,out);out<<'\n';checked(out);++result.rows_written[kind];
   }
   out.flush();checked(out);out.close();checked(out);
  }
  if(cancelled&&cancelled()){manifest(dir,spec,sources,result,"cancelled");return result;}
  result.complete=true;manifest(dir,spec,sources,result,"complete");fs::remove(dir/"INCOMPLETE");return result;
 }catch(...){result.complete=false;try{manifest(dir,spec,sources,result,"failed");}catch(...){}throw;}
}
}
