// Independent bitfield/interval vectors, not generated from status_info/period().
// References: https://raw.githubusercontent.com/torvalds/linux/master/include/sound/asoundef.h
// https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/libavformat/spdifdec.c
// https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/libavcodec/ac3defs.h
// https://d3uzseaevmutz1.cloudfront.net/pubs/appNote/an22.pdf (Tables2/3)
// Burst bytes remain original transport words; valid_bits counts HIGH significant
// bits of the final word. These fixtures do not validate codec CRC/decompression.
#include "Fixtures.h"
#include <iostream>
#include <map>
#include <stdexcept>
using namespace fixture;
static unsigned checks=0;
static void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
struct Capture {
 std::vector<spdif::Subframe> frames;std::vector<spdif::Diagnostic> diagnostics;std::vector<spdif::BurstEvent> bursts;
};
static Capture run(const Signal& signal){Capture c;spdif::Sink sink;
 sink.subframe=[&](const spdif::Subframe& f){c.frames.push_back(f);};
 sink.diagnostic=[&](const spdif::Diagnostic& d){c.diagnostics.push_back(d);};
 sink.burst=[&](const spdif::BurstEvent& b){c.bursts.push_back(b);};
 spdif::Decoder decoder({signal.capture,0},sink);for(auto e:signal.edges)decoder.edge(e);
 decoder.finish(signal.edges.empty()?0:signal.edges.back());return c;
}
static bool code(const Capture& c,const char* name){return std::any_of(c.diagnostics.begin(),c.diagnostics.end(),[&](const spdif::Diagnostic& d){return d.code==name;});}
static Signal transport(unsigned total,const std::map<unsigned,uint16_t>& words,
                        const std::array<uint8_t,24>& before,const std::array<uint8_t,24>& after={},unsigned change=~0u,
                        unsigned corrupt=~0u){
 Signal s(100000000,48000);
 for(unsigned frame=0;frame<total;++frame)for(unsigned ch=0;ch<2;++ch){unsigned n=frame*2+ch,pos=frame%192;
  const auto& status=frame>=change?after:before;bool c=(status[pos/8]>>(pos%8))&1;
  auto it=words.find(n);uint32_t data=raw(uint32_t(it==words.end()?0:it->second)<<8,false,false,c);
  if(n==corrupt)data^=1u<<27;
  s.subframe(ch?'W':pos?'M':'B',data);
 }
 return s;
}
static void put(std::map<unsigned,uint16_t>& words,unsigned frame,uint16_t pc,uint16_t pd,unsigned count){
 unsigned start=frame*2;words[start]=0xf872;words[start+1]=0x4e1f;words[start+2]=pc;words[start+3]=pd;
 for(unsigned i=0;i<count;++i)words[start+4+i]=uint16_t(0x1234+0x1111*i);
}
static void layouts(){
 // Hand-checked AES3 byte0: professional=1, emphasis none=(1<<2),
 // 48kHz=(2<<6). Byte1=2 stereo; byte2=4+(5<<3): 24-bit main audio.
 std::array<uint8_t,24> b{};b[0]=0x85;b[1]=2;b[2]=0x2c;
 auto x=spdif::status_info(b);check(x.professional&&x.supported&&!x.non_audio,"professional supported audio");
 check(x.sample_rate==48000&&x.emphasis_code==1&&x.word_length==24,"professional independent fields");
 check(x.layout.known&&x.layout.bits==24&&x.layout.stereo,"professional stereo layout");
 check(spdif::interpret_status(b).find("nominal_rate=48000")!=std::string::npos,"professional frequency text");
 check(spdif::interpret_status(b).find("copyright=")==std::string::npos,"no consumer field leakage");
 for(unsigned code=0;code<4;++code){b[0]=uint8_t(5|(code<<6));const unsigned expected[]={0,44100,48000,32000};check(spdif::status_info(b).sample_rate==expected[code],"professional frequency mapping");}
 b[0]=0x9d;check(spdif::interpret_status(b).find("CCITT J.17")!=std::string::npos,"professional emphasis is not consumer emphasis");b[0]=0x85;
 b[1]=8;x=spdif::status_info(b);check(x.supported&&!x.stereo&&x.layout.known,"professional independent A/B not universal stereo");
 b[1]=4;check(!spdif::status_info(b).supported&&!spdif::status_info(b).layout.known,"unsupported single channel conservative");
 b[1]=2;b[2]=0x32;x=spdif::status_info(b);check(x.word_length==16,"professional coord20 code6 means16 not17");
 b[2]=0x34;check(spdif::status_info(b).word_length==20,"professional coord24 code6 means20 not21");
 // Independent byte2 literal table from ALSA IEC958 professional definitions:
 // coordination2=20-bit main,4=24-bit main; source codes2/4/5/6.
 const unsigned professional_lengths[][2]={{0x12,18},{0x22,19},{0x2a,20},{0x32,16},
                                            {0x14,22},{0x24,23},{0x2c,24},{0x34,20}};
 for(const auto& vector:professional_lengths){
  b[2]=uint8_t(vector[0]);const auto info=spdif::status_info(b);
  check(info.word_length==vector[1]&&info.layout.known&&info.layout.bits==vector[1],"handchecked professional source-length table");
  check(spdif::pcm_value(0x800000,info.layout)==-int32_t(uint32_t(1)<<(vector[1]-1)),"professional word-length signed minimum");
  check(spdif::interpret_status(b).find("word_length="+std::to_string(vector[1]))!=std::string::npos,"professional length rendered without consumer mapping");
 }
 for(unsigned coordination:{2u,4u})for(unsigned source_code:{0u,1u,3u,7u}){
  b[2]=uint8_t(coordination|(source_code<<3));check(!spdif::status_info(b).layout.known,"unimplemented or not-indicated professional source-length codes remain unknown");
 }
 b[2]=0x0a;check(!spdif::status_info(b).layout.known,"professional source code1 not consumer word-length code1");
 b[2]=0x28;check(!spdif::status_info(b).layout.known,"missing professional coordination unknown");
 b[2]=0x2c;b[0]|=2;check(!spdif::status_info(b).layout.known,"non-audio cannot imply PCM layout");
 b={};b[4]=2;x=spdif::status_info(b);check(x.word_length==16&&x.layout.known,"consumer16 declared");
 check(spdif::pcm_value(0x8000ff,x.layout)==-32768,"16bit extraction excludes auxiliary and unused LSBs");
 check(spdif::pcm_value(0x7fffab,x.layout)==32767,"16bit maximum exact");
 check(spdif::pcm_value(0xffff00,x.layout)==-1,"16bit sign extension exact");
 b[4]=0x0b;check(spdif::pcm_value(0x800000,spdif::status_info(b).layout)==-8388608,"24bit minimum exact");
 bool threw=false;try{spdif::pcm_value(0,{});}catch(const std::invalid_argument&){threw=true;}check(threw,"unknown layout rejected");
}
static void trusted_layout(){
 std::array<uint8_t,24> a{},b{};a[4]=2;b[4]=0x0b;
 auto c=run(transport(600,{},a,b,384));
 auto at=[&](unsigned frame)->const spdif::Subframe&{return c.frames.at(frame*2);};
 check(!at(100).layout.known,"no layout before confirmed status");
 check(at(220).layout.known&&at(220).layout.bits==16,"trusted declared16 layout");
 check(!at(420).layout.known,"consumer byte4 change clears stale layout");
 check(at(580).layout.known&&at(580).layout.bits==24,"new complete24 layout restored");
 a={};a[0]=0x85;a[1]=2;a[2]=0x2c;b=a;b[2]=0x32;
 c=run(transport(600,{},a,b,384));
 check(c.frames.at(440).content==spdif::ContentFormat::Pcm&&c.frames.at(440).layout.professional,"professional PCM corroboration");
 check(!c.frames.at(820).layout.known,"professional byte2 change invalidates stale layout");
 check(c.frames.at(1160).layout.bits==16,"new professional word length applied");
 b=a;b[1]=4;c=run(transport(600,{},a,b,384));check(!c.frames.at(800).layout.known&&c.frames.at(1160).content==spdif::ContentFormat::Unknown,"professional mode change clears supported classification");
 a[1]=4;c=run(transport(230,{},a));check(c.frames.at(440).content==spdif::ContentFormat::Unknown,"unsupported professional mode not classified PCM");
}
static void streaming(){
 std::array<uint8_t,24> non{};non[0]=2;std::map<unsigned,uint16_t> w;put(w,220,1,27,2);w[444]=0x1234;w[445]=0xabcd;
 auto c=run(transport(230,w,non));check(c.bursts.size()==4,"one Begin two Word one End");
 check(c.bursts[0].phase==spdif::BurstPhase::Begin&&c.bursts[0].declared_bits==27,"Begin exact bit extent");
 check(c.bursts[1].word==0x1234&&c.bursts[1].valid_bits==16&&c.bursts[1].index==0,"first raw transport word");
 check(c.bursts[2].word==0xabcd&&c.bursts[2].valid_bits==11&&c.bursts[2].index==1,"last high11bits, low padding preserved");
 check(c.bursts[3].phase==spdif::BurstPhase::End&&c.bursts[3].info.complete&&c.bursts[3].info.integrity,"complete trusted payload End");
 check(c.bursts[3].info.format==spdif::ContentFormat::Ac3,"trusted AC3 payload transport");
 for(unsigned known=1;known<=3;++known){
  std::map<unsigned,uint16_t> header;put(header,220,1,128,8);
  auto truncated=transport(230,header,non);
  const auto boundary=truncated.expected[440+known].start;
  truncated.edges.erase(std::upper_bound(truncated.edges.begin(),truncated.edges.end(),boundary),truncated.edges.end());
  const auto partial=run(truncated);
  check(partial.bursts.size()==2&&partial.bursts[0].phase==spdif::BurstPhase::Begin&&partial.bursts[1].phase==spdif::BurstPhase::End,"partial header candidate receives forensic Begin/End");
  check(!partial.bursts[0].supported&&!partial.bursts[1].supported&&partial.bursts[0].declared_bits==0,"partial header never guesses payload units");
  check(!partial.bursts[1].info.complete&&!partial.bursts[1].info.integrity,"partial header never trusted");
  check(partial.bursts[1].info.pc==(known==3?1:0),"only decoded partial Pc retained");
  check(partial.bursts[1].info.evidence.find("known_header_words="+std::to_string(known))!=std::string::npos,"partial header evidence identifies known words");
 }
 w.clear();put(w,220,0x15,3,2);c=run(transport(230,w,non));check(c.bursts[0].declared_bits==24&&c.bursts[2].valid_bits==8,"EAC3 Pdbytes exact24bits");
 w.clear();put(w,220,1,128,8);c=run(transport(223,w,non));check(!c.bursts.back().info.complete&&!c.bursts.back().info.integrity,"truncated stream End rejects");
 c=run(transport(230,w,non,{},~0u,445));check(c.bursts.back().phase==spdif::BurstPhase::End&&!c.bursts.back().info.integrity,"parity damage terminates writer trust");
 w.clear();put(w,220,0x81,32,2);c=run(transport(230,w,non));check(c.bursts.size()==4&&!c.bursts.back().info.integrity,"Pcerror retains temporary words but rejects End");
 std::array<uint8_t,24> pro{};pro[0]=0x87;pro[1]=2;pro[2]=0x2c;
 w.clear();put(w,220,11,32,2);c=run(transport(230,w,pro));check(c.bursts.size()==2&&!c.bursts[0].supported&&c.bursts.back().info.format==spdif::ContentFormat::Unsupported,"professional SMPTE types never misread as IEC DTS");
 w.clear();put(w,220,0x33,128,8);c=run(transport(230,w,non));check(c.bursts.size()==2&&!c.bursts[0].supported&&!c.bursts[1].info.complete,"unsupported units no payload guessed");
 w.clear();put(w,220,11,65535,0);c=run(transport(230,w,non));check(c.bursts.size()==2&&!c.bursts.back().info.complete&&code(c,"burst_overlap"),"capacity overlap rejected without payload");
 w.clear();put(w,220,1,128,8);w[444]=0xf872;w[445]=0x4e1f;w[446]=1;w[447]=32;
 c=run(transport(230,w,non));check(code(c,"burst_overlap_candidate")&&!code(c,"burst_overlap"),"payload header shape only ambiguous candidate");
 check(c.bursts.size()==10&&c.bursts.back().info.complete,"payload sync pattern never splits burst");
}
static void cadence(){
 std::array<uint8_t,24> non{};non[0]=2;
 const unsigned types[]={1,11,12,13},periods[]={1536,512,1024,2048};
 for(unsigned n=0;n<4;++n){for(unsigned scale:{1u,2u}){std::map<unsigned,uint16_t>w;unsigned next=220+periods[n]*scale;put(w,220,uint16_t(types[n]),32,2);put(w,next,uint16_t(types[n]),32,2);
  auto c=run(transport(next+5,w,non));check(!code(c,"burst_early")&&!code(c,"burst_late"),"no early/late nominal cadence");check(code(c,"burst_missing")==bool(scale==2),"later header proves missing nominal period");}
  std::map<unsigned,uint16_t>w;unsigned next=220+periods[n]-1;put(w,220,uint16_t(types[n]),32,2);put(w,next,uint16_t(types[n]),32,2);
  check(code(run(transport(next+5,w,non)),"burst_early"),"early supported-type cadence");
 }
 std::map<unsigned,uint16_t>w;put(w,220,1,32,2);
 check(!code(run(transport(2000,w,non)),"burst_missing"),"capture boundary never proves missing next burst");
 put(w,230,11,32,2);auto c=run(transport(240,w,non));check(!code(c,"burst_early"),"format change resets cadence");
 w.clear();put(w,220,1,32,2);put(w,230,0x2001,32,2);c=run(transport(240,w,non));check(!code(c,"burst_early"),"stream change resets cadence");
 w.clear();put(w,220,1,32,2);put(w,700,1,32,2);auto signal=transport(710,w,non);
 const uint64_t gap_at=signal.expected[600].start;for(auto& edge:signal.edges)if(edge>=gap_at)edge+=1000000;
 c=run(signal);check(!code(c,"burst_early")&&!code(c,"burst_missing"),"gap resets cadence before corroborated reacquisition");
}

static void interleaved_cadence(){
 std::array<uint8_t,24> non{};non[0]=2;
 for(unsigned displacement:{1535u,1536u,3072u}){
  std::map<unsigned,uint16_t>w;put(w,220,1,32,2);put(w,220+displacement,1,32,2);
  put(w,300,0x2001,32,2);put(w,1836,0x2001,32,2);
  if(displacement==3072)put(w,3372,0x2001,32,2);
  auto c=run(transport(std::max(displacement==3072?3380u:1840u,224+displacement),w,non));
  check(code(c,"burst_early")==bool(displacement==1535),"interleaved stream0 early cadence retained");
  check(code(c,"burst_missing")==bool(displacement==3072),"interleaved stream0 missing cadence retained");
  for(const auto& d:c.diagnostics)if(d.code=="burst_early"||d.code=="burst_missing")
   check(d.message.find("stream=0")!=std::string::npos,"healthy interleaved stream1 has no fault");
 }
 // A format change resets only that stream: stream1 still has known AC3 cadence.
 std::map<unsigned,uint16_t>w;put(w,220,1,32,2);put(w,230,0x2001,32,2);
 put(w,240,11,32,2);put(w,230+1535,0x2001,32,2);
 auto c=run(transport(1800,w,non));
 check(code(c,"burst_early"),"stream0 format change preserves stream1 history");
 for(const auto& d:c.diagnostics)if(d.code=="burst_early")check(d.message.find("stream=1")!=std::string::npos,"format change does not fault stream0");
 w.clear();put(w,220,1,32,2);put(w,230,0x2001,32,2);put(w,700,1,32,2);put(w,710,0x2001,32,2);
 auto signal=transport(720,w,non);const uint64_t gap_at=signal.expected[600].start;
 for(auto& edge:signal.edges)if(edge>=gap_at)edge+=1000000;
 c=run(signal);check(!code(c,"burst_early")&&!code(c,"burst_missing"),"gap resets all interleaved stream histories");
}


static void pending_header_snapshot(){
 std::array<uint8_t,24> non{};non[0]=2;std::map<unsigned,uint16_t>w;put(w,220,1,128,8);
 auto signal=transport(230,w,non);std::vector<spdif::BurstEvent> events,snapshots;
 spdif::Sink sink;sink.burst=[&](const spdif::BurstEvent& event){events.push_back(event);};
 spdif::Decoder decoder({signal.capture,0},sink);
 check(!decoder.pending_header(),"idle decoder has no header snapshot");
 size_t edge=0;
 for(unsigned known=1;known<=3;++known){
  const auto boundary=signal.expected[440+known].start;
  while(edge<signal.edges.size()&&signal.edges[edge]<=boundary)decoder.edge(signal.edges[edge++]);
  const auto snapshot=decoder.pending_header();
  check(bool(snapshot),"live decoded partial header available without EOF");
  check(snapshot->phase==spdif::BurstPhase::Begin&&!snapshot->supported&&snapshot->declared_bits==0,"live pending header has no guessed payload");
  check(!snapshot->info.complete&&!snapshot->info.integrity&&snapshot->info.format==spdif::ContentFormat::Unknown,"pending header never claimed verified");
  check(snapshot->info.start==signal.expected[440].start&&snapshot->info.end==boundary-1,"pending header exact last decoded word coverage");
  check(snapshot->info.pc==(known==3?1:0)&&snapshot->info.pd==0,"pending header only decoded Pc known");
  check(snapshot->info.evidence.find("known_header_words="+std::to_string(known))!=std::string::npos,"pending header evidence identifies known prefix");
  snapshots.push_back(*snapshot);
  const auto again=decoder.pending_header();
  check(again&&again->info.end==snapshot->info.end&&events.empty(),"repeated getter neither mutates nor emits Begin");
 }
 for(unsigned i=0;i<snapshots.size();++i)
  check(snapshots[i].info.end==signal.expected[441+i].start-1&&snapshots[i].info.start<=snapshots[i].info.end,"captured snapshots remain immutable as decoder continues");
 const auto pd_end=signal.expected[444].start;
 while(edge<signal.edges.size()&&signal.edges[edge]<=pd_end)decoder.edge(signal.edges[edge++]);
 check(!decoder.pending_header()&&events.size()==1&&events[0].phase==spdif::BurstPhase::Begin,"normal Pd continuation emits exactly one real Begin");
 for(;edge<signal.edges.size();++edge)decoder.edge(signal.edges[edge]);
 check(!decoder.pending_header(),"completed payload has no partial header snapshot");
 unsigned begins=0,ends=0;for(const auto& event:events){begins+=event.phase==spdif::BurstPhase::Begin;ends+=event.phase==spdif::BurstPhase::End;}
 check(begins==1&&ends==1,"snapshot reading does not duplicate continuation lifecycle");
 decoder.finish(signal.edges.back());check(!decoder.pending_header(),"finished decoder has no partial header snapshot");
}


static void professional_reliability(){
 // Cirrus AN22 Table3: byte22 bit4 marks bytes0..5 unreliable; bits5..7
 // describe other groups and do not invalidate the audio-format fields.
 std::array<uint8_t,24> good{};good[0]=0x85;good[1]=2;good[2]=0x2c;
 auto bad=good;bad[22]=0x10;
 const auto unreliable=spdif::status_info(bad);
 check(!unreliable.audio_fields_reliable&&!unreliable.supported&&!unreliable.layout.known,"explicit unreliable professional audio fields are not trusted");
 check(unreliable.word_length==24&&unreliable.sample_rate==48000,"untrusted raw field interpretation preserved");
 check(spdif::interpret_status(bad).find("audio_fields_reliable=false")!=std::string::npos,"professional reliability is visible");
 for(unsigned mask:{1u,2u,4u,8u,32u,64u,128u,224u}){
  auto other=good;other[22]=uint8_t(mask);const auto info=spdif::status_info(other);
  check(info.audio_fields_reliable&&info.supported&&info.layout.known,"other reliability groups do not invalidate audio fields");
 }
 auto consumer=good;consumer[0]=4;consumer[4]=0x0b;consumer[22]=0x10;
 check(spdif::status_info(consumer).audio_fields_reliable&&spdif::status_info(consumer).layout.known,"professional reliability meaning never leaks to consumer mode");
 Signal signal(100000000,48000);
 for(unsigned frame=0;frame<800;++frame)for(unsigned ch=0;ch<2;++ch){
  const auto& bytes=frame>=192&&frame<384?bad:good;unsigned pos=frame%192;
  signal.subframe(ch?'W':pos?'M':'B',raw(0,false,false,(bytes[pos/8]>>(pos%8))&1));
 }
 auto result=run(signal);
 check(result.frames.at(220*2).content==spdif::ContentFormat::Pcm&&result.frames.at(220*2).layout.known,"reliable professional baseline classified");
 check(result.frames.at(372*2).content==spdif::ContentFormat::Unknown&&!result.frames.at(372*2).layout.known,"incoming unreliable bit immediately clears stale evidence");
 check(result.frames.at(400*2).content==spdif::ContentFormat::Unknown,"unreliable confirmed block stays unknown");
 check(result.frames.at(576*2).content==spdif::ContentFormat::Pcm&&result.frames.at(576*2).layout.known,"fresh reliable complete block restores PCM evidence");
 auto addresses=good;addresses[6]=0x41;addresses[14]=1;addresses[18]=2;addresses[22]=0xe0;addresses[23]=0x37;
 result=run(transport(600,{},good,addresses,192));
 check(std::all_of(result.frames.begin()+384,result.frames.end(),[](const spdif::Subframe& f){return f.content==spdif::ContentFormat::Pcm&&f.layout.known&&f.layout.bits==24;}),"professional addresses other reliability flags and unchecked CRC changes keep audio classification continuous");
}

int main(){try{layouts();trusted_layout();streaming();cadence();interleaved_cadence();pending_header_snapshot();professional_reliability();std::cout<<"PASS "<<checks<<" protocol feature checks\n";return 0;}catch(const std::exception&e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
