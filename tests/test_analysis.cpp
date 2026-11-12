#include "analysis/Analysis.h"
#include "Fixtures.h"
#include <algorithm>
#include <cmath>
#include <cfenv>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace spdif;
namespace {
unsigned checks=0;
void check(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
void near(double actual,double expected,double tolerance,const char* message){check(std::abs(actual-expected)<=tolerance,message);}
Subframe frame(unsigned index,int32_t value=0,unsigned bits=16){
 Subframe f;f.start=64*index;f.end=f.start+63;f.channel=index%2;f.preamble=index==0?'B':f.channel?'W':'M';
 f.raw=(uint32_t(value)&((uint32_t(1)<<bits)-1))<<(24-bits);f.sample=value;f.sample_rate=48000;
 f.parity_ok=true;f.content=ContentFormat::Pcm;f.layout={bits,true,true,false};return f;
}
struct Collected {std::vector<HealthWindow> health;std::vector<PcmWindow> pcm;std::vector<StatusChange> changes;AnalysisSink sink(){return {[&](const HealthWindow&w){health.push_back(w);},[&](const StatusChange&c){changes.push_back(c);},[&](const PcmWindow&w){pcm.push_back(w);}};}};
void health(){Collected out;Analysis a({6144000,256./6144000},out.sink());
 for(unsigned i=0;i<10;++i){if(i==4){StatusBlock b;b.start=0;b.end=255;b.integrity=true;a.add(b);b.channel=1;a.add(b);}auto f=frame(i);if(i==4)f.parity_ok=false;if(i==7)f.validity=true;a.add(f);}
 a.finish(640);auto s=a.summary();check(out.health.size()==3,"two full health windows and partial tail");
 check(s.start==0&&s.end==640,"health coverage denominator");check(s.decoded_stream_samples==640&&s.unknown_stream_samples==0,"global decoded interval union");
 for(auto& ch:s.channels){check(ch.decoded_subframes==5,"per-channel decoded counts");check(ch.decoded_coverage_samples==512,"conservative per-channel cycle coverage");check(ch.unknown_coverage_samples==128,"explicit head/tail unknown coverage");check(ch.status_good==1,"status confirmation counts");}
 check(s.channels[0].parity_failures==1&&s.channels[1].parity_failures==0,"parity channel counts");
 check(s.channels[1].v_invalid==1&&s.channels[0].v_invalid==0,"raw validity separate from parity");
 check(s.channels[0].has_fault&&s.channels[0].first_fault==256&&s.channels[0].last_fault==319,"fault source locations");
 check(!s.channels[1].has_fault,"raw V1 does not imply transport fault");
 uint64_t decoded=0,covered=0,good=0;for(const auto&w:out.health){decoded+=w.channels[0].decoded_subframes+w.channels[1].decoded_subframes;covered+=w.decoded_stream_samples;good+=w.channels[0].status_good+w.channels[1].status_good;check(w.end>w.start,"nonempty health windows");}
 check(decoded==10&&covered==640&&good==2,"window totals equal detailed records");
 check(out.health[0].channels[0].status_good==1&&out.health[1].channels[0].status_good==0,"status counts assigned inclusive source block end");
}
void holes_and_faults(){Collected out;Analysis a({6144000,256./6144000},out.sink());a.add(frame(0));a.add(frame(1));a.add(Diagnostic{128,255,"sync_loss","gap"});auto f=frame(8);a.add(f);a.add(frame(9));a.add(frame(10));a.finish(800);auto s=a.summary();
 check(s.decoded_stream_samples==320&&s.unknown_stream_samples==480,"missing stream time explicit");
 check(s.sync_losses==1&&s.channels[0].sync_losses==1&&s.channels[1].sync_losses==1,"global sync loss not double counted");
 check(s.channels[0].decoded_coverage_samples==128&&s.channels[1].decoded_coverage_samples==0,"no inferred coverage across gap");
 check(s.channels[0].first_fault==128&&s.channels[0].last_fault==255,"diagnostic source fault interval");
 Collected empty;Analysis idle({1000,1},empty.sink());idle.finish(2500);auto e=idle.summary();check(e.unknown_stream_samples==2500&&e.channels[0].unknown_coverage_samples==2500,"empty capture unknown coverage");check(empty.health.size()==3,"idle windows bounded incremental output");
}
StatusBlock block(uint64_t index,unsigned channel=0){StatusBlock b;b.start=index*384;b.end=b.start+383;b.channel=channel;b.c[3]=2;b.c[4]=0x0b;return b;}
void differences(){Collected out;Analysis a({1000,1},out.sink());auto first=block(0);a.add(first);a.add(block(0,1));a.add(block(1));check(out.changes.empty(),"initial acquisition and identical status suppressed");auto changed=block(2);changed.c[3]=15;changed.c[23]=128;a.add(changed);
 check(out.changes.size()==1&&out.changes[0].channel==0,"sparse per-channel status event");const auto& c=out.changes[0];check(c.changed_mask[3]==13&&c.changed_mask[23]==128,"exact XOR change mask");check(c.previous_start==384&&c.start==768&&c.end==1152,"status source coverage preserved");
 check(std::any_of(c.fields.begin(),c.fields.end(),[](const FieldChange&f){return f.name=="sample_rate_code"&&f.old_value=="2"&&f.new_value=="15";}),"unknown numeric field code preserved");
 check(std::any_of(c.fields.begin(),c.fields.end(),[](const FieldChange&f){return f.name=="C_byte_23"&&f.new_value=="128";}),"uninterpreted changed bytes remain numeric");
 auto semantic=block(1,1);semantic.c[3]=0;semantic.c[4]=2;semantic.c[0]=8;a.add(semantic);
 check(out.changes.size()==2&&out.changes.back().channel==1,"independent B-channel semantic change");
 check(std::any_of(out.changes.back().fields.begin(),out.changes.back().fields.end(),[](const FieldChange&f){return f.name=="sample_rate_hz"&&f.old_value=="48000"&&f.new_value=="44100";}),"supported old/new sample rate values");
 check(std::any_of(out.changes.back().fields.begin(),out.changes.back().fields.end(),[](const FieldChange&f){return f.name=="word_length_bits"&&f.old_value=="24"&&f.new_value=="16";}),"supported old/new word length values");
 check(std::any_of(out.changes.back().fields.begin(),out.changes.back().fields.end(),[](const FieldChange&f){return f.name=="emphasis"&&f.old_value=="none"&&f.new_value=="50/15 us";}),"supported emphasis transitions have semantic values");
 auto bad=block(3);bad.integrity=false;a.add(bad);a.add(block(4));check(out.changes.size()==2,"damaged status cannot bridge transition");a.add(Diagnostic{1900,2000,"incomplete_status","partial"});a.add(block(5));check(out.changes.size()==2,"incomplete evidence clears baselines");a.add(block(7));check(out.changes.size()==2,"nonconsecutive blocks do not bridge gap");a.finish(4000);
}
void unsupported_status_diff(){
 Collected out;Analysis a({1000,1},out.sink());auto old=block(0);old.c[0]=0x40;old.c[3]=15;a.add(old);
 auto next=block(1);next.c[0]=0x48;next.c[3]=14;a.add(next);a.finish(768);
 check(out.changes.size()==1&&out.changes[0].previous[3]==15&&out.changes[0].current[3]==14,"unsupported status differences preserve raw numeric codes");
 check(std::none_of(out.changes[0].fields.begin(),out.changes[0].fields.end(),[](const FieldChange& f){return f.name=="emphasis"||f.name=="sample_rate_hz";}),"unsupported consumer mode does not inherit mode-zero semantics");
}
void pcm_statistics(){Collected out;Analysis a({6144000,1},out.sink());const int32_t values[]={-32768,32767,0,0};for(unsigned i=0;i<8;++i)a.add(frame(i,i%2?100:values[i/2]));a.finish(512);
 const auto found=std::find_if(out.pcm.begin(),out.pcm.end(),[](const PcmWindow&p){return p.channel==0&&p.accepted;});check(found!=out.pcm.end(),"PCM per-channel statistics emitted");const auto& p=*found;
 check(p.accepted==4&&p.bits==16&&p.minimum==-32768&&p.maximum==32767&&p.peak==32768,"declared16-bit extrema and peak");near(p.mean,-.25,1e-12,"PCM mean/DC in integer units");near(p.rms,std::sqrt((32768.*32768+32767.*32767)/4),1e-9,"independent RMS");near(p.peak_normalized,1,1e-12,"normalized digital full scale");
 check(p.silence_samples==2&&p.full_scale_samples==2&&p.full_scale_runs==1&&p.max_full_scale_run==2,"silence and digital full-scale runs");near(p.silence_seconds,2./48000,1e-12,"silence duration counts observed samples only");
}
void exclusions_override_and_reset(){Collected out;Analysis a({6144000,1},out.sink());uint64_t accepted=0,parity=0,invalid=0,unknown=0,nonpcm=0,layout=0;
 for(unsigned i=0;i<12;++i){auto f=frame(i,1);if(i==2)f.parity_ok=false;if(i==3)f.validity=true;if(i==4)f.content=ContentFormat::Unknown;if(i==5)f.content=ContentFormat::Ac3;if(i==6)f.layout={};if(i==8)f.layout={20,true,true,false};if(i==10)f.sample_rate=96000;a.add(f);}a.finish(768);
 for(const auto&p:out.pcm){accepted+=p.accepted;parity+=p.excluded_parity;invalid+=p.excluded_invalid;unknown+=p.excluded_unknown;nonpcm+=p.excluded_non_pcm;layout+=p.excluded_layout;}
 check(accepted==7&&parity==1&&invalid==1&&unknown==1&&nonpcm==1&&layout==1,"exclusive exclusion counts reconcile to input");check(out.pcm.size()>=8,"content layout and rate transitions split segments");
 Collected forced;Analysis override({6144000,1,0,16},forced.sink());auto f=frame(0,-32768);f.layout={};override.add(f);auto invalidf=frame(1);invalidf.layout={};invalidf.validity=true;override.add(invalidf);auto compressed=frame(2);compressed.layout={};compressed.content=ContentFormat::Ac3;override.add(compressed);auto declared=frame(3,1,20);override.add(declared);override.finish(256);
 check(std::any_of(forced.pcm.begin(),forced.pcm.end(),[](const PcmWindow&p){return p.accepted==1&&p.layout_override&&p.bits==16&&p.minimum==-32768;}),"explicit override resolves ambiguous PCM only");
 check(std::any_of(forced.pcm.begin(),forced.pcm.end(),[](const PcmWindow&p){return p.accepted==1&&!p.layout_override&&p.bits==20;}),"declared layout takes precedence over override");
 uint64_t total=0;for(const auto&p:forced.pcm)total+=p.accepted;check(total==2,"override never bypasses V or compressed content gates");
 Collected windows;Analysis split({6144000,256./6144000},windows.sink());for(unsigned i=0;i<10;++i)split.add(frame(i,32767));split.finish(640);for(const auto&p:windows.pcm)check(p.accepted<=2,"PCM per-channel fixed windows bounded");
}
void excluded_samples_share_window(){
 Collected out;Analysis a({6144000,1},out.sink());
 // A has three accepted values with excluded samples between observations.
 // B remains valid, ensuring contiguous alternating transport throughout.
 const int32_t values[]={-32768,123,0,-456,32767};
 for(unsigned i=0;i<10;++i){auto f=frame(i,i%2?17:values[i/2]);if(i==2)f.validity=true;if(i==6)f.parity_ok=false;a.add(f);}a.finish(640);
 check(out.pcm.size()==2,"validity/parity toggles do not fragment per-channel PCM windows");
 const auto it=std::find_if(out.pcm.begin(),out.pcm.end(),[](const PcmWindow&p){return p.channel==0;});check(it!=out.pcm.end(),"A PCM aggregate present");const auto& p=*it;
 check(p.accepted==3&&p.excluded_invalid==1&&p.excluded_parity==1,"same window counts accepted and excluded observations");
 check(p.bits==16&&p.minimum==-32768&&p.maximum==32767&&p.peak==32768,"accepted extrema ignore excluded values");near(p.mean,-1./3,1e-12,"accepted-only mean across exclusions");near(p.rms,std::sqrt((32768.*32768+32767.*32767)/3),1e-9,"accepted-only RMS across exclusions");
 check(p.full_scale_samples==2&&p.full_scale_runs==2&&p.max_full_scale_run==1,"excluded observations never bridge digital full-scale runs");
 check(p.silence_samples==1,"only accepted zero sample is silence");near(p.silence_seconds,1./48000,1e-12,"excluded observations do not extend silence duration");near(p.peak_normalized,1,1e-12,"known layout normalization retained");
 Collected runs;Analysis interrupted({6144000,1},runs.sink());for(unsigned i=0;i<10;++i){auto f=frame(i,i%2?0:(i/2)%2?7:-32768);if(i==2)f.validity=true;if(i==6)f.parity_ok=false;interrupted.add(f);}interrupted.finish(640);
 const auto run=std::find_if(runs.pcm.begin(),runs.pcm.end(),[](const PcmWindow&p){return p.channel==0;});check(run!=runs.pcm.end()&&run->accepted==3&&run->full_scale_runs==3&&run->max_full_scale_run==1,"full-scale observations separated only by exclusions form independent runs");
 // Start invalid: precision/provenance must be initialized before acceptance.
 Collected initial;Analysis b({6144000,1,0,16},initial.sink());for(unsigned i=0;i<6;++i){auto f=frame(i,-32768);f.layout.known=false;f.layout.bits=0;if(i==0)f.validity=true;b.add(f);}b.finish(384);
 check(initial.pcm.size()==2,"invalid first sample does not create separate empty PCM segment");
 const auto first=std::find_if(initial.pcm.begin(),initial.pcm.end(),[](const PcmWindow&p){return p.channel==0;});check(first!=initial.pcm.end()&&first->accepted==2&&first->excluded_invalid==1,"invalid-first aggregate preserves exclusion and subsequent samples");
 check(first->bits==16&&first->layout_override,"invalid-first aggregate retains explicit layout provenance");near(first->rms_normalized,1,1e-12,"invalid-first aggregate has correct normalized RMS");
}
void rate_layout_matrix(){
 for(double rate:{32000.,44100.,48000.,96000.,192000.})for(unsigned bits:{16u,18u,20u,24u}){
  Collected out;const double capture=rate*200;Analysis a({capture,1000/capture},out.sink());
  const int32_t minimum=-(int32_t(1)<<(bits-1)),maximum=(int32_t(1)<<(bits-1))-1;
  const int32_t values[]={0,1,-1,minimum,maximum};
  for(unsigned i=0;i<30;++i){auto f=frame(i,i%2?-17:values[(i/2)%5],bits);f.start=i*100;f.end=f.start+99;f.sample_rate=rate;a.add(f);}a.finish(3000);
  check(out.health.size()==3,"rate matrix health window count");check(out.pcm.size()==6,"one PCM record per channel per fixed window");
  for(const auto&p:out.pcm){check(p.accepted==5&&p.bits==bits,"rate/layout matrix accepted samples");if(p.channel==0){near(p.mean,-.2,1e-12,"matrix known DC");check(p.peak==uint64_t(-int64_t(minimum)),"matrix signed full-scale peak");near(p.silence_seconds,1/rate,1e-12,"matrix sample duration");}else near(p.mean,-17,1e-12,"independent channel mean");}
 }
 Collected jitter;Analysis stable({6144000,1},jitter.sink());for(unsigned i=0;i<20;++i){auto f=frame(i,17);f.sample_rate=(i/2)%2?47984:48030;stable.add(f);}stable.finish(1280);
 check(jitter.pcm.size()==2,"ordinary sampled-rate quantization does not split PCM segments");for(const auto&p:jitter.pcm)check(p.accepted==10,"quantized rate counts retained");
}
void diagnostic_only_stream(){
 uint64_t windows=0;AnalysisSink sink;sink.health=[&](const HealthWindow&w){++windows;check(w.decoded_stream_samples==0,"noise does not invent decoded coverage");};
 Analysis a({1000000,1},sink);
 for(unsigned i=1;i<=1000;++i){a.add(Diagnostic{uint64_t(i)*1000000,uint64_t(i)*1000000,"searching","noise"});if(i>1)check(windows>=i-1,"diagnostic-only windows flush without unbounded retention");}
 auto s=a.summary();check(s.end==1000000001&&s.unknown_stream_samples==1000000001,"metadata time is explicit unknown coverage");a.finish(1000000001);check(windows==1001,"noise final partial window emitted");
}
void pending_candidate_watermark(){
 Collected out;Analysis a({1000000,.0001},out.sink());
 a.add(Diagnostic{500,500,"searching","candidate not yet complete"});
 check(out.health.size()==3,"metadata watermark keeps bounded candidate lookback");
 auto f=frame(0,1);f.start=450;f.end=514;f.sample_rate=7500;a.add(f);
 f=frame(1,1);f.start=515;f.end=579;f.sample_rate=7500;a.add(f);
 f=frame(2,1);f.start=580;f.end=644;f.sample_rate=7500;a.add(f);a.finish(645);
 auto s=a.summary();check(s.late_events==0&&s.decoded_stream_samples==195,"pending candidate never backdates published health windows");
 uint64_t frames=0,coverage=0;for(const auto&w:out.health){frames+=w.channels[0].decoded_subframes+w.channels[1].decoded_subframes;coverage+=w.decoded_stream_samples;}
 check(frames==3&&coverage==195,"window output retains late-decoded candidate exactly once");
}
void decoder_integration(){
 std::array<uint8_t,24> status{};status[0]=4;status[3]=2;status[4]=0x0b;
 fixture::Signal clean(100000000,48000);clean.frames(210,0,&status,&status);
 Collected out;Analysis analysis({clean.capture,1},out.sink());
 Sink sink;sink.subframe=[&](const Subframe&f){analysis.add(f);};sink.status=[&](const StatusBlock&b){analysis.add(b);};sink.diagnostic=[&](const Diagnostic&d){analysis.add(d);};sink.content=[&](const ContentInfo&c){analysis.add(c);};
 Decoder decoder({clean.capture,0},sink);for(auto edge:clean.edges)decoder.edge(edge);decoder.finish(clean.edges.back());analysis.finish(clean.edges.back()+1);
 auto summary=analysis.summary();check(summary.channels[0].decoded_subframes==210&&summary.channels[1].decoded_subframes==209,"real decoder health agrees with detailed decoded rows");
 check(!summary.channels[0].has_fault&&!summary.channels[1].has_fault,"ordinary unconfirmed EOF tail is not channel corruption");
 uint64_t accepted=0,unknown=0;for(const auto&p:out.pcm){accepted+=p.accepted;unknown+=p.excluded_unknown;}
 check(accepted==35&&unknown==384,"PCM stats wait for actual confirmed status/layout callbacks");
 fixture::Signal damaged(100000000,48000);
 for(unsigned i=0;i<400;++i)for(unsigned ch=0;ch<2;++ch){unsigned pos=i%192;auto bytes=status;if(ch==1&&i>=192)bytes[3]=0;bool c=(bytes[pos/8]>>(pos%8))&1;auto raw=fixture::raw(i,false,false,c);if(i==30&&ch==0)raw^=1u<<27;damaged.subframe(ch?'W':pos?'M':'B',raw);}
 Collected badout;Analysis bad({damaged.capture,1},badout.sink());Sink badsink;badsink.subframe=[&](const Subframe&f){bad.add(f);};badsink.status=[&](const StatusBlock&b){bad.add(b);};badsink.diagnostic=[&](const Diagnostic&d){bad.add(d);};badsink.content=[&](const ContentInfo&c){bad.add(c);};
 Decoder baddecoder({damaged.capture,0},badsink);for(auto edge:damaged.edges)baddecoder.edge(edge);baddecoder.finish(damaged.edges.back());bad.finish(damaged.edges.back()+1);auto b=bad.summary();
 check(b.channels[0].parity_failures==1&&b.channels[1].parity_failures==0,"actual damaged parity attributed only to source channel");
 check(b.channels[0].has_fault&&!b.channels[1].has_fault,"parity diagnostic and partial tail never contaminate other channel faults");
 check(b.channels[0].first_fault==damaged.expected[60].start&&b.channels[0].last_fault==damaged.expected[61].start-1,"damaged block metadata does not widen precise parity fault locations");
 check(b.channels[0].status_damaged==1&&b.channels[1].status_damaged==0,"damaged status integrity attributed by channel");
 check(badout.changes.size()==1&&badout.changes[0].channel==1,"healthy channel keeps independent trusted status baseline across other-channel parity");
}
void status_source_end(){
 Collected out;Analysis a({1000,.256},out.sink());StatusBlock b;b.start=0;b.end=255;a.add(b);a.finish(256);
 check(a.summary().end==256&&out.health.size()==1&&out.health[0].channels[0].status_good==1,"status-only replay uses source coverage without extra invented sample");
 std::array<uint8_t,24> c{};c[0]=4;c[3]=2;c[4]=0x0b;fixture::Signal signal(49152000,48000);signal.frames(193,0,&c,&c);
 Collected real;const uint64_t exact_boundary=signal.expected[384].start;Analysis actual({signal.capture,double(exact_boundary)/signal.capture},real.sink());Sink sink;sink.subframe=[&](const Subframe&f){actual.add(f);};sink.status=[&](const StatusBlock&b){actual.add(b);};sink.diagnostic=[&](const Diagnostic&d){actual.add(d);};sink.content=[&](const ContentInfo&i){actual.add(i);};Decoder decoder({signal.capture,0},sink);for(auto edge:signal.edges)decoder.edge(edge);decoder.finish(signal.edges.back());actual.finish(signal.edges.back()+1);
 check(!real.health.empty()&&real.health[0].end==exact_boundary&&real.health[0].channels[0].status_good==1&&real.health[0].channels[1].status_good==1,"delayed decoder status callback is counted in source-ending window");check(actual.summary().late_events==0,"actual callback ordering never backdates closed window");
}
void invalid_rate_measurements(){
 Collected out;Analysis a({6144000,1},out.sink());for(unsigned i=0;i<4;++i){auto f=frame(i);f.sample_rate=i==0?0:i==1?std::numeric_limits<double>::quiet_NaN():i==2?48000:44100;a.add(f);}a.finish(256);
 auto summary=a.summary();for(unsigned ch=0;ch<2;++ch){const auto& h=summary.channels[ch];check(h.decoded_subframes==2&&h.rate_measurements==1,"invalid measured rates excluded from mean denominator only");near(h.mean_sample_rate,ch?44100:48000,1e-12,"single valid rate retains correct mean");check(h.min_sample_rate<=h.mean_sample_rate&&h.mean_sample_rate<=h.max_sample_rate,"health measured mean lies within measured extrema");}
 bool bad=false;try{Analysis overflow({1,std::ldexp(1.0,64)},{});}catch(const std::invalid_argument&){bad=true;}check(bad,"excessive finite window rejected before integral conversion");
 // Exercise a large accepted window near the old signed llround boundary.
 std::feclearexcept(FE_ALL_EXCEPT);Analysis near_limit({4294967295.,2147483648.5},{});near_limit.finish(0);check(near_limit.summary().end==0&&std::fetestexcept(FE_INVALID)==0,"near-signed-limit window rounding remains defined without signed llround domain error");
}
void snapshots(){
 Collected out;Analysis a({6144000,1},out.sink());for(unsigned i=0;i<6;++i)a.add(frame(i,i%2?100:1));
 auto first=a.current_pcm(),second=a.current_pcm();check(first.size()==2&&second.size()==2,"read-only live PCM snapshot bounded to channels");
 check(out.pcm.empty(),"snapshot does not emit or finish a PCM window");
 for(unsigned ch=0;ch<2;++ch){check(first[ch].accepted==3&&second[ch].accepted==3,"repeated snapshots never duplicate samples");near(first[ch].mean,ch?100:1,1e-12,"snapshot calculates current means");check(first[ch].segment==second[ch].segment,"snapshot does not reset segments");}
 for(unsigned i=6;i<8;++i)a.add(frame(i,i%2?100:1));
 a.finish(512);
 check(out.pcm.size()==2&&out.pcm[0].accepted==4&&out.pcm[1].accepted==4,"final emission counts unaffected by snapshots");check(a.current_pcm().empty(),"finished PCM has no duplicate current snapshots");
}
void boundaries(){Collected out;uint64_t origin=uint64_t(1)<<54;Analysis a({1000,.2,origin},out.sink());for(unsigned i=0;i<4;++i){auto f=frame(i);f.start+=origin;f.end+=origin;a.add(f);}a.finish(origin+256);auto s=a.summary();check(s.start==origin&&s.end==origin+256&&s.decoded_stream_samples==256,"large integer timestamps lossless");bool rejected=false;try{Analysis bad({0,1},{});}catch(const std::invalid_argument&){rejected=true;}check(rejected,"bad analysis config rejected");
 for(double rate:{1e30,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){bool bad=false;try{Analysis extreme({rate,1},{});}catch(const std::invalid_argument&){bad=true;}check(bad,"out-of-range capture rates rejected before timestamp conversion");}
}
}
int main(){try{health();holes_and_faults();differences();unsupported_status_diff();pcm_statistics();exclusions_override_and_reset();excluded_samples_share_window();rate_layout_matrix();diagnostic_only_stream();pending_candidate_watermark();decoder_integration();status_source_end();invalid_rate_measurements();snapshots();boundaries();std::cout<<"PASS analysis "<<checks<<" checks\n";return 0;}catch(const std::exception&e){std::cerr<<"FAIL analysis after "<<checks<<": "<<e.what()<<'\n';return 1;}}
