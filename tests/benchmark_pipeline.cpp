// Portable pipeline scaling benchmark. Fixture generation is either a separate
// process/file or explicitly included in online timings; no SDK is simulated.
#include "core/Decoder.h"
#include "codec/Codec.h"
#include "analysis/Analysis.h"
#include "export/Exports.h"
#include "report/Json.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif
namespace fs=std::filesystem;using Clock=std::chrono::steady_clock;
namespace {
constexpr uint64_t HeaderBytes=56,DefaultLimit=512ull*1024*1024;
const char* scenarios[]={"clean","faults","transitions","burst"};
uint64_t integer(const std::string& s){size_t n=0;if(s.empty()||s[0]=='-')throw std::invalid_argument("Invalid integer");auto v=std::stoull(s,&n);if(n!=s.size())throw std::invalid_argument("Invalid integer");return v;}
std::string memory_method;
uint64_t peak_rss(){
#ifdef _WIN32
 memory_method="Windows PeakWorkingSetSize";
 PROCESS_MEMORY_COUNTERS c{};if(!GetProcessMemoryInfo(GetCurrentProcess(),&c,sizeof(c)))throw std::runtime_error("Cannot query process peak working set");return c.PeakWorkingSetSize;
#else
#ifdef __linux__
 // ru_maxrss may include the Python launcher's pre-exec RSS. VmHWM belongs
 // to this executable's memory map and avoids that inherited measurement floor.
 std::ifstream status("/proc/self/status");std::string line;
 while(std::getline(status,line))if(line.rfind("VmHWM:",0)==0){std::istringstream value(line.substr(6));uint64_t kib=0;if(value>>kib){memory_method="Linux proc VmHWM";return kib*1024;}}
#endif
 memory_method="getrusage ru_maxrss";
 rusage r{};if(getrusage(RUSAGE_SELF,&r))throw std::runtime_error("Cannot query peak RSS");
#ifdef __APPLE__
 return uint64_t(r.ru_maxrss);
#else
 return uint64_t(r.ru_maxrss)*1024;
#endif
#endif
}
void put(std::ostream& out,uint64_t n,unsigned bytes){for(unsigned i=0;i<bytes;++i)out.put(char((n>>(8*i))&255));if(!out)throw std::runtime_error("Fixture write failed");}
uint64_t get(std::istream& in,unsigned bytes){uint64_t n=0;for(unsigned i=0;i<bytes;++i){int b=in.get();if(b==EOF)throw std::runtime_error("Truncated fixture");n|=uint64_t(uint8_t(b))<<(8*i);}return n;}
struct Header {uint64_t capture=0,nanos=0,edges=0,end=0,frames=0;unsigned rate=0,scenario=0;};
void write_header(std::ostream& out,const Header& h){out.write("SPDBEN01",8);put(out,h.capture,8);put(out,h.nanos,8);put(out,h.rate,4);put(out,h.scenario,4);put(out,h.edges,8);put(out,h.end,8);put(out,h.frames,8);}
Header read_header(std::istream& in){char magic[8];in.read(magic,8);if(std::string(magic,8)!="SPDBEN01")throw std::runtime_error("Unsupported fixture format");Header h;h.capture=get(in,8);h.nanos=get(in,8);h.rate=unsigned(get(in,4));h.scenario=unsigned(get(in,4));h.edges=get(in,8);h.end=get(in,8);h.frames=get(in,8);if(!h.capture||h.capture>UINT32_MAX||h.scenario>3||!h.edges||!h.frames||!h.end||!h.nanos||h.nanos>3600000000000ull||(h.rate!=48000&&h.rate!=96000&&h.rate!=192000)||h.end>h.capture*3601)throw std::runtime_error("Invalid fixture header");return h;}
// Same independent encoder-produced reference bytes used by codec regression
// tests: actual AC3/DTS frames, not an invented codec header or silent mock.
std::vector<uint8_t> reference(const char* text){std::vector<uint8_t> out;for(size_t i=0;text[i];i+=2)out.push_back(uint8_t(std::stoul(std::string(text+i,2),nullptr,16)));return out;}
const auto Ac3=reference("0b773f11004043e106f00108080821010104571f3e7cf9f3e7cf9f3e7cfbfe757cf9f3e7cf9f3e7cf8ff9d5f3e7cf9f3e7cf9f3e32fede4c99200000f1e35ad680035ac3000000078f1ad6b4001ad6180000003c78d6b5a000d6b0c0000001e3c6b5ad0006b5860000000f1e35ad680035ac3000000078f1ad6b4001ad60e48e");
const auto Dts=reference("7ffe8001fc3c1ff0b54001380003ef7fe006c91e1e0db6db6000000000200000000000000001eebae5cb972ebaebdef7bdb3355000001eebae5cb972ebaebdef7bdb335500000000000000000000000020408102040810200000000000000000000000000000000000000000204081020408102000000000000000000000000000000000000002492492222222222222222000000000000000000000000000000000000000000000000000000000000000022222222222222222222222224924924924924924924924924924915556aaaaaaa80000000000000000000000000002492492222222222222222000000000000000000000000000000000000000000000000000000000000000022222222222222222222222224924924924924924924924924924915556aaaaaaa80000000000000000000000000002492492222222222222222000000000000000000000000000000000000000000000000000000000000000022222222222222222222222224924924924924924924924924924915556aaaaaaa80000000000000000000000000002492492222222222222222000000000000000000000000000000000000000000000000000000000000000022222222222222222222222224924924924924924924924924924915556aaaaaaa80000000000000000000000000003fffc000000000000000000000000000000");
struct Generator {
 Header header;double at=100;bool level=false,gapped=false;uint64_t ideal_edges=0,last=0;unsigned phase=0;
 std::function<void(uint64_t)> consume;
 Generator(unsigned rate,double seconds,unsigned scenario,std::function<void(uint64_t)> callback,uint64_t capture_override=0):consume(std::move(callback)){
  header.rate=rate;header.scenario=scenario;const unsigned fastest=std::max(rate,scenario==2?(rate==192000?48000:rate*2):rate);header.capture=capture_override?capture_override:(fastest<=96000?100000000:250000000);if(header.capture>UINT32_MAX||header.capture<uint64_t(fastest)*512)throw std::invalid_argument("Capture rate must provide at least four samples per half-cell");header.nanos=uint64_t(std::llround(seconds*1e9));
 }
 void edge(uint64_t sample){
  ++ideal_edges;
  if(header.scenario==1 && ideal_edges%257==0)return;
  emit(sample);
  if(header.scenario==1&&ideal_edges%509==0)emit(sample+1);
 }
 void emit(uint64_t sample){if(header.edges&&sample<=last)throw std::runtime_error("Generator produced nonmonotonic edges");consume(sample);last=sample;++header.edges;}
 void cell(bool bit,double half){if(bit!=level){level=bit;edge(uint64_t(std::llround(at)));}at+=half;}
 void frame(unsigned rate,bool nonaudio,unsigned format){
  for(unsigned channel=0;channel<2;++channel){
   const unsigned position=unsigned(header.frames%192);const uint8_t pre=channel?0xe4:(position?0xe2:0xe8);const bool invert=level;const double half=double(header.capture)/(128*rate);
   for(int bit=7;bit>=0;--bit)cell(bool((pre>>bit)&1)^invert,half);
   std::array<uint8_t,24> status{};status[0]=nonaudio?6:4;status[4]=nonaudio?0:11;
   switch(rate){case 48000:status[3]=2;break;case 96000:status[3]=10;break;case 192000:status[3]=14;break;default:throw std::runtime_error("Unsupported benchmark rate");}
   uint32_t payload=0;
   if(nonaudio){
    const unsigned period=format==1?1536:512;
    const auto n=header.frames>=384?2*((header.frames-384)%period)+channel:UINT64_MAX;
    const auto& bytes=format==1?Ac3:Dts;
    uint16_t word=0;
    if(n==0)word=0xf872;else if(n==1)word=0x4e1f;else if(n==2)word=uint16_t(format);else if(n==3)word=uint16_t(bytes.size()*8);
    else if(n>=4&&n<4+(bytes.size()+1)/2){const size_t pos=size_t(n-4)*2;word=uint16_t(uint16_t(bytes[pos])<<8);if(pos+1<bytes.size())word|=bytes[pos+1];}
    if(header.frames>=384)payload=uint32_t(word)<<8;
   }else {int32_t value=int32_t((header.frames*257)&0x7fffff);if(channel)value=-value;payload=uint32_t(value)&0xffffff;}
   uint32_t raw=payload;if(nonaudio)raw|=1u<<24;if((status[position/8]>>(position%8))&1)raw|=1u<<26;
   unsigned ones=0;for(unsigned i=0;i<27;++i)ones+=(raw>>i)&1;raw|=(ones&1)<<27;
   if(header.scenario==1&&(2*header.frames+channel)%17==0)raw^=1u<<27;
   for(unsigned i=0;i<28;++i){cell(!level,half);cell(((raw>>i)&1)?!level:level,half);}
  }
  ++header.frames;
 }
 Header run(){const double stop=double(header.nanos)*header.capture/1e9;
  while(at<stop){
   if(header.frames%192==0){
    if(header.scenario==1&&!gapped&&at>=stop/2){at+=header.capture*.002;gapped=true;}
    if(header.scenario==2){if(at>=stop*2/3)phase=2;else if(at>=stop/3)phase=1;}
   }
   const unsigned rate=header.scenario==2&&phase?(header.rate==192000?48000:header.rate*2):header.rate;
   frame(rate,header.scenario==3||(header.scenario==2&&phase),phase==2?11:1);
  }
  // One boundary edge closes the last complete subframe without inventing a
  // following complete preamble or payload.
  emit(uint64_t(std::llround(at)));header.end=last+1;return header;
 }
};
struct Counts {uint64_t codec_records=0,codec_structure_passed=0,codec_crc_passed=0,subframes=0,status=0,diagnostics=0,content=0,words=0,health=0,changes=0,pcm=0,checksum=1469598103934665603ull;};
void csv_string(std::ostream& out,const std::string& s){out<<'"';for(char c:s){if(c=='"')out<<'"';out<<c;}out<<'"';}
struct Pipeline {
 Header header;Counts counts;bool retain=false;std::vector<spdif::Subframe> frames;std::vector<spdif::StatusBlock> status;std::vector<spdif::Diagnostic> diagnostics;std::vector<spdif::ContentInfo> content;
 std::unique_ptr<spdif::exports::WavWriter> wav;std::unique_ptr<spdif::exports::BurstWriter> bursts;std::unique_ptr<spdif::Analysis> analysis;std::unique_ptr<spdif::Decoder> decoder;std::unique_ptr<spdif::codec::Inspector> inspector;
 explicit Pipeline(Header h,const std::string& output):header(h),retain(!output.empty()){
  if(retain){if(fs::exists(output)||!fs::create_directories(output))throw std::runtime_error("Pipeline output requires new directory");wav=std::make_unique<spdif::exports::WavWriter>(fs::path(output)/"wav",spdif::exports::WavConfig{double(h.capture)});bursts=std::make_unique<spdif::exports::BurstWriter>(fs::path(output)/"bursts");}
  inspector=std::make_unique<spdif::codec::Inspector>([&](const spdif::codec::CodecInfo& info){++counts.codec_records;counts.codec_structure_passed+=info.frame.structure==spdif::IntegrityCheck::Passed;counts.codec_crc_passed+=info.frame.frame_crc==spdif::IntegrityCheck::Passed;});
  spdif::AnalysisSink a;a.health=[&](const auto&){++counts.health;};a.status_change=[&](const auto&){++counts.changes;};a.pcm=[&](const auto&){++counts.pcm;};analysis=std::make_unique<spdif::Analysis>(spdif::AnalysisConfig{double(h.capture),.1,0,0},a);
  spdif::Sink sink;sink.subframe=[&](const auto& f){++counts.subframes;counts.checksum^=f.start;counts.checksum*=1099511628211ull;counts.checksum^=f.raw;counts.checksum*=1099511628211ull;analysis->add(f);if(retain){frames.push_back(f);wav->subframe(f);}};
  sink.status=[&](const auto& b){++counts.status;analysis->add(b);if(retain){status.push_back(b);wav->status(b);}};
  sink.diagnostic=[&](const auto& d){++counts.diagnostics;analysis->add(d);if(retain){diagnostics.push_back(d);wav->diagnostic(d);}};
  sink.content=[&](const auto& c){++counts.content;analysis->add(c);if(retain)content.push_back(c);};
  sink.burst=[&](const auto& event){if(event.phase==spdif::BurstPhase::Word)++counts.words;inspector->burst(event);if(retain)bursts->burst(event);};decoder=std::make_unique<spdif::Decoder>(spdif::Config{double(h.capture),0},sink);
 }
 void finish(uint64_t end){decoder->finish(end-1);inspector->finish();analysis->finish(end);if(retain){wav->finish(end);bursts->finish();}}
 uint64_t retained_bytes()const{return frames.capacity()*sizeof(spdif::Subframe)+status.capacity()*sizeof(spdif::StatusBlock)+diagnostics.capacity()*sizeof(spdif::Diagnostic)+content.capacity()*sizeof(spdif::ContentInfo);}
 spdif::exports::BundleResult bundle(const std::string& output,uint64_t cancel_after){
  std::array<spdif::exports::CsvSource,4> source;
  source[0].header="start_seconds,end_seconds,channel,preamble,block_position,raw_slots_4_31,payload_hex,signed_container_24,main_field_raw,auxiliary,V_invalid,U,C,P,parity_ok,measured_sample_rate_hz,sample_meaning,content,layout_bits,layout_known,layout_stereo,layout_professional";
  source[0].row_count=frames.size();source[0].write_row=[&](uint64_t i,std::ostream& out){const auto& f=frames.at(size_t(i));out<<std::setprecision(12)<<double(f.start)/header.capture<<','<<double(f.end)/header.capture<<','<<(f.channel?'B':'A')<<','<<f.preamble<<','<<f.block_position<<','<<f.raw<<','<<std::hex<<(f.raw&0xffffff)<<std::dec<<','<<f.sample<<','<<((f.raw>>4)&0xfffff)<<','<<(f.raw&15)<<','<<f.validity<<','<<f.user<<','<<f.status<<','<<f.parity<<','<<f.parity_ok<<','<<f.sample_rate<<",numeric container only,";csv_string(out,spdif::content_name(f.content));out<<','<<f.layout.bits<<','<<f.layout.known<<','<<f.layout.stereo<<','<<f.layout.professional;};
  source[1].header="start_sample,end_sample,channel,integrity,C_hex,U_hex,interpretation";source[1].row_count=status.size();source[1].write_row=[&](uint64_t i,std::ostream& out){const auto& b=status.at(size_t(i));out<<b.start<<','<<b.end<<','<<b.channel<<','<<b.integrity<<','<<spdif::json::hex(b.c)<<','<<spdif::json::hex(b.u)<<',';csv_string(out,b.interpretation);};
  source[2].header="start_sample,end_sample,code,message";source[2].row_count=diagnostics.size();source[2].write_row=[&](uint64_t i,std::ostream& out){const auto& d=diagnostics.at(size_t(i));out<<d.start<<','<<d.end<<',';csv_string(out,d.code);out<<',';csv_string(out,d.message);};
  source[3].header="start_sample,end_sample,format,Pc,Pd,complete,integrity,evidence";source[3].row_count=content.size();source[3].write_row=[&](uint64_t i,std::ostream& out){const auto& c=content.at(size_t(i));out<<c.start<<','<<c.end<<',';csv_string(out,spdif::content_name(c.format));out<<','<<c.pc<<','<<c.pd<<','<<c.complete<<','<<c.integrity<<',';csv_string(out,c.evidence);};
  spdif::exports::BundleSpec spec;spec.capture_rate=double(header.capture);spec.input_channel="synthetic";spec.coverage_end=header.end-1;spec.settings_json="{\"benchmark\":true,\"analysis_window_seconds\":0.1}";
  uint64_t visited=0;for(auto& s:source){auto original=s.write_row;s.write_row=[&,original](uint64_t i,std::ostream& out){original(i,out);++visited;};}
  return spdif::exports::write_bundle(fs::path(output)/"bundle",spec,source,[&]{return cancel_after&&visited>=cancel_after;});
 }
};
double elapsed(Clock::time_point start){return std::chrono::duration<double>(Clock::now()-start).count();}
uint64_t output_bytes(const std::string& path){uint64_t n=0;if(!path.empty())for(const auto& f:fs::recursive_directory_iterator(path))if(f.is_regular_file())n+=f.file_size();return n;}
}
int main(int argc,char** argv){try{
 std::string generate,input,online,output,mode="analysis",scenario="clean";unsigned rate=48000;double seconds=.1;uint64_t limit=DefaultLimit,cancel=0,capture=0;
 for(int i=1;i<argc;++i){std::string key=argv[i];auto value=[&](){if(++i>=argc)throw std::invalid_argument("Missing option value");return std::string(argv[i]);};if(key=="--generate")generate=value();else if(key=="--input")input=value();else if(key=="--online")online=value();else if(key=="--output")output=value();else if(key=="--mode")mode=value();else if(key=="--scenario")scenario=value();else if(key=="--rate"){auto n=integer(value());if(n>UINT32_MAX)throw std::invalid_argument("Rate overflow");rate=unsigned(n);}else if(key=="--seconds")seconds=std::stod(value());else if(key=="--capture-rate")capture=integer(value());else if(key=="--max-fixture-bytes")limit=integer(value());else if(key=="--cancel-after")cancel=integer(value());else throw std::invalid_argument("Unknown benchmark option: "+key);}
 if((rate!=48000&&rate!=96000&&rate!=192000)||!std::isfinite(seconds)||seconds<=0||seconds>3600||limit<HeaderBytes)throw std::invalid_argument("Invalid benchmark limits");
 unsigned scenario_id=4;for(unsigned i=0;i<4;++i)if(scenario==scenarios[i])scenario_id=i;if(scenario_id==4)throw std::invalid_argument("Unknown scenario");
 const unsigned selections=unsigned(!generate.empty())+unsigned(!input.empty())+unsigned(!online.empty());if(selections!=1)throw std::invalid_argument("Select exactly one of --generate, --input, --online");
 auto report=spdif::json::event("pipeline_benchmark");
#ifdef _MSC_VER
 report.text("compiler","MSVC "+std::to_string(_MSC_VER));
#else
 report.text("compiler",__VERSION__);
#endif
 report.text("compiled",__DATE__ " " __TIME__);
 if(!generate.empty()){
  if(fs::exists(generate))throw std::runtime_error("Fixture already exists");
  if(!fs::path(generate).parent_path().empty())fs::create_directories(fs::path(generate).parent_path());
  std::ofstream file(generate,std::ios::binary);Header placeholder;write_header(file,placeholder);const auto start=Clock::now();uint64_t written_bytes=HeaderBytes;
  Generator generator(rate,seconds,scenario_id,[&](uint64_t e){if(written_bytes>limit-8)throw std::runtime_error("Fixture byte limit exceeded");put(file,e,8);written_bytes+=8;},capture);auto h=generator.run();file.seekp(0);write_header(file,h);file.flush();if(!file)throw std::runtime_error("Fixture flush failed");file.close();
  report.text("mode","generate").text("scenario",scenario).integer("nominal_rate_hz",rate).integer("capture_rate_hz",h.capture).number("requested_seconds",seconds).number("capture_seconds",double(h.end)/h.capture).integer("edges",h.edges).integer("source_frames",h.frames).integer("fixture_bytes",HeaderBytes+8*h.edges).number("generation_seconds",elapsed(start)).integer("peak_rss_bytes",peak_rss()).text("peak_memory_method",memory_method);std::cout<<report.str()<<'\n';return 0;
 }
 Header h;std::ifstream file;
 if(!input.empty()){file.open(input,std::ios::binary);h=read_header(file);if(h.edges>(limit-HeaderBytes)/8||fs::file_size(input)!=HeaderBytes+h.edges*8)throw std::runtime_error("Fixture size/count exceeds limit or differs from header");}
 else {if(online!="analysis"&&online!="generator")throw std::invalid_argument("Online mode must be analysis or generator");Generator g(rate,seconds,scenario_id,[](uint64_t){},capture);h=g.header;mode=online;}
 if(mode!="analysis"&&mode!="exports"&&mode!="generator")throw std::invalid_argument("Invalid pipeline mode");
 if(mode=="exports"&&output.empty())throw std::invalid_argument("Export benchmark requires --output new-directory");
 std::unique_ptr<Pipeline> pipeline;if(mode!="generator")pipeline=std::make_unique<Pipeline>(h,mode=="exports"?output:"");const auto start=Clock::now();
 if(!input.empty()){for(uint64_t n=0;n<h.edges;++n){auto e=get(file,8);if(e>=h.end)throw std::runtime_error("Fixture edge exceeds declared source end");if(pipeline)pipeline->decoder->edge(e);}}
 else {Generator g(rate,seconds,scenario_id,[&](uint64_t e){if(pipeline)pipeline->decoder->edge(e);},capture);h=g.run();}
 if(pipeline){pipeline->header=h;pipeline->finish(h.end);}
 const double decode_seconds=elapsed(start);double bundle_seconds=0;bool bundle_complete=true;
 if(mode=="exports"){const auto bundle_start=Clock::now();bundle_complete=pipeline->bundle(output,cancel).complete;bundle_seconds=elapsed(bundle_start);}
 const Counts c=pipeline?pipeline->counts:Counts{};
 if(pipeline&&h.scenario==0&&c.subframes!=2*h.frames)throw std::runtime_error("Clean benchmark lost decoded subframes");
 report.text("mode",mode).text("source",input.empty()?"online-generated":"file").text("scenario",scenarios[h.scenario]).integer("nominal_rate_hz",h.rate).integer("capture_rate_hz",h.capture).number("requested_seconds",double(h.nanos)/1e9).number("capture_seconds",double(h.end)/h.capture).integer("edges",h.edges).integer("source_frames",h.frames).integer("subframes",c.subframes).integer("status_records",c.status).integer("diagnostics",c.diagnostics).integer("content_records",c.content).integer("payload_words",c.words).integer("codec_records",c.codec_records).integer("codec_structure_passed",c.codec_structure_passed).integer("codec_crc_passed",c.codec_crc_passed).integer("health_windows",c.health).integer("pcm_records",c.pcm).integer("status_changes",c.changes).text("result_checksum",std::to_string(c.checksum)).number(input.empty()?"generation_and_pipeline_seconds":"decode_analysis_export_seconds",decode_seconds).number("bundle_seconds",bundle_seconds).number("capture_seconds_per_wall_second",(double(h.end)/h.capture)/(decode_seconds+bundle_seconds)).integer("peak_rss_bytes",peak_rss()).text("peak_memory_method",memory_method).integer("retained_history_capacity_bytes",pipeline?pipeline->retained_bytes():0).integer("retained_subframe_capacity_bytes",pipeline?pipeline->frames.capacity()*sizeof(spdif::Subframe):0).integer("output_bytes",output_bytes(mode=="exports"?output:"")).boolean("bundle_complete",bundle_complete).integer("cancel_after_rows",cancel).integer("fixture_bytes",input.empty()?0:HeaderBytes+8*h.edges);
 std::cout<<report.str()<<'\n';return 0;
 }catch(const std::exception& e){std::cerr<<"benchmark_pipeline: "<<e.what()<<'\n';return 2;}}
