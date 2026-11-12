#include "SpdifAnalyzer.h"
#include "report/Events.h"
#include "report/CodecEvents.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
namespace {
constexpr U8 PreambleType=0,AuxType=1,MainType=2,VType=3,UType=4,CType=5,PType=6,SampleType=7;
constexpr U8 StatusType=1,DiagnosticType=2,ContentType=3,HealthType=4,ChangeType=5,PcmType=6,CodecType=7;
std::string hex(uint64_t value,int width){ std::ostringstream s; s<<"0x"<<std::uppercase<<std::hex<<std::setw(width)<<std::setfill('0')<<value; return s.str(); }
int32_t signed_sample(uint32_t raw){ const auto p=raw&0xffffff;return (p&0x800000)?int32_t(p)-0x1000000:int32_t(p); }
char preamble(const Frame& f){return char(f.mData2&255);}
unsigned channel(const Frame& f){return preamble(f)=='W'?1:0;}
bool parity_ok(uint32_t raw){unsigned n=0;for(unsigned i=0;i<28;++i)n+=(raw>>i)&1;return (n&1)==0;}
Frame frame(U8 type,uint64_t start,uint64_t end){Frame f;f.mType=type;f.mFlags=0;f.mData1=0;f.mData2=0;f.mStartingSampleInclusive=start;f.mEndingSampleInclusive=std::max(start,end);return f;}
}
SpdifResults::SpdifResults(SpdifAnalyzer* a,SpdifSettings* s):analyzer(a),settings(s){}
void SpdifResults::Add(const spdif::Subframe& s){
 last_decoded_end=s.end;
 auto f=frame(SampleType,s.start,s.end); f.mData1=uint64_t(s.raw)|(uint64_t(s.content)<<32)|(uint64_t(s.layout.bits&31)<<40)|(uint64_t(s.layout.known)<<45)|(uint64_t(s.layout.stereo)<<46)|(uint64_t(s.layout.professional)<<47);
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
   AddFrame(part);++legacy_frames_written;
  }
 }else {AddFrame(f);++legacy_frames_written;}
 const U64 v2_start=PublishPending(s);
 if(settings->table_samples){
 FrameV2 v;const char p[]={s.preamble,0};v.AddString("channel",s.layout.stereo?(s.channel?"B / right":"A / left"):(s.channel?"B":"A"));v.AddString("preamble",p);
 v.AddInteger("raw_slots_4_31",s.raw);v.AddString("payload_hex",hex(s.raw&0xffffff,6).c_str());
 v.AddString("content",spdif::content_name(s.content));v.AddInteger("layout_bits",s.layout.bits);v.AddBoolean("layout_known",s.layout.known);v.AddBoolean("layout_stereo",s.layout.stereo);v.AddBoolean("layout_professional",s.layout.professional);
 v.AddInteger("signed_container_24",s.sample);v.AddByte("auxiliary",s.raw&15);
 v.AddInteger("main_field_raw",(s.raw>>4)&0xfffff);
 v.AddBoolean("V_invalid",s.validity);v.AddBoolean("U",s.user);v.AddBoolean("C",s.status);v.AddBoolean("P",s.parity);v.AddBoolean("parity_ok",s.parity_ok);
 if(s.block_position>=0)v.AddInteger("block_position",s.block_position);
 const char* boundaries[]={"preamble_start_sample","aux_start_sample","main_start_sample","V_start_sample","U_start_sample","C_start_sample","P_start_sample","subframe_end_exclusive_sample"};
 for(size_t i=0;i<s.field_boundaries.size();++i)v.AddInteger(boundaries[i],s.field_boundaries[i]);
 v.AddInteger("coverage_start_sample",s.start);v.AddInteger("coverage_end_sample",s.end);
 v.AddDouble("measured_sample_rate_hz",s.sample_rate);v.AddString("sample_meaning","Numeric container; consult channel status and integrity before interpreting as PCM");
 AddFrameV2(v,"subframe",v2_start,f.mEndingSampleInclusive);
 }
 if(!s.parity_ok)AddMarker(v2_start,ErrorX,settings->channel);
}
void SpdifResults::Add(const spdif::StatusBlock& b){
 std::lock_guard<std::mutex> lock(mutex);
 metadata_end=std::max(metadata_end,U64(b.end));status_blocks.push_back(b);
 if(pending.size()<7)pending.push_back({StatusType,status_blocks.size()-1});else ++suppressed_notifications;
}
void SpdifResults::Add(const spdif::Diagnostic& d){
 std::lock_guard<std::mutex> lock(mutex);
 metadata_end=std::max(metadata_end,U64(d.end));diagnostics.push_back(d);
 if(pending.size()<7)pending.push_back({DiagnosticType,diagnostics.size()-1});else ++suppressed_notifications;
}
void SpdifResults::Add(const spdif::ContentInfo& c){
 std::lock_guard<std::mutex> lock(mutex);
 metadata_end=std::max(metadata_end,U64(c.end));content_events.push_back(c);
 if(pending.size()<7)pending.push_back({ContentType,content_events.size()-1});else ++suppressed_notifications;
}
void SpdifResults::Add(const spdif::BurstEvent& event){
 const U64 first=legacy_frames_written+(settings->detail==0?7:1);
 std::lock_guard<std::mutex> lock(mutex);
 if(event.phase==spdif::BurstPhase::Begin){BurstRecord record;record.begin=event;record.first_payload=first;burst_records.push_back(record);}
 else if(!burst_records.empty()&&!burst_records.back().closed){auto& record=burst_records.back();if(event.phase==spdif::BurstPhase::Word)++record.words;else{record.end=event;record.closed=true;}}
}
void SpdifResults::Add(const spdif::codec::CodecInfo& c){
 std::lock_guard<std::mutex> lock(mutex);
 metadata_end=std::max(metadata_end,U64(c.transport.end));codec_events.push_back(c);
 if(pending.size()<7)pending.push_back({CodecType,codec_events.size()-1});else ++suppressed_notifications;
}
void SpdifResults::PublishAnalysis(SpdifAnalysisBatch& batch,const spdif::HealthWindow& summary,const std::vector<spdif::PcmWindow>& current){
 std::lock_guard<std::mutex> lock(mutex);
 const auto queue=[&](U8 type,size_t index){if(pending.size()<7)pending.push_back({type,index});else ++suppressed_notifications;};
 for(auto& h:batch.health){health_windows.push_back(std::move(h));queue(HealthType,health_windows.size()-1);}
 for(auto& c:batch.changes){status_changes.push_back(std::move(c));queue(ChangeType,status_changes.size()-1);}
 for(auto& p:batch.pcm){pcm_windows.push_back(std::move(p));queue(PcmType,pcm_windows.size()-1);}
 health_summary=summary;current_pcm=current;
 batch.health.clear();batch.changes.clear();batch.pcm.clear();
}
U64 SpdifResults::PublishPending(const spdif::Subframe& s){
 // Legacy bubbles contain only sample fields (or compact subframes). Structured notifications use a short
 // prefix of the next subframe, never overlapping another FrameV2. Their true
 // coverage remains in named fields and the metadata CSV exports.
 U64 cursor=s.start;
 struct Copy { U8 type; spdif::StatusBlock status; spdif::Diagnostic diagnostic; spdif::ContentInfo content;spdif::HealthWindow health;spdif::StatusChange change;spdif::PcmWindow pcm;spdif::codec::CodecInfo codec; };
 std::vector<Copy> notifications;
 U64 suppressed=0;
 {
  std::lock_guard<std::mutex> lock(mutex);
  notifications.reserve(pending.size());
  for(const auto& event:pending){
   Copy copy;copy.type=event.type;
   if(event.type==StatusType)copy.status=status_blocks.at(event.index);
   else if(event.type==ContentType)copy.content=content_events.at(event.index);
   else if(event.type==DiagnosticType)copy.diagnostic=diagnostics.at(event.index);
   else if(event.type==HealthType)copy.health=health_windows.at(event.index);
   else if(event.type==ChangeType)copy.change=status_changes.at(event.index);
   else if(event.type==CodecType)copy.codec=codec_events.at(event.index);
   else copy.pcm=pcm_windows.at(event.index);
   notifications.push_back(std::move(copy));
  }
  pending.clear();suppressed=suppressed_notifications;suppressed_notifications=0;
 }
 // No calls into Saleae while holding the metadata mutex: export runs on a
 // separate SDK thread and can otherwise invert metadata/SDK lock ordering.
 for(const auto& event:notifications){
  FrameV2 v;
  if(event.type==CodecType){
   const auto& c=event.codec;const auto& f=c.frame;
   v.AddString("codec",spdif::codec::codec_name(f.codec));v.AddString("packing",spdif::codec::packing_name(f.packing));v.AddString("header_state",spdif::codec::header_state_name(f.header));
   v.AddInteger("sample_rate_hz",f.sample_rate);v.AddInteger("channels",f.channels);v.AddBoolean("channel_count_known",f.channel_count_known);v.AddInteger("frame_bytes",f.frame_bytes);v.AddInteger("samples_per_frame",f.samples_per_frame);
   v.AddString("structure",spdif::integrity_name(f.structure));v.AddString("crc1",spdif::integrity_name(f.crc1));v.AddString("header_crc",spdif::integrity_name(f.header_crc));v.AddString("frame_crc",spdif::integrity_name(f.frame_crc));
   v.AddBoolean("transport_complete",c.transport.complete);v.AddBoolean("transport_integrity",c.transport.integrity);v.AddBoolean("transport_conflict",f.transport_conflict);
   v.AddString("channel_configuration",f.channel_configuration.c_str());if(f.bit_rate)v.AddInteger("bit_rate",*f.bit_rate);
   v.AddInteger("coverage_start_sample",c.start);v.AddInteger("coverage_end_sample",c.end);v.AddBoolean("source_interval_known",c.source_interval_known);
   const auto record=spdif::json::record(c);v.AddString("json",record.c_str());AddFrameV2(v,"codec_frame",cursor,cursor);
  }else if(event.type==StatusType){
   const auto& b=event.status;
   const auto info=spdif::status_info(b.c);const bool stereo=b.integrity&&info.supported&&info.stereo;
   v.AddString("channel",stereo?(b.channel?"B / right":"A / left"):(b.channel?"B":"A"));
   v.AddByteArray("C",b.c.data(),b.c.size());v.AddByteArray("U",b.u.data(),b.u.size());
   v.AddInteger("coverage_start_sample",b.start);v.AddInteger("coverage_end_sample",b.end);
   v.AddBoolean("complete",true);v.AddBoolean("integrity",b.integrity);v.AddString("status_crc",spdif::integrity_name(spdif::status_crc(b.c)));v.AddInteger("status_crc_received",b.c[23]);v.AddInteger("status_crc_expected",(b.c[0]&1)?spdif::professional_crc(b.c):0);v.AddBoolean("audio_fields_reliable",spdif::status_info(b.c).audio_fields_reliable);v.AddString("interpretation",b.interpretation.c_str());
   AddFrameV2(v,"channel_status",cursor,cursor);
  }else if(event.type==ContentType){
   const auto& c=event.content;
   v.AddInteger("coverage_start_sample",c.start);v.AddInteger("coverage_end_sample",c.end);
   v.AddString("format",spdif::content_name(c.format));v.AddString("evidence",c.evidence.c_str());
   v.AddInteger("Pc",c.pc);v.AddInteger("Pd",c.pd);v.AddBoolean("burst",c.burst);
   v.AddBoolean("complete",c.complete);v.AddBoolean("integrity",c.integrity);
   AddFrameV2(v,"content",cursor,cursor);
  }else if(event.type>=HealthType){
   if(event.type==HealthType){
    const auto& h=event.health;v.AddString("record_json",spdif::json::record(h).c_str());
    v.AddInteger("coverage_start_sample",h.start);v.AddInteger("coverage_end_exclusive_sample",h.end);
    v.AddInteger("decoded_A",h.channels[0].decoded_subframes);v.AddInteger("decoded_B",h.channels[1].decoded_subframes);
    v.AddInteger("parity_A",h.channels[0].parity_failures);v.AddInteger("parity_B",h.channels[1].parity_failures);
    v.AddInteger("V_invalid_A",h.channels[0].v_invalid);v.AddInteger("V_invalid_B",h.channels[1].v_invalid);
    v.AddInteger("sync_losses",h.sync_losses);v.AddInteger("unknown_stream_samples",h.unknown_stream_samples);
   }else if(event.type==ChangeType){
    const auto& c=event.change;v.AddString("record_json",spdif::json::record(c).c_str());v.AddString("channel",c.channel?"B":"A");
    v.AddInteger("coverage_start_sample",c.start);v.AddInteger("coverage_end_exclusive_sample",c.end);
    v.AddByteArray("changed_Cmask",c.changed_mask.data(),c.changed_mask.size());
    v.AddString("old_status",spdif::interpret_status(c.previous).c_str());v.AddString("new_status",spdif::interpret_status(c.current).c_str());
   }else{
    const auto& p=event.pcm;v.AddString("record_json",spdif::json::record(p).c_str());v.AddString("channel",p.channel?"B":"A");
    v.AddInteger("coverage_start_sample",p.start);v.AddInteger("coverage_end_exclusive_sample",p.end);v.AddInteger("bits",p.bits);v.AddInteger("accepted",p.accepted);
    v.AddInteger("excluded",p.excluded_parity+p.excluded_invalid+p.excluded_unknown+p.excluded_non_pcm+p.excluded_layout);
    v.AddInteger("peak",p.peak);v.AddDouble("RMS",p.rms);v.AddDouble("DC_mean",p.mean);v.AddDouble("silence_seconds",p.silence_seconds);v.AddInteger("full_scale_runs",p.full_scale_runs);
   }
   AddFrameV2(v,event.type==HealthType?"health_window":event.type==ChangeType?"status_change":"pcm_window",cursor,cursor);
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
  s<<preamble(f)<<" / "<<((f.mData1&(1ull<<46))?(channel(f)?"B-R":"A-L"):(channel(f)?"B":"A"));
  if(preamble(f)=='B')s<<" BLOCK START";
  break;
 case AuxType:s<<"Aux="<<hex(raw&15,1)<<" (LSB4)";break;
 case MainType:
  s<<"Main20=";
  if(settings->display)s<<hex(main,5);else s<<main_signed;
  s<<"; Full24=";
  if(settings->display)s<<hex(payload,6);else s<<signed_sample(raw);
  s<<" (containers); "<<spdif::content_name(static_cast<spdif::ContentFormat>((f.mData1>>32)&255));break;
 case VType:s<<"V="<<((raw>>24)&1)<<(((raw>>24)&1)?" invalid":" valid");break;
 case UType:s<<"U="<<((raw>>25)&1);break;
 case CType:s<<"C="<<((raw>>26)&1);break;
 case PType:s<<"P="<<((raw>>27)&1)<<(parity_ok(raw)?" parity OK":" PARITY ERROR");break;
 case SampleType:
  s<<((f.mData1&(1ull<<46))?(channel(f)?"B/R ":"A/L "):(channel(f)?"B ":"A "))<<preamble(f)<<" ";
  if(settings->display)s<<hex(payload,6);else s<<signed_sample(raw)<<" (container24)";
  if(!parity_ok(raw))s<<" PARITY ERROR";
  if((raw>>24)&1)s<<" V=1";
  s<<"; "<<spdif::content_name(static_cast<spdif::ContentFormat>((f.mData1>>32)&255));
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
