#include "SpdifAnalyzer.h"
#include "export/Exports.h"
#include "report/Events.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
namespace {
constexpr U8 PreambleType=0,PType=6,SampleType=7;
std::string hex(uint64_t x,int width){std::ostringstream o;o<<"0x"<<std::uppercase<<std::hex<<std::setw(width)<<std::setfill('0')<<x;return o.str();}
std::string bytes(const std::array<uint8_t,24>& b){std::ostringstream out;for(auto n:b)out<<std::uppercase<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(n);return out.str();}
std::string csv(const std::string& value){std::string out="\"";for(char c:value){if(c=='"')out+='"';out+=c;}return out+'"';}
int32_t signed_sample(uint32_t raw){const uint32_t p=raw&0xffffff;return (p&0x800000)?int32_t(p)-0x1000000:int32_t(p);}
bool parity(uint32_t raw){unsigned count=0;for(unsigned i=0;i<28;++i)count+=(raw>>i)&1;return !(count&1);}
void checked(std::ostream& out){if(!out)throw std::runtime_error("Cannot write analyzer export");}
}
void SpdifResults::PublishSnapshot(U64 observed_sample,const std::optional<spdif::BurstEvent>& pending_header){
 // Single worker, outside Decoder.edge: no callback batch or legacy field group
 // is half written. Read SDK scalars BEFORE acquiring the metadata mutex.
 Snapshot s;s.pending_header=pending_header;s.capture_rate=analyzer->GetSampleRate();s.trigger=analyzer->GetTriggerSample();
 s.detail=settings->detail;s.output=settings->output;s.csv_export=settings->csv_export;
 s.channel=settings->channel.mChannelIndex;s.pcm_bits=settings->pcm_bits;s.wav_rate=settings->wav_rate;s.assume_stereo=settings->assume_stereo;
 spdif::json::Object config;
 config.sample("device_id",settings->channel.mDeviceId).sample("trigger_sample",s.trigger).integer("sample_display",settings->display).integer("csv_export",s.csv_export).integer("export_output",s.output).integer("nominal_rate",settings->nominal).integer("waveform_detail",s.detail).integer("analysis_window_ms",settings->window_ms).integer("pcm_bits_override",s.pcm_bits).integer("wav_rate_override",s.wav_rate).boolean("assume_stereo",s.assume_stereo);
 s.settings_json=config.str();s.frames=legacy_frames_written;s.samples=s.frames/(s.detail==0?7:1);s.start=0;s.end=std::max(observed_sample,last_decoded_end);
 std::lock_guard<std::mutex> lock(mutex);
 s.status=status_blocks.size();s.diagnostics=diagnostics.size();s.content=content_events.size();s.changes=status_changes.size();s.pcm=pcm_windows.size();s.health=health_windows.size();s.bursts=burst_records.size();
 s.summary=health_summary;s.current_pcm=current_pcm;
 s.end=std::max(s.end,metadata_end);
 if(s.summary.end)s.end=std::max(s.end,U64(s.summary.end-1));
 if(s.bursts)s.tail_burst=burst_records.back();
 // Observe the raw-edge watermark without finalizing the live Analysis. These
 // intervals are current snapshots, not synthetic EOF or complete PCM windows.
 if(s.end!=~U64(0)&&s.summary.end<s.end+1){s.summary.end=s.end+1;const auto duration=s.summary.end-s.summary.start;s.summary.unknown_stream_samples=duration-std::min(duration,s.summary.decoded_stream_samples);for(auto& c:s.summary.channels)c.unknown_coverage_samples=duration-std::min(duration,c.decoded_coverage_samples);}
 s.ready=true;published_snapshot=std::move(s);
}
SpdifResults::Snapshot SpdifResults::Freeze(){
 std::lock_guard<std::mutex> lock(mutex);
 if(!published_snapshot.ready)throw std::runtime_error("Decoder has not published a snapshot yet");
 return published_snapshot;
}
spdif::Subframe SpdifResults::ReadSubframe(U64 index,const Snapshot& snapshot){
 const U64 stride=snapshot.detail==0?7:1;auto f=GetFrame(index*stride);spdif::Subframe s;
 if(stride==7){if(f.mType!=PreambleType)throw std::runtime_error("Unexpected legacy field ordering");for(U64 i=0;i<7;++i){auto part=GetFrame(index*stride+i);s.field_boundaries[size_t(i)]=part.mStartingSampleInclusive;if(i==6){if(part.mType!=PType)throw std::runtime_error("Incomplete legacy subframe");f.mEndingSampleInclusive=part.mEndingSampleInclusive;s.field_boundaries[7]=part.mEndingSampleInclusive+1;}}}
 else if(f.mType!=SampleType)throw std::runtime_error("Unexpected sample ordering");
 s.start=f.mStartingSampleInclusive;s.end=f.mEndingSampleInclusive;s.raw=uint32_t(f.mData1);s.sample=signed_sample(s.raw);s.preamble=char(f.mData2&255);s.channel=s.preamble=='W';s.block_position=int((f.mData2>>8)&511)-1;s.sample_rate=double(f.mData2>>17)/1000;
 s.content=static_cast<spdif::ContentFormat>((f.mData1>>32)&255);s.layout.bits=unsigned((f.mData1>>40)&31);s.layout.known=((f.mData1>>45)&1)!=0;s.layout.stereo=((f.mData1>>46)&1)!=0;s.layout.professional=((f.mData1>>47)&1)!=0;
 s.validity=((s.raw>>24)&1)!=0;s.user=((s.raw>>25)&1)!=0;s.status=((s.raw>>26)&1)!=0;s.parity=((s.raw>>27)&1)!=0;s.parity_ok=parity(s.raw);return s;
}
std::string SpdifResults::CsvHeader(unsigned kind)const{
 switch(kind){case 0:return "start_seconds,end_seconds,channel,preamble,block_position,raw_slots_4_31,payload_hex,signed_container_24,main_field_raw,auxiliary,V_invalid,U,C,P,parity_ok,measured_sample_rate_hz,sample_meaning,content,layout_bits,layout_known,layout_stereo,layout_professional";
 case 1:return "start_seconds,end_seconds,channel,complete,integrity,C_bytes_earliest_first,U_bytes_earliest_first,interpretation";
 case 2:return "start_seconds,end_seconds,code,message";
 default:return "start_seconds,end_seconds,format,burst,complete,integrity,Pc,Pd,evidence";}
}
void SpdifResults::CsvRow(unsigned kind,U64 row,std::ostream& out,const Snapshot& snapshot){
 out.imbue(std::locale::classic());out<<std::setprecision(12);
 const auto trigger=snapshot.trigger;const auto rate=snapshot.capture_rate;
 const auto time=[=](U64 n){return (double(n)-double(trigger))/rate;};
 if(kind==0){auto s=ReadSubframe(row,snapshot);out<<time(s.start)<<','<<time(s.end)<<','<<(s.channel?'B':'A')<<','<<s.preamble<<',';if(s.block_position>=0)out<<s.block_position;
  out<<','<<hex(s.raw,7)<<','<<hex(s.raw&0xffffff,6)<<','<<s.sample<<','<<hex((s.raw>>4)&0xfffff,5)<<','<<(s.raw&15)<<','<<s.validity<<','<<s.user<<','<<s.status<<','<<s.parity<<','<<s.parity_ok<<','<<s.sample_rate<<",numeric container only,"<<csv(spdif::content_name(s.content))<<','<<s.layout.bits<<','<<s.layout.known<<','<<s.layout.stereo<<','<<s.layout.professional;
 }else if(kind==1){spdif::StatusBlock b;{std::lock_guard<std::mutex> lock(mutex);b=status_blocks.at(size_t(row));}out<<time(b.start)<<','<<time(b.end)<<','<<(b.channel?'B':'A')<<",1,"<<b.integrity<<','<<bytes(b.c)<<','<<bytes(b.u)<<','<<csv(b.interpretation);
 }else if(kind==2){spdif::Diagnostic d;{std::lock_guard<std::mutex> lock(mutex);d=diagnostics.at(size_t(row));}out<<time(d.start)<<','<<time(d.end)<<','<<csv(d.code)<<','<<csv(d.message);
 }else{spdif::ContentInfo c;{std::lock_guard<std::mutex> lock(mutex);c=content_events.at(size_t(row));}out<<time(c.start)<<','<<time(c.end)<<','<<csv(spdif::content_name(c.format))<<','<<c.burst<<','<<c.complete<<','<<c.integrity<<','<<hex(c.pc,4)<<','<<c.pd<<','<<csv(c.evidence);}
}
void SpdifResults::ExportOutput(const char* name,const Snapshot& s){
 const auto cancelled=[&](U64 n,U64 total){return UpdateExportProgressAndCheckForCancel(n,total);};
 if(s.output==6){
  spdif::exports::BundleSpec spec;spec.input_channel="D"+std::to_string(s.channel);spec.capture_rate=s.capture_rate;spec.coverage_start=s.start;spec.coverage_end=s.end;
  spec.settings_json=s.settings_json;
  std::array<spdif::exports::CsvSource,4> source;const std::array<U64,4> counts={s.samples,s.status,s.diagnostics,s.content};
  U64 done=0,total=0;for(auto n:counts)total+=n;
  for(unsigned kind=0;kind<4;++kind){source[kind].header=CsvHeader(kind);source[kind].row_count=counts[kind];source[kind].write_row=[&,kind](uint64_t row,std::ostream& out){CsvRow(kind,row,out,s);++done;};}
  spdif::exports::write_bundle(std::string(name)+".bundle",spec,source,[&]{return cancelled(done,total);});return;
 }
 if(s.output==4){
  spdif::exports::WavWriter writer(std::string(name)+".wav-export",{double(s.capture_rate),s.pcm_bits,s.wav_rate,s.assume_stereo});
  size_t st=0,di=0;
  for(U64 n=0;n<s.samples;++n){if(cancelled(n,s.samples))throw std::runtime_error("WAV export cancelled; incomplete marker retained");auto f=ReadSubframe(n,s);
   while(st<s.status){spdif::StatusBlock b;{std::lock_guard<std::mutex> lock(mutex);b=status_blocks.at(st);}if(b.end>=f.start)break;writer.status(b);++st;}
   while(di<s.diagnostics){spdif::Diagnostic d;{std::lock_guard<std::mutex> lock(mutex);d=diagnostics.at(di);}if(d.start>f.start)break;writer.diagnostic(d);++di;}
   writer.subframe(f);
  }
  writer.finish(s.end+1);return;
 }
 if(s.output==5){
  spdif::exports::BurstWriter writer(std::string(name)+".bursts");const U64 stride=s.detail==0?7:1;
  for(size_t n=0;n<s.bursts;++n){if(cancelled(n,s.bursts))throw std::runtime_error("Payload export cancelled; incomplete marker retained");BurstRecord b;if(n+1==s.bursts)b=s.tail_burst;else{std::lock_guard<std::mutex> lock(mutex);b=burst_records.at(n);}writer.burst(b.begin);
   const U64 available=b.first_payload<s.frames?(s.frames-b.first_payload)/stride:0;
   const U64 words=std::min<U64>(b.words,available);uint64_t remaining=b.begin.declared_bits;
   for(U64 i=0;i<words;++i){if(cancelled(i,words))throw std::runtime_error("Payload export cancelled; incomplete marker retained");auto f=ReadSubframe(b.first_payload/stride+i,s);auto event=b.begin;event.phase=spdif::BurstPhase::Word;event.index=i;event.word=uint16_t(f.raw>>8);event.valid_bits=uint8_t(std::min<uint64_t>(remaining,16));event.info.end=f.end;writer.burst(event);remaining-=event.valid_bits;}
   if(b.closed&&b.end.info.end<=s.end&&words==b.words)writer.burst(b.end);
  }
  if(s.pending_header)writer.burst(*s.pending_header);
  writer.finish();return;
 }
 std::ofstream out(name,std::ios::binary);checked(out);
 if(s.output==1){auto o=spdif::json::event("summary");o.boolean("live_snapshot",true);out<<spdif::json::health_fields(o,s.summary)<<'\n';}
 else if(s.output==2){for(size_t i=0;i<s.changes;++i){if(cancelled(i,s.changes))throw std::runtime_error("Status change export cancelled");spdif::StatusChange change;{std::lock_guard<std::mutex> lock(mutex);change=status_changes.at(i);}out<<spdif::json::record(change)<<'\n';}}
 else if(s.output==3){for(size_t i=0;i<s.pcm;++i){if(cancelled(i,s.pcm))throw std::runtime_error("PCM statistics export cancelled");spdif::PcmWindow p;{std::lock_guard<std::mutex> lock(mutex);p=pcm_windows.at(i);}out<<spdif::json::record(p)<<'\n';}for(const auto& p:s.current_pcm){auto record=spdif::json::record(p);record.pop_back();out<<record<<",\"live_snapshot\":true}\n";}}
 out.flush();checked(out);out.close();checked(out);
}
void SpdifResults::GenerateExportFile(const char* name,DisplayBase,U32 kind){
 try{
  const auto snapshot=Freeze();
  if(kind==0&&snapshot.output!=0){ExportOutput(name,snapshot);UpdateExportProgressAndCheckForCancel(1,1);return;}
  if(kind==0)kind=unsigned(snapshot.csv_export);
  std::ofstream out(name,std::ios::binary);checked(out);out<<CsvHeader(kind)<<'\n';
  const std::array<U64,4> counts={snapshot.samples,snapshot.status,snapshot.diagnostics,snapshot.content};if(kind>3)throw std::invalid_argument("Unknown CSV export kind");
  for(U64 n=0;n<counts[kind];++n){if(UpdateExportProgressAndCheckForCancel(n,counts[kind]))break;CsvRow(kind,n,out,snapshot);out<<'\n';}
  out.flush();checked(out);out.close();checked(out);UpdateExportProgressAndCheckForCancel(counts[kind],counts[kind]);
 }catch(const std::exception& error){
  // SDK exposes no export error setter. Keep a separate explicit failure report;
  // directory writers also retain INCOMPLETE and never commit rejected payloads.
  std::ofstream out(std::string(name)+".error.json",std::ios::binary);out<<spdif::json::event("export_error").text("message",error.what()).boolean("complete",false).str()<<'\n';
 }
}
