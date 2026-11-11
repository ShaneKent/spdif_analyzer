#include "SpdifAnalyzer.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
namespace {
constexpr U8 SampleType=0,StatusType=1,DiagnosticType=2;
std::string hex(uint64_t value,int width){ std::ostringstream s; s<<"0x"<<std::uppercase<<std::hex<<std::setw(width)<<std::setfill('0')<<value; return s.str(); }
std::string bytes(const std::array<uint8_t,24>& a){ std::ostringstream s; for(auto v:a)s<<std::hex<<std::uppercase<<std::setw(2)<<std::setfill('0')<<unsigned(v); return s.str(); }
std::string csv(const std::string& value){ std::string out="\""; for(char c:value){if(c=='"')out+='"';out+=c;}return out+'"'; }
int32_t signed_sample(uint32_t raw){ const auto p=raw&0xffffff;return (p&0x800000)?int32_t(p)-0x1000000:int32_t(p); }
char preamble(const Frame& f){return char(f.mData2&255);}
unsigned channel(const Frame& f){return preamble(f)=='W'?1:0;}
int position(const Frame& f){return int((f.mData2>>8)&511)-1;}
double measured(const Frame& f){return double(f.mData2>>17)/1000.0;}
bool parity_ok(uint32_t raw){unsigned n=0;for(unsigned i=0;i<28;++i)n+=(raw>>i)&1;return (n&1)==0;}
Frame frame(U8 type,uint64_t start,uint64_t end){Frame f;f.mType=type;f.mFlags=0;f.mData1=0;f.mData2=0;f.mStartingSampleInclusive=start;f.mEndingSampleInclusive=std::max(start,end);return f;}
}
SpdifResults::SpdifResults(SpdifAnalyzer* a,SpdifSettings* s):analyzer(a),settings(s){}
void SpdifResults::Add(const spdif::Subframe& s){
 auto f=frame(SampleType,s.start,s.end); f.mData1=s.raw;
 f.mData2=uint64_t(s.preamble)|(uint64_t(s.block_position+1)<<8)|(uint64_t(std::llround(s.sample_rate*1000))<<17);
 if(!s.parity_ok)f.mFlags|=DISPLAY_AS_ERROR_FLAG;
 else if(s.validity)f.mFlags|=DISPLAY_AS_WARNING_FLAG;
 AddFrame(f);
 const U64 v2_start=PublishPending(s);
 FrameV2 v;const char p[]={s.preamble,0};v.AddString("channel",s.channel?"B / right":"A / left");v.AddString("preamble",p);
 v.AddInteger("raw_slots_4_31",s.raw);v.AddString("payload_hex",hex(s.raw&0xffffff,6).c_str());
 v.AddInteger("signed_container_24",s.sample);v.AddByte("auxiliary",s.raw&15);
 v.AddInteger("main_field_raw",(s.raw>>4)&0xfffff);
 v.AddBoolean("V_invalid",s.validity);v.AddBoolean("U",s.user);v.AddBoolean("C",s.status);v.AddBoolean("P",s.parity);v.AddBoolean("parity_ok",s.parity_ok);
 if(s.block_position>=0)v.AddInteger("block_position",s.block_position);
 v.AddInteger("coverage_start_sample",s.start);v.AddInteger("coverage_end_sample",s.end);
 v.AddDouble("measured_sample_rate_hz",s.sample_rate);v.AddString("sample_meaning","Numeric container; consult channel status and integrity before interpreting as PCM");
 AddFrameV2(v,"subframe",v2_start,f.mEndingSampleInclusive);
 if(!s.parity_ok)AddMarker(v2_start,ErrorX,settings->channel);
}
void SpdifResults::Add(const spdif::StatusBlock& b){
 std::lock_guard<std::mutex> lock(mutex);
 status_blocks.push_back(b);
 if(pending.size()<7)pending.push_back({StatusType,status_blocks.size()-1});else ++suppressed_notifications;
}
void SpdifResults::Add(const spdif::Diagnostic& d){
 std::lock_guard<std::mutex> lock(mutex);
 diagnostics.push_back(d);
 if(pending.size()<7)pending.push_back({DiagnosticType,diagnostics.size()-1});else ++suppressed_notifications;
}
U64 SpdifResults::PublishPending(const spdif::Subframe& s){
 // Legacy bubbles contain only subframes. Structured notifications use a short
 // prefix of the next subframe, never overlapping another FrameV2. Their true
 // coverage remains in named fields and the metadata CSV exports.
 U64 cursor=s.start;
 std::lock_guard<std::mutex> lock(mutex);
 for(const auto& event:pending){
  FrameV2 v;
  if(event.type==StatusType){
   const auto& b=status_blocks.at(event.index);
   v.AddString("channel",b.channel?"B / right":"A / left");
   v.AddByteArray("C",b.c.data(),b.c.size());v.AddByteArray("U",b.u.data(),b.u.size());
   v.AddInteger("coverage_start_sample",b.start);v.AddInteger("coverage_end_sample",b.end);
   v.AddBoolean("complete",true);v.AddBoolean("integrity",b.integrity);v.AddString("interpretation",b.interpretation.c_str());
   AddFrameV2(v,"channel_status",cursor,cursor);
  }else{
   const auto& d=diagnostics.at(event.index);
   v.AddInteger("coverage_start_sample",d.start);v.AddInteger("coverage_end_sample",d.end);
   v.AddString("code",d.code.c_str());v.AddString("message",d.message.c_str());
   AddFrameV2(v,"diagnostic",cursor,cursor);AddMarker(cursor,ErrorX,settings->channel);
  }
  ++cursor;
 }
 if(suppressed_notifications){
  FrameV2 v;v.AddString("code","notification_summary");v.AddInteger("count",suppressed_notifications);
  v.AddString("message","Additional notifications omitted from waveform table; original events remain available in metadata CSV exports.");
  AddFrameV2(v,"diagnostic",cursor,cursor);++cursor;
 }
 pending.clear();suppressed_notifications=0;
 return cursor;
}
std::string SpdifResults::Text(const Frame& f){
 if(f.mType==SampleType){const auto raw=uint32_t(f.mData1);std::ostringstream s;s<<(channel(f)?"B/R ":"A/L ")<<preamble(f)<<" ";if(settings->display)s<<hex(raw&0xffffff,6);else s<<signed_sample(raw)<<" (container24)";if(!parity_ok(raw))s<<" PARITY ERROR";if((raw>>24)&1)s<<" V=1";return s.str();}
 return "";
}
void SpdifResults::GenerateBubbleText(U64 index,Channel&,DisplayBase){ClearResultStrings();auto f=GetFrame(index);auto t=Text(f);AddResultString(t.c_str());}
void SpdifResults::GenerateFrameTabularText(U64 index,DisplayBase){ClearTabularText();auto t=Text(GetFrame(index));AddTabularText(t.c_str());}
void SpdifResults::GeneratePacketTabularText(U64,DisplayBase){ClearTabularText();}
void SpdifResults::GenerateTransactionTabularText(U64,DisplayBase){ClearTabularText();}
void SpdifResults::GenerateExportFile(const char* name,DisplayBase,U32 type){
 std::ofstream out(name,std::ios::binary);if(!out)return;out.imbue(std::locale::classic());out<<std::setprecision(12);
 if(type==0)out<<"start_seconds,end_seconds,channel,preamble,block_position,raw_slots_4_31,payload_hex,signed_container_24,main_field_raw,auxiliary,V_invalid,U,C,P,parity_ok,measured_sample_rate_hz,sample_meaning\n";
 else if(type==1)out<<"start_seconds,end_seconds,channel,complete,integrity,C_bytes_earliest_first,U_bytes_earliest_first,interpretation\n";
 else out<<"start_seconds,end_seconds,code,message\n";
 const auto trigger=analyzer->GetTriggerSample();const auto rate=analyzer->GetSampleRate();
 const auto time=[=](S64 sample){return (double(sample)-double(trigger))/rate;};
 U64 total=0;
 {std::lock_guard<std::mutex> lock(mutex);total=type==0?GetNumFrames():type==1?status_blocks.size():diagnostics.size();}
 for(U64 i=0;i<total;++i){
  if(UpdateExportProgressAndCheckForCancel(i,total))break;
  if(type==0){
   const auto f=GetFrame(i);const uint32_t raw=uint32_t(f.mData1);
   out<<time(f.mStartingSampleInclusive)<<','<<time(f.mEndingSampleInclusive)<<','<<(channel(f)?'B':'A')<<','<<preamble(f)<<',';
   if(position(f)>=0)out<<position(f);
   out<<','<<hex(raw,7)<<','<<hex(raw&0xffffff,6)<<','<<signed_sample(raw)<<','<<hex((raw>>4)&0xfffff,5)<<','<<unsigned(raw&15);
   for(unsigned bit=24;bit<28;++bit)out<<','<<((raw>>bit)&1);
   out<<','<<parity_ok(raw)<<','<<measured(f)<<",numeric container only\n";
  }else if(type==1){
   spdif::StatusBlock b;{std::lock_guard<std::mutex> lock(mutex);b=status_blocks.at(size_t(i));}
   out<<time(b.start)<<','<<time(b.end)<<','<<(b.channel?'B':'A')<<",1,"<<b.integrity<<','<<bytes(b.c)<<','<<bytes(b.u)<<','<<csv(b.interpretation)<<'\n';
  }else{
   spdif::Diagnostic d;{std::lock_guard<std::mutex> lock(mutex);d=diagnostics.at(size_t(i));}
   out<<time(d.start)<<','<<time(d.end)<<','<<csv(d.code)<<','<<csv(d.message)<<'\n';
  }
 }
 UpdateExportProgressAndCheckForCancel(total,total);
}
