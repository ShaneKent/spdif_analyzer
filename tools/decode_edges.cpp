#include "core/Decoder.h"
#include "input/EdgeInput.h"
#include "analysis/Analysis.h"
#include "report/Events.h"
#include "report/CodecEvents.h"
#include "export/Exports.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
uint64_t integer(const std::string& s){return spdif::input::unsigned_integer(s);}
double real(const std::string& s){size_t used=0;double n=std::stod(s,&used);if(used!=s.size()||!std::isfinite(n))throw std::invalid_argument("Invalid finite number: "+s);return n;}
std::string lower(std::string s){for(auto& c:s)c=char(std::tolower(static_cast<unsigned char>(c)));return s;}
bool matches(const std::string& s,spdif::ContentFormat f){using F=spdif::ContentFormat;return (s=="pcm"&&f==F::Pcm)||(s=="nonpcm"&&f==F::NonPcm)||(s=="ac3"&&f==F::Ac3)||(s=="eac3"&&f==F::Eac3)||(s=="dts1"&&f==F::Dts1)||(s=="dts2"&&f==F::Dts2)||(s=="dts3"&&f==F::Dts3)||(s=="dts"&&(f==F::Dts1||f==F::Dts2||f==F::Dts3));}
struct Options {
 double capture=0,nominal=0,window=1;uint64_t origin=0,end=0;bool json=false,explicit_end=false;
 unsigned bits=0,rate=0;bool stereo=false;std::string expected,wav,bursts;std::set<std::string> failures;
};
void help(){std::cout<<"Usage: decode_edges <capture-rate-Hz> [options] < edge-indices.txt\n"
 "  --jsonl                     Versioned machine events; sample indices are strings\n"
 "  --nominal-rate Hz            Manual transport-rate acquisition constraint\n"
 "  --window-seconds seconds     Analysis window (default 1)\n"
 "  --origin-sample index        Capture/window origin (default 0)\n"
 "  --end-exclusive index        Known capture end; otherwise last edge + 1\n"
 "  --pcm-bits 16..24            Resolve unknown PCM layout explicitly\n"
 "  --wav-output new-directory   Segmented PCM WAV and timestamp index\n"
 "  --wav-rate Hz               Explicit WAV-rate override\n"
 "  --assume-stereo              Explicit A/B stereo interpretation for WAV\n"
 "  --burst-output new-directory Complete trusted IEC 61937 payload files\n"
 "  --fail-on list               Comma-separated parity,sync_loss,content_conflict,no_frames\n"
 "  --expect-content format      pcm,nonpcm,ac3,eac3,dts,dts1,dts2,dts3\n"
 "Exit: 0=success, 1=no decoded frames or failed policy, 2=invalid input/I/O.\n";}
Options parse(int argc,char** argv){Options o;if(argc<2)throw std::invalid_argument("Capture sample rate is required");o.capture=real(argv[1]);if(o.capture<=0||o.capture>std::numeric_limits<uint32_t>::max())throw std::invalid_argument("Capture sample rate must be positive and within the SDK uint32 range");for(int i=2;i<argc;++i){std::string key=argv[i];auto value=[&](){if(++i>=argc)throw std::invalid_argument("Missing value for "+key);return std::string(argv[i]);};
 if(key=="--jsonl")o.json=true;else if(key=="--nominal-rate")o.nominal=real(value());else if(key=="--window-seconds")o.window=real(value());else if(key=="--origin-sample")o.origin=integer(value());else if(key=="--end-exclusive"){o.end=integer(value());o.explicit_end=true;}
 else if(key=="--pcm-bits"){auto n=integer(value());if(n<16||n>24)throw std::invalid_argument("PCM override must be 16..24");o.bits=unsigned(n);}
 else if(key=="--wav-rate"){auto n=integer(value());if(n<8000||n>192000)throw std::invalid_argument("WAV rate must be 8000..192000");o.rate=unsigned(n);}
 else if(key=="--assume-stereo")o.stereo=true;else if(key=="--wav-output")o.wav=value();else if(key=="--burst-output")o.bursts=value();
 else if(key=="--expect-content"){o.expected=lower(value());const std::set<std::string> allowed={"pcm","nonpcm","ac3","eac3","dts","dts1","dts2","dts3"};if(!allowed.count(o.expected))throw std::invalid_argument("Unsupported expected content: "+o.expected);}
 else if(key=="--fail-on"){std::string v=value();size_t at=0;for(;;){auto end=v.find(',',at);auto f=v.substr(at,end-at);if(f!="parity"&&f!="sync_loss"&&f!="content_conflict"&&f!="no_frames")throw std::invalid_argument("Unknown failure policy: "+f);o.failures.insert(f);if(end==std::string::npos)break;at=end+1;}}
 else throw std::invalid_argument("Unknown option: "+key);
 }
 if(o.nominal!=0&&(o.nominal<8000||o.nominal>192000))throw std::invalid_argument("Nominal rate must be 0 or 8000..192000 Hz");
 if(o.window<=0)throw std::invalid_argument("Analysis window must be positive");
 if(o.explicit_end&&o.end<o.origin)throw std::invalid_argument("Capture end precedes origin");
 return o;
}
}
int main(int argc,char** argv){
 if(argc==2&&(std::string(argv[1])=="--help"||std::string(argv[1])=="-h")){help();return 0;}
 bool json_mode=false;
 try {
 const auto opt=parse(argc,argv);json_mode=opt.json;
 std::unique_ptr<spdif::exports::WavWriter> wav;std::unique_ptr<spdif::exports::BurstWriter> bursts;
 if(!opt.wav.empty())wav=std::make_unique<spdif::exports::WavWriter>(opt.wav,spdif::exports::WavConfig{opt.capture,opt.bits,opt.rate,opt.stereo});
 if(!opt.bursts.empty())bursts=std::make_unique<spdif::exports::BurstWriter>(opt.bursts);
 auto emit=[&](const auto& value){if(opt.json)std::cout<<spdif::json::record(value)<<'\n';};
 if(opt.json)std::cout<<spdif::json::event("metadata").text("analyzer_version",spdif::analyzer_version).number("capture_rate_hz",opt.capture).number("nominal_rate_hz",opt.nominal).number("window_seconds",opt.window).sample("origin_sample",opt.origin).integer("pcm_bits_override",opt.bits).text("sample_index_encoding","decimal-string").str()<<'\n';
 spdif::AnalysisSink analysis_sink;analysis_sink.health=[&](const auto& h){emit(h);};analysis_sink.pcm=[&](const auto& p){emit(p);};analysis_sink.status_change=[&](const auto& c){emit(c);};
 spdif::Analysis analysis({opt.capture,opt.window,opt.origin,opt.bits},analysis_sink);
 std::map<std::string,uint64_t> errors;uint64_t count=0,blocks=0,last=opt.origin,parity=0,conflicts=0;double sum=0;bool seen=false,expected_seen=false;
 spdif::codec::Inspector codec([&](const spdif::codec::CodecInfo& c){emit(c);if(bursts)bursts->codec(c);if(!opt.json)std::cout<<"codec format="<<spdif::codec::codec_name(c.frame.codec)<<" header="<<spdif::codec::header_state_name(c.frame.header)<<" sample_rate="<<c.frame.sample_rate<<" channels="<<c.frame.channels<<" frame_crc="<<spdif::integrity_name(c.frame.frame_crc)<<" "<<c.frame.evidence<<'\n';});
 spdif::Sink sink;
 sink.subframe=[&](const spdif::Subframe& f){++count;sum+=f.sample_rate;if(!f.parity_ok)++parity;if(f.content==spdif::ContentFormat::Conflict)++conflicts;if(matches(opt.expected,f.content))expected_seen=true;emit(f);analysis.add(f);if(wav)wav->subframe(f);};
 sink.content=[&](const spdif::ContentInfo& c){emit(c);analysis.add(c);if(c.format==spdif::ContentFormat::Conflict)++conflicts;if(!opt.json)std::cout<<"content format="<<spdif::content_name(c.format)<<" complete="<<c.complete<<" integrity="<<c.integrity<<" "<<c.evidence<<'\n';};
 sink.status=[&](const spdif::StatusBlock& b){++blocks;emit(b);analysis.add(b);if(wav)wav->status(b);if(!opt.json)std::cout<<"status channel="<<b.channel<<" integrity="<<b.integrity<<" "<<b.interpretation<<'\n';};
 sink.diagnostic=[&](const spdif::Diagnostic& d){++errors[d.code];emit(d);analysis.add(d);if(wav)wav->diagnostic(d);if(errors[d.code]<=3)std::cerr<<d.code<<" at "<<d.start<<": "<<d.message<<'\n';};
 sink.burst=[&](const spdif::BurstEvent& b){codec.burst(b);if(bursts)bursts->burst(b);};
 spdif::Decoder decoder({opt.capture,opt.nominal},sink);
 std::string token;while(std::cin>>token){last=integer(token);if(last<opt.origin)throw std::invalid_argument("Input edge precedes capture origin");decoder.edge(last);seen=true;}
 if(!std::cin.eof())throw std::runtime_error("Cannot read edge input");
 uint64_t end=opt.end;
 if(!opt.explicit_end){if(seen&&last==std::numeric_limits<uint64_t>::max())throw std::invalid_argument("Exclusive capture end cannot be represented");end=seen?last+1:opt.origin;}
 if(seen&&end<=last)throw std::invalid_argument("Exclusive capture end must follow the final edge");
 decoder.finish(end?end-1:0);codec.finish();analysis.finish(end);if(wav)wav->finish(end);if(bursts)bursts->finish();
 std::vector<std::string> failed;
 if(!count)failed.push_back("no_frames");
 if(parity&&opt.failures.count("parity"))failed.push_back("parity");
 if(errors["sync_loss"]&&opt.failures.count("sync_loss"))failed.push_back("sync_loss");
 if(conflicts&&opt.failures.count("content_conflict"))failed.push_back("content_conflict");
 if(!opt.expected.empty()&&!expected_seen)failed.push_back("expected_content_absent");
 if(opt.json){spdif::json::Object totals,diagnostics;totals.integer("subframes",count).integer("status_records",blocks).integer("parity_failures",parity).integer("content_conflict_records",conflicts).number("mean_transport_rate_hz",count?sum/count:0);for(const auto& e:errors)if(e.second)diagnostics.integer(e.first,e.second);std::vector<std::string> failures;for(const auto& f:failed)failures.push_back(spdif::json::quote(f));auto summary=spdif::json::event("summary");summary.boolean("passed",failed.empty()).raw("failed_policies",spdif::json::array(failures)).raw("totals",totals.str()).raw("diagnostics",diagnostics.str());if(!opt.expected.empty())summary.text("expected_content",opt.expected).boolean("expected_content_observed",expected_seen);std::cout<<spdif::json::health_fields(summary,analysis.summary())<<'\n';}
 else{std::cout<<"subframes="<<count<<" status_records="<<blocks<<" mean_transport_rate="<<std::setprecision(12)<<(count?sum/count:0)<<'\n';for(const auto& e:errors)if(e.second)std::cout<<"diagnostics "<<e.first<<'='<<e.second<<'\n';const auto health=analysis.summary();for(unsigned ch=0;ch<2;++ch){const auto& c=health.channels[ch];std::cout<<"health channel="<<ch<<" decoded="<<c.decoded_subframes<<" V_invalid="<<c.v_invalid<<" parity_failures="<<c.parity_failures<<" unknown_coverage_samples="<<c.unknown_coverage_samples<<'\n';}for(const auto& f:failed)std::cerr<<"Failed policy: "<<f<<'\n';}
 if(!std::cout)throw std::runtime_error("Cannot write report output");
 return failed.empty()?0:1;
 }catch(const std::exception& e){std::cerr<<"decode_edges: "<<e.what()<<'\n';if(json_mode)std::cout<<spdif::json::event("fatal_error").text("message",e.what()).str()<<'\n';return 2;}
}
