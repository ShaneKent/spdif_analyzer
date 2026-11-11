#include "SpdifAnalyzer.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
namespace {
constexpr U8 PreambleType=0,AuxType=1,MainType=2,VType=3,UType=4,CType=5,PType=6,SampleType=7;
constexpr U8 StatusType=1,DiagnosticType=2,ContentType=3;
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
 auto f=frame(SampleType,s.start,s.end); f.mData1=uint64_t(s.raw)|(uint64_t(s.content)<<32);
 f.mData2=uint64_t(s.preamble)|(uint64_t(s.block_position+1)<<8)|(uint64_t(std::llround(s.sample_rate*1000))<<17);
 if(!s.parity_ok)f.mFlags|=DISPLAY_AS_ERROR_FLAG;
 else if(s.validity)f.mFlags|=DISPLAY_AS_WARNING_FLAG;
 if(settings->detail==0){
  for(U8 field=0;field<7;++field){
   auto part=f; part.mType=field;
   part.mStartingSampleInclusive=s.field_boundaries[field];
   part.mEndingSampleInclusive=s.field_boundaries[field+1]-1;
   part.mFlags=0;
   if(field==PType && !s.parity_ok)part.mFlags=DISPLAY_AS_ERROR_FLAG;
   if(field==VType && s.validity)part.mFlags=DISPLAY_AS_WARNING_FLAG;
   AddFrame(part);
  }
 }else AddFrame(f);
 const U64 v2_start=PublishPending(s);
 FrameV2 v;const char p[]={s.preamble,0};v.AddString("channel",s.channel?"B / right":"A / left");v.AddString("preamble",p);
 v.AddInteger("raw_slots_4_31",s.raw);v.AddString("payload_hex",hex(s.raw&0xffffff,6).c_str());
 v.AddString("content",spdif::content_name(s.content));
 v.AddInteger("signed_container_24",s.sample);v.AddByte("auxiliary",s.raw&15);
 v.AddInteger("main_field_raw",(s.raw>>4)&0xfffff);
 v.AddBoolean("V_invalid",s.validity);v.AddBoolean("U",s.user);v.AddBoolean("C",s.status);v.AddBoolean("P",s.parity);v.AddBoolean("parity_ok",s.parity_ok);
 if(s.block_position>=0)v.AddInteger("block_position",s.block_position);
 const char* boundaries[]={"preamble_start_sample","aux_start_sample","main_start_sample","V_start_sample","U_start_sample","C_start_sample","P_start_sample","subframe_end_exclusive_sample"};
 for(size_t i=0;i<s.field_boundaries.size();++i)v.AddInteger(boundaries[i],s.field_boundaries[i]);
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
void SpdifResults::Add(const spdif::ContentInfo& c){
 std::lock_guard<std::mutex> lock(mutex);
 content_events.push_back(c);
 if(pending.size()<7)pending.push_back({ContentType,content_events.size()-1});else ++suppressed_notifications;
}
U64 SpdifResults::PublishPending(const spdif::Subframe& s){
 // Legacy bubbles contain only sample fields (or compact subframes). Structured notifications use a short
 // prefix of the next subframe, never overlapping another FrameV2. Their true
 // coverage remains in named fields and the metadata CSV exports.
 U64 cursor=s.start;
 struct Copy { U8 type; spdif::StatusBlock status; spdif::Diagnostic diagnostic; spdif::ContentInfo content; };
 std::vector<Copy> notifications;
 U64 suppressed=0;
 {
  std::lock_guard<std::mutex> lock(mutex);
  notifications.reserve(pending.size());
  for(const auto& event:pending){
   Copy copy;copy.type=event.type;
   if(event.type==StatusType)copy.status=status_blocks.at(event.index);
   else if(event.type==ContentType)copy.content=content_events.at(event.index);
   else copy.diagnostic=diagnostics.at(event.index);
   notifications.push_back(std::move(copy));
  }
  pending.clear();suppressed=suppressed_notifications;suppressed_notifications=0;
 }
 // No calls into Saleae while holding the metadata mutex: export runs on a
 // separate SDK thread and can otherwise invert metadata/SDK lock ordering.
 for(const auto& event:notifications){
  FrameV2 v;
  if(event.type==StatusType){
   const auto& b=event.status;
   v.AddString("channel",b.channel?"B / right":"A / left");
   v.AddByteArray("C",b.c.data(),b.c.size());v.AddByteArray("U",b.u.data(),b.u.size());
   v.AddInteger("coverage_start_sample",b.start);v.AddInteger("coverage_end_sample",b.end);
   v.AddBoolean("complete",true);v.AddBoolean("integrity",b.integrity);v.AddString("interpretation",b.interpretation.c_str());
   AddFrameV2(v,"channel_status",cursor,cursor);
  }else if(event.type==ContentType){
   const auto& c=event.content;
   v.AddInteger("coverage_start_sample",c.start);v.AddInteger("coverage_end_sample",c.end);
   v.AddString("format",spdif::content_name(c.format));v.AddString("evidence",c.evidence.c_str());
   v.AddInteger("Pc",c.pc);v.AddInteger("Pd",c.pd);v.AddBoolean("burst",c.burst);
   v.AddBoolean("complete",c.complete);v.AddBoolean("integrity",c.integrity);
   AddFrameV2(v,"content",cursor,cursor);
  }else{
   const auto& d=event.diagnostic;
   v.AddInteger("coverage_start_sample",d.start);v.AddInteger("coverage_end_sample",d.end);
   v.AddString("code",d.code.c_str());v.AddString("message",d.message.c_str());
   AddFrameV2(v,"diagnostic",cursor,cursor);AddMarker(cursor,ErrorX,settings->channel);
  }
  ++cursor;
 }
 if(suppressed){
  FrameV2 v;v.AddString("code","notification_summary");v.AddInteger("count",suppressed);
  v.AddString("message","Additional notifications omitted from waveform table; original events remain available in metadata CSV exports.");
  AddFrameV2(v,"diagnostic",cursor,cursor);++cursor;
 }
 return cursor;
}
std::string SpdifResults::Text(const Frame& f){
 const auto raw=uint32_t(f.mData1);
 const auto payload=raw&0xffffff;
 const auto main=(raw>>4)&0xfffff;
 const int32_t main_signed=(main&0x80000)?int32_t(main)-0x100000:int32_t(main);
 std::ostringstream s;
 switch(f.mType){
 case PreambleType:
  s<<preamble(f)<<" / "<<(channel(f)?"B-R":"A-L");
  if(preamble(f)=='B')s<<" BLOCK START";
  break;
 case AuxType:s<<"Aux="<<hex(raw&15,1)<<" (LSB4)";break;
 case MainType:
  s<<"Main20=";
  if(settings->display)s<<hex(main,5);else s<<main_signed;
  s<<"; Full24=";
  if(settings->display)s<<hex(payload,6);else s<<signed_sample(raw);
  s<<" (containers); "<<spdif::content_name(static_cast<spdif::ContentFormat>(f.mData1>>32));break;
 case VType:s<<"V="<<((raw>>24)&1)<<(((raw>>24)&1)?" invalid":" valid");break;
 case UType:s<<"U="<<((raw>>25)&1);break;
 case CType:s<<"C="<<((raw>>26)&1);break;
 case PType:s<<"P="<<((raw>>27)&1)<<(parity_ok(raw)?" parity OK":" PARITY ERROR");break;
 case SampleType:
  s<<(channel(f)?"B/R ":"A/L ")<<preamble(f)<<" ";
  if(settings->display)s<<hex(payload,6);else s<<signed_sample(raw)<<" (container24)";
  if(!parity_ok(raw))s<<" PARITY ERROR";
  if((raw>>24)&1)s<<" V=1";
  s<<"; "<<spdif::content_name(static_cast<spdif::ContentFormat>(f.mData1>>32));
  break;
 default:break;
 }
 return s.str();
}
void SpdifResults::GenerateBubbleText(U64 index,Channel&,DisplayBase){
 ClearResultStrings();auto f=GetFrame(index);
 // Saleae picks the longest supplied string which fits the current zoom.
 const char* names[]={"Preamble","Aux","Main20","V","U","C","P","Sample"};
 if(f.mType<8)AddResultString(names[f.mType]);
 auto t=Text(f);AddResultString(t.c_str());
}
void SpdifResults::GenerateFrameTabularText(U64 index,DisplayBase){ClearTabularText();auto t=Text(GetFrame(index));AddTabularText(t.c_str());}
void SpdifResults::GeneratePacketTabularText(U64,DisplayBase){ClearTabularText();}
void SpdifResults::GenerateTransactionTabularText(U64,DisplayBase){ClearTabularText();}
void SpdifResults::GenerateExportFile(const char* name,DisplayBase,U32 type){
 // Logic's Automation API exposes only the generic (id 0) export command.
 // Explicit non-default SDK export commands retain their named format.
 if(type==0)type=static_cast<U32>(settings->csv_export);
 std::ofstream out(name,std::ios::binary);if(!out)return;out.imbue(std::locale::classic());out<<std::setprecision(12);
 if(type==0)out<<"start_seconds,end_seconds,channel,preamble,block_position,raw_slots_4_31,payload_hex,signed_container_24,main_field_raw,auxiliary,V_invalid,U,C,P,parity_ok,measured_sample_rate_hz,sample_meaning,content\n";
 else if(type==1)out<<"start_seconds,end_seconds,channel,complete,integrity,C_bytes_earliest_first,U_bytes_earliest_first,interpretation\n";
 else if(type==2)out<<"start_seconds,end_seconds,code,message\n";
 else out<<"start_seconds,end_seconds,format,burst,complete,integrity,Pc,Pd,evidence\n";
 const auto trigger=analyzer->GetTriggerSample();const auto rate=analyzer->GetSampleRate();
 const auto time=[=](S64 sample){return (double(sample)-double(trigger))/rate;};
 U64 total=0;
 if(type==0)total=GetNumFrames();
 else {std::lock_guard<std::mutex> lock(mutex);total=type==1?status_blocks.size():type==2?diagnostics.size():content_events.size();}
 for(U64 i=0;i<total;++i){
  if(UpdateExportProgressAndCheckForCancel(i,total))break;
  if(type==0){
   auto f=GetFrame(i);
   if(f.mType==PreambleType){
    // Field mode writes exactly seven consecutive frames for each subframe.
    // Snapshot total may end mid-subframe during live export; omit incomplete rows.
    if(i+6>=total)break;
    auto last=GetFrame(i+6);
    if(last.mType!=PType)continue;
    f.mEndingSampleInclusive=last.mEndingSampleInclusive;
    i+=6;
   }else if(f.mType!=SampleType)continue;
   const uint32_t raw=uint32_t(f.mData1);
   out<<time(f.mStartingSampleInclusive)<<','<<time(f.mEndingSampleInclusive)<<','<<(channel(f)?'B':'A')<<','<<preamble(f)<<',';
   if(position(f)>=0)out<<position(f);
   out<<','<<hex(raw,7)<<','<<hex(raw&0xffffff,6)<<','<<signed_sample(raw)<<','<<hex((raw>>4)&0xfffff,5)<<','<<unsigned(raw&15);
   for(unsigned bit=24;bit<28;++bit)out<<','<<((raw>>bit)&1);
   out<<','<<parity_ok(raw)<<','<<measured(f)<<",numeric container only,"<<csv(spdif::content_name(static_cast<spdif::ContentFormat>(f.mData1>>32)))<<'\n';
  }else if(type==1){
   spdif::StatusBlock b;{std::lock_guard<std::mutex> lock(mutex);b=status_blocks.at(size_t(i));}
   out<<time(b.start)<<','<<time(b.end)<<','<<(b.channel?'B':'A')<<",1,"<<b.integrity<<','<<bytes(b.c)<<','<<bytes(b.u)<<','<<csv(b.interpretation)<<'\n';
  }else if(type==2){
   spdif::Diagnostic d;{std::lock_guard<std::mutex> lock(mutex);d=diagnostics.at(size_t(i));}
   out<<time(d.start)<<','<<time(d.end)<<','<<csv(d.code)<<','<<csv(d.message)<<'\n';
  }else{
   spdif::ContentInfo c;{std::lock_guard<std::mutex> lock(mutex);c=content_events.at(size_t(i));}
   out<<time(c.start)<<','<<time(c.end)<<','<<csv(spdif::content_name(c.format))<<','<<c.burst<<','<<c.complete<<','<<c.integrity<<','<<hex(c.pc,4)<<','<<c.pd<<','<<csv(c.evidence)<<'\n';
  }
 }
 UpdateExportProgressAndCheckForCancel(total,total);
}
