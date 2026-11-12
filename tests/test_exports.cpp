#include "export/Exports.h"
#include "ContentTests.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <locale>
#include <stdexcept>
#include <sstream>
#include <vector>
using namespace spdif;using namespace spdif::exports;namespace fs=std::filesystem;
namespace {
unsigned checks=0;
void expect(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
std::string read(const fs::path& p){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("Missing test output "+p.string());return {std::istreambuf_iterator<char>(f),{}};}
uint32_t le(const std::string& s,size_t i,unsigned n){uint32_t x=0;for(unsigned b=0;b<n;++b)x|=uint32_t(uint8_t(s.at(i+b)))<<(8*b);return x;}
size_t files(const fs::path& p,const std::string& ext){size_t n=0;for(auto& e:fs::directory_iterator(p))if(e.path().extension()==ext)++n;return n;}
Subframe sample(uint64_t n,uint32_t raw,unsigned bits=24){Subframe f;f.start=100+n*512;f.end=f.start+511;f.channel=unsigned(n%2);f.raw=raw;f.parity_ok=true;f.content=ContentFormat::Pcm;f.layout={bits,true,true,false};return f;}
void status(WavWriter& w,uint8_t rate=2){for(unsigned ch=0;ch<2;++ch){StatusBlock b;b.channel=ch;b.c[3]=rate;b.c[4]=0xb;w.status(b);}}
void pair(WavWriter& w,uint64_t n,uint32_t a=0,uint32_t b=0,unsigned bits=24){w.subframe(sample(n,a,bits));w.subframe(sample(n+1,b,bits));}
BurstEvent begin(unsigned type=1,unsigned declared=20){BurstEvent e;e.phase=BurstPhase::Begin;e.info.start=9007199254741100ull;e.info.end=e.info.start+3;e.info.format=type==21?ContentFormat::Eac3:ContentFormat::Ac3;e.info.pc=uint16_t(type|(3<<13));e.info.pd=uint16_t(type==21?declared/8:declared);e.declared_bits=declared;e.supported=true;e.info.burst=true;e.info.integrity=true;e.info.evidence="confirmed non-audio";return e;}
void word(BurstWriter& w,const BurstEvent& b,unsigned i,uint16_t value,unsigned valid=16){auto e=b;e.phase=BurstPhase::Word;e.index=i;e.word=value;e.valid_bits=uint8_t(valid);e.info.end+=i+1;w.burst(e);}
void end(BurstWriter& w,BurstEvent e,bool good=true){e.phase=BurstPhase::End;e.info.end+=10;e.info.complete=true;e.info.integrity=good;w.burst(e);}
void wav_tests(const fs::path& root){
 {WavWriter w(root/"wav24",{49152000});status(w);pair(w,0,0x800000,0x7fffff);pair(w,2,0xffffff,1);w.finish(2148);}
 auto s=read(root/"wav24"/"segment-000001.wav");
 expect(s.size()==80,"WAV two stereo frames exact size");expect(s.substr(0,4)=="RIFF"&&s.substr(8,8)=="WAVEfmt ","WAV RIFF/fmt identity");expect(le(s,4,4)==72&&le(s,16,4)==40,"RIFF/fmt lengths");expect(le(s,20,2)==0xfffe&&le(s,22,2)==2,"Extensible stereo format");expect(le(s,24,4)==48000&&le(s,28,4)==288000&&le(s,32,2)==6,"Sample rate and alignment");expect(le(s,34,2)==24&&le(s,38,2)==24&&le(s,40,4)==3,"Container/effective bits and A/B speaker mask");expect(s.substr(60,4)=="data"&&le(s,64,4)==12,"Data size");expect(le(s,68,3)==0x800000&&le(s,71,3)==0x7fffff&&le(s,74,3)==0xffffff&&le(s,77,3)==1,"Independent signed extrema and A/B byte order");expect(!fs::exists(root/"wav24"/"INCOMPLETE"),"Finished WAV marker removed");
 for(unsigned bits:{16u,18u,20u,24u}){
  auto p=root/("wav"+std::to_string(bits)+"pad");WavWriter w(p,{49152000});status(w);pair(w,0,0x80000f,0x7fffff,bits);w.finish(1124);auto b=read(p/"segment-000001.wav");const uint32_t mask=0xffffffu<< (24-bits);expect(le(b,68,3)==(0x80000f&mask),"Declared layout strips unused low auxiliary bits");expect(le(b,71,3)==(0x7fffff&mask),"Positive full scale left aligned");expect(le(b,38,2)==bits,"Valid bits explicit");
 }
 {WavWriter w(root/"splits",{49152000});status(w);pair(w,0);auto bad=sample(2,0);bad.validity=true;w.subframe(bad);w.subframe(sample(3,0));pair(w,4);pair(w,8);status(w,0);pair(w,10);pair(w,12,0,0,20);w.finish(7268);}
 auto idx=read(root/"splits"/"index.jsonl");expect(files(root/"splits",".wav")==5,"Exclusion/gap/rate/layout each split");expect(idx.find("validity_invalid")!=std::string::npos&&idx.find("unpaired_channel_B")!=std::string::npos&&idx.find("gap")!=std::string::npos,"Reasons explicit");expect(read(root/"splits"/"segment-000004.wav").size()==74,"Rate segment exact frame count");expect(le(read(root/"splits"/"segment-000004.wav"),24,4)==44100,"Declared new integer rate used");
 {WavWriter w(root/"exclude",{49152000});status(w);for(unsigned i=0;i<6;++i){auto f=sample(i,0);if(i<2)f.content=ContentFormat::Ac3;else if(i<4)f.parity_ok=false;else f.content=ContentFormat::Unknown;w.subframe(f);}w.subframe(sample(6,0));w.finish(4000);}
 expect(files(root/"exclude",".wav")==0,"NonPCM/unknown/parity/unpaired do not become audio");idx=read(root/"exclude"/"index.jsonl");expect(idx.find("not_confirmed_pcm")!=std::string::npos&&idx.find("parity_error")!=std::string::npos&&idx.find("unpaired_end_of_input")!=std::string::npos,"Exclusion evidence retained");
 {WavWriter w(root/"unknownrate",{49152000});pair(w,0);w.finish(1124);}expect(files(root/"unknownrate",".wav")==0&&read(root/"unknownrate"/"index.jsonl").find("sample_rate_unknown")!=std::string::npos,"Unknown rate requires explicit override");
 {WavWriter w(root/"override",{49152000,16,48000});auto a=sample(0,0xff00ff),b=sample(1,0x7fffab);a.layout.known=b.layout.known=false;a.layout.bits=b.layout.bits=0;w.subframe(a);w.subframe(b);w.finish(1124);}
 s=read(root/"override"/"segment-000001.wav");expect(le(s,68,3)==0xff0000&&le(s,71,3)==0x7fff00&&le(s,38,2)==16,"Override only precision, correct signed audio");idx=read(root/"override"/"index.jsonl");expect(idx.find("\"bits_overridden\":true")!=std::string::npos&&idx.find("\"rate_overridden\":true")!=std::string::npos,"Explicit override recorded");
 {WavWriter w(root/"preservedeclared",{49152000,16,48000});pair(w,0,1,2,24);w.finish(1124);}expect(le(read(root/"preservedeclared"/"segment-000001.wav"),38,2)==24,"Override does not replace known precision");
 {WavWriter w(root/"notstereo",{49152000,0,48000});auto a=sample(0,0),b=sample(1,0);a.layout.stereo=b.layout.stereo=false;w.subframe(a);w.subframe(b);w.finish(1124);}expect(files(root/"notstereo",".wav")==0,"Independent channels not silently stereo");
 {WavWriter w(root/"assumestereo",{49152000,0,48000,true});auto a=sample(0,0),b=sample(1,0);a.layout.stereo=b.layout.stereo=false;w.subframe(a);w.subframe(b);w.finish(1124);}expect(files(root/"assumestereo",".wav")==1,"Explicit stereo assumption supported");
 {WavWriter w(root/"unfinished",{49152000});status(w);pair(w,0);}expect(fs::exists(root/"unfinished"/"INCOMPLETE"),"Unfinished stream is explicit");
 {WavWriter w(root/"override-declared-change",{49152000,0,48000});status(w);pair(w,0);status(w,0);pair(w,2);w.finish(2148);}expect(files(root/"override-declared-change",".wav")==2,"Rate override does not conceal changed source rate");expect(read(root/"override-declared-change"/"index.jsonl").find("\"source_declared_rate_A\":44100")!=std::string::npos,"New source rate recorded despite override");
 {WavWriter w(root/"quantization",{49152000});status(w);for(unsigned i=0;i<8;++i){auto f=sample(i,i);f.sample_rate=(i%2)?47984:48030;w.subframe(f);}w.finish(4196);}expect(files(root/"quantization",".wav")==1,"Sampled clock quantization does not fragment nominal-rate audio");
 {WavWriter w(root/"stereo-override-nonpcm",{49152000,16,48000,true});auto a=sample(0,0),b=sample(1,0);a.content=b.content=ContentFormat::NonPcm;w.subframe(a);w.subframe(b);w.finish(1124);}expect(files(root/"stereo-override-nonpcm",".wav")==0,"Overrides never bypass nonPCM gate");
 {WavWriter w(root/"status-reset",{49152000});status(w);pair(w,0);w.diagnostic({1124,1124,"sync_loss","fixture"});pair(w,2);status(w);pair(w,4);w.finish(3172);}expect(files(root/"status-reset",".wav")==2,"Discontinuity clears declared-rate baseline until fresh status");
 for(double invalid_rate:{1e30,double(UINT32_MAX)+1.0,std::numeric_limits<double>::infinity()}){bool rejected=false;try{WavWriter w(root/"badrate",{invalid_rate});}catch(const std::invalid_argument&){rejected=true;}expect(rejected,"Unrepresentable capture rate rejected before output creation");}
 {WavWriter w(root/"exclusive-end",{49152000});status(w);pair(w,0);bool rejected=false;try{w.finish(1123);}catch(const std::invalid_argument&){rejected=true;}expect(rejected,"Inclusive final sample is not a valid exclusive end");expect(fs::exists(root/"exclusive-end"/"INCOMPLETE"),"Rejected endpoint cannot complete an export");w.finish(1124);expect(read(root/"exclusive-end"/"index.jsonl").find("\"end_exclusive_sample\":\"1124\"")!=std::string::npos,"Finish index records exact exclusive endpoint");}
 {WavWriter w(root/"backwards-after-gap",{49152000});status(w);pair(w,2);w.diagnostic({2148,2148,"sync_loss","fixture"});bool rejected=false;try{pair(w,0);}catch(const std::invalid_argument&){rejected=true;}expect(rejected,"Discontinuity never permits timestamps to move backwards");w.finish(2148);}
 bool thrown=false;try{WavWriter w(root/"badbits",{49152000,15});}catch(const std::invalid_argument&){thrown=true;}expect(thrown,"Reject out of range override");
}
void burst_tests(const fs::path& root){
 {BurstWriter w(root/"burst");auto b=begin();w.burst(b);word(w,b,0,0x1234);word(w,b,1,0xabcd,4);end(w,b);w.finish();}
 auto s=read(root/"burst"/"burst-000001.bin");expect(s==std::string("\x12\x34\xa0\x00",4),"BE transport words and high partial bits independently verified");auto idx=read(root/"burst"/"index.jsonl");expect(idx.find("\"declared_bits\":20")!=std::string::npos&&idx.find("\"final_word_valid_bits\":4")!=std::string::npos&&idx.find("\"stream_number\":3")!=std::string::npos,"Exact length/stream in index");expect(idx.find("9007199254741100")!=std::string::npos,"64bit timestamps not rounded");
 {BurstWriter w(root/"eac3");auto b=begin(21,24);w.burst(b);word(w,b,0,0xf872);word(w,b,1,0x4e1f,8);end(w,b);w.finish();}expect(read(root/"eac3"/"burst-000001.bin")==std::string("\xf8\x72\x4e\0",4),"EAC3 Pd bytes converted to bits, sync-like payload not treated as delimiter");
 for(unsigned failure=0;failure<8;++failure){auto p=root/("reject"+std::to_string(failure));BurstWriter w(p);auto b=begin();if(failure==0)b.supported=false;if(failure==1)b.info.pc|=128;if(failure==2)b.declared_bits=19;w.burst(b);word(w,b,0,0x1234);if(failure!=3)word(w,b,1,0xffff,failure==4?3:4);if(failure==5){b.info.format=ContentFormat::Provisional;end(w,b);}else if(failure==6)end(w,b,false);else if(failure!=7)end(w,b);w.finish();expect(files(p,".bin")==0&&files(p,".part")==0,"Rejected/incomplete burst has no apparently valid payload");expect(read(p/"index.jsonl").find("\"accepted\":false")!=std::string::npos,"Rejected burst indexed");}
 {BurstWriter w(root/"extended-type");auto b=begin();b.info.pc=uint16_t((b.info.pc&~127u)|0x21);w.burst(b);word(w,b,0,0x1234);word(w,b,1,0xabcd,4);end(w,b);w.finish();}expect(files(root/"extended-type",".bin")==0,"Seven-bit unsupported type cannot alias AC3");
 {BurstWriter w(root/"two");auto a=begin();w.burst(a);word(w,a,0,1);auto b=begin();b.info.start+=100;w.burst(b);word(w,b,0,0x1234);word(w,b,1,0xf123,4);end(w,b);w.finish();}expect(files(root/"two",".bin")==1&&read(root/"two"/"index.jsonl").find("new_header_before_payload_end")!=std::string::npos,"New header rejects old pending burst");
}
class HostileNumbers final : public std::numpunct<char> {
 char do_decimal_point() const override { return ','; }
 char do_thousands_sep() const override { return '#'; }
 std::string do_grouping() const override { return "\1"; }
};
class GlobalLocaleGuard {
 std::locale previous;
public:
 GlobalLocaleGuard():previous(std::locale()) {std::locale::global(std::locale(std::locale::classic(),new HostileNumbers));}
 ~GlobalLocaleGuard(){std::locale::global(previous);}
 GlobalLocaleGuard(const GlobalLocaleGuard&)=delete;
 GlobalLocaleGuard& operator=(const GlobalLocaleGuard&)=delete;
};
void locale_tests(const fs::path& root){
 const auto previous=std::locale();
 {
  GlobalLocaleGuard guard;
  // Prove the facet is active rather than relying on installed OS locale names.
  std::ostringstream probe;probe<<1234<<' '<<12.5;
  expect(probe.str()=="1#2#3#4 1#2,5","Custom global numeric locale is active");
  const auto wavdir=root/"locale-wav";
  {WavWriter writer(wavdir,{49152000.5});status(writer);for(unsigned i=0;i<12;++i)pair(writer,i*4,0x800000,0x7fffff);writer.finish(23652);}
  auto text=read(wavdir/"index.jsonl");
  expect(text.find("\"capture_rate\":49152000.5")!=std::string::npos&&text.find("\"rate\":48000")!=std::string::npos,"WAV JSON numbers use classic decimal syntax");
  expect(fs::exists(wavdir/"segment-000012.wav"),"WAV filenames never inherit digit grouping");
  expect(text.find('#')==std::string::npos,"WAV index has no locale grouping separators");
  expect(le(read(wavdir/"segment-000012.wav"),24,4)==48000,"Locale does not change binary WAV rate");
  const auto burstdir=root/"locale-burst";
  {BurstWriter writer(burstdir);for(unsigned i=0;i<12;++i){auto b=begin();b.info.start+=100*i;b.info.end+=100*i;b.info.evidence="control:"+std::string(1,char(31));writer.burst(b);word(writer,b,0,0x1234);word(writer,b,1,0xabcd,4);end(writer,b);}writer.finish();}
  text=read(burstdir/"index.jsonl");
  expect(text.find("\"Pc\":24577")!=std::string::npos&&text.find("\"declared_bits\":20")!=std::string::npos,"Burst index JSON integer formatting is locale independent");
  expect(text.find("\\u001f")!=std::string::npos&&text.find('#')==std::string::npos,"JSON control escapes never contain locale punctuation");
  expect(fs::exists(burstdir/"burst-000012.bin"),"Payload filenames never inherit digit grouping");
  expect(read(burstdir/"burst-000012.bin")==std::string("\x12\x34\xa0\0",4),"Locale does not alter payload bytes");
  std::array<CsvSource,4> sources;for(auto& source:sources){source.header="integer,real";source.row_count=12;source.write_row=[](uint64_t row,std::ostream& out){out<<1234+row<<','<<12.5;};}
  BundleSpec spec;spec.capture_rate=49152000.5;spec.input_channel="D6";
  const auto result=write_bundle(root/"locale-bundle",spec,sources);
  expect(result.complete,"Bundle completes under a non-classic global locale");
  text=read(root/"locale-bundle"/"manifest.json");
  expect(text.find("\"capture_rate\":49152000.5")!=std::string::npos&&text.find("\"snapshot_rows\":12")!=std::string::npos&&text.find('#')==std::string::npos,"Bundle manifest numbers use JSON-compatible classic syntax");
  expect(read(root/"locale-bundle"/"subframes.csv").find("integer,real\n1234,12.5\n") == 0,"CSV source cursors receive classic-locale streams");
 }
 expect(std::locale()==previous,"Global locale restored after export regression");
}
void core_integration(const fs::path& root){
 {BurstWriter writer(root/"core-burst");auto signal=fixture::content_signal(1,20,2);Sink sink;sink.burst=[&](const BurstEvent& e){writer.burst(e);};Decoder decoder({signal.capture,0},sink);for(auto edge:signal.edges)decoder.edge(edge);decoder.finish(signal.edges.back());writer.finish();}
 expect(read(root/"core-burst"/"burst-000001.bin")==std::string("\x0b\x77\x50\0",4),"Actual core streaming contract yields independent two-word payload bytes");
 {fixture::Signal signal(100000000,48000);std::array<uint8_t,24> c{};c[3]=2;c[4]=0xb;signal.frames(400,0,&c,&c);WavWriter writer(root/"core-wav",{signal.capture});Sink sink;sink.status=[&](const StatusBlock& b){writer.status(b);};sink.diagnostic=[&](const Diagnostic& d){writer.diagnostic(d);};sink.subframe=[&](const Subframe& f){writer.subframe(f);};Decoder decoder({signal.capture,0},sink);for(auto edge:signal.edges)decoder.edge(edge);decoder.finish(signal.edges.back());writer.finish(signal.edges.back()+1);}
 auto s=read(root/"core-wav"/"segment-000001.wav");const unsigned i=192;
 expect(le(s,68,3)==((i*0x23451u)^0x800000u)%0x1000000u && le(s,71,3)==((i*0x23451u)^0xf65432u)%0x1000000u,"Core confirmed status/layout produces exact A/B samples");
}
void bundle_tests(const fs::path& root){
 std::array<CsvSource,4> sources;uint64_t calls=0;
 for(size_t i=0;i<4;++i){sources[i].header="n,value";sources[i].row_count=i+1;sources[i].write_row=[i,&calls](uint64_t n,std::ostream& out){++calls;out<<n<<",dataset-"<<i;};}
 BundleSpec spec;spec.input_channel="D6";spec.capture_rate=100000000;spec.settings_json="{\"detail\":\"Fields\"}";spec.coverage_start=9007199254740993ull;spec.coverage_end=9007199254741999ull;
 const auto r=write_bundle(root/"bundle",spec,sources);expect(r.complete&&calls==10,"One streaming cursor visit per snapshot row");expect(r.rows_written==std::array<uint64_t,4>{1,2,3,4},"Bundle all four row counts");auto text=read(root/"bundle"/"manifest.json");expect(text.find("\"complete\":true")!=std::string::npos&&text.find("9007199254740993")!=std::string::npos&&text.find("\"detail\":\"Fields\"")!=std::string::npos,"Complete manifest includes settings and exact coverage");expect(read(root/"bundle"/"channel-status.csv")=="n,value\n0,dataset-1\n1,dataset-1\n","Existing supplied CSV row/header unchanged");expect(!fs::exists(root/"bundle"/"INCOMPLETE"),"Complete bundle marker removed");
 unsigned polls=0;auto cancelled=write_bundle(root/"cancel",spec,sources,[&]{return ++polls>=4;});expect(!cancelled.complete&&fs::exists(root/"cancel"/"INCOMPLETE"),"Cancelled bundle explicitly incomplete");text=read(root/"cancel"/"manifest.json");expect(text.find("\"state\":\"cancelled\"")!=std::string::npos&&text.find("\"complete\":false")!=std::string::npos,"Cancelled manifest state");
 auto failure=sources;failure[0].write_row=[](uint64_t,std::ostream&){throw std::runtime_error("fixture cursor failure");};bool thrown=false;try{write_bundle(root/"failed",spec,failure);}catch(const std::runtime_error&){thrown=true;}expect(thrown&&fs::exists(root/"failed"/"INCOMPLETE"),"Source failure preserves incomplete marker");expect(read(root/"failed"/"manifest.json").find("\"state\":\"failed\"")!=std::string::npos,"Failure manifest explicit");
 thrown=false;try{write_bundle(root/"bundle",spec,sources);}catch(const std::runtime_error&){thrown=true;}expect(thrown,"Existing output never overwritten");
 auto empty=sources;for(auto& source:empty)source.row_count=0;auto e=write_bundle(root/"empty",spec,empty);expect(e.complete&&e.rows_written==std::array<uint64_t,4>{},"Empty snapshot exports all headers");
}
}
int main(){const auto root=fs::temp_directory_path()/("spdif-exports-"+std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));fs::create_directories(root);try{wav_tests(root);burst_tests(root);core_integration(root);bundle_tests(root);locale_tests(root);fs::remove_all(root);std::cout<<checks<<" export checks passed\n";return 0;}catch(const std::exception& e){std::cerr<<"Export failure: "<<e.what()<<" (fixtures retained at "<<root<<")\n";return 1;}}
