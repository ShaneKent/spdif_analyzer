#include "Codec.h"
#include <algorithm>
#include <array>
#include <limits>
#include <utility>
#include <vector>

namespace spdif::codec {
const char* header_state_name(HeaderState s){switch(s){case HeaderState::Ok:return "ok";case HeaderState::Truncated:return "truncated";case HeaderState::Unsupported:return "unsupported";default:return "invalid";}}
const char* codec_name(CodecKind c){switch(c){case CodecKind::Ac3:return "AC-3";case CodecKind::Eac3:return "E-AC-3";case CodecKind::DtsCore:return "DTS core";default:return "unknown";}}
const char* packing_name(Packing p){switch(p){case Packing::BigEndian16:return "16-bit big endian";case Packing::LittleEndian16:return "16-bit little endian";case Packing::BigEndian14:return "14-bit big endian";case Packing::LittleEndian14:return "14-bit little endian";default:return "unknown";}}
namespace {
// Specification bit ordering, independent of host endianness and alignment.
class Bits {
 const uint8_t* data; uint32_t physical; Packing packing;
 public:
 uint32_t position=0; bool short_input=false;
 Bits(const uint8_t* d,uint32_t bits,Packing p):data(d),physical(bits),packing(p){}
 bool packed14()const{return packing==Packing::BigEndian14||packing==Packing::LittleEndian14;}
 bool little()const{return packing==Packing::LittleEndian16||packing==Packing::LittleEndian14;}
 uint32_t available()const{return packed14()?(physical/16)*14:little()?(physical/16)*16:physical;}
 unsigned bit(uint32_t logical)const{
  uint32_t offset=logical;
  if(packed14())offset=(logical/14)*16+2+logical%14;
  uint32_t byte=offset/8;if(little())byte^=1u;
  return (data[byte]>>(7-(offset%8)))&1;
 }
 uint32_t get(unsigned n){
  if(n>32||position>available()||n>available()-position){short_input=true;return 0;}
  uint32_t v=0;for(unsigned i=0;i<n;++i)v=(v<<1)|bit(position++);return v;
 }
 void skip(unsigned n){(void)get(n);}
 uint8_t byte(uint32_t n)const{uint8_t v=0;for(unsigned i=0;i<8;++i)v=uint8_t((v<<1)|bit(n*8+i));return v;}
};
FrameResult fail(FrameResult r,HeaderState s,const char* reason){r.header=s;r.structure=s==HeaderState::Truncated?CheckState::Truncated:s==HeaderState::Unsupported?CheckState::Unsupported:CheckState::Failed;r.evidence=reason;r.consumed_bytes=0;return r;}
uint16_t crc16(const Bits& b,unsigned begin,unsigned end){
 // ETSI TS102366 6.10.1: polynomial x^16+x^15+x^2+1, initial0, MSB first.
 uint16_t reg=0;
 for(unsigned n=begin;n<end;++n){reg^=uint16_t(b.byte(n))<<8;for(unsigned i=0;i<8;++i)reg=uint16_t((reg<<1)^((reg&0x8000)?0x8005:0));}
 return reg;
}
void channels(FrameResult& r){
 static const unsigned count[]={2,1,2,3,3,4,4,5};
 static const char* names[]={"A+B (dual mono)","C (mono)","L+R (stereo)","L+C+R","L+R+S","L+C+R+S","L+R+Ls+Rs","L+C+R+Ls+Rs"};
 r.channels=count[r.channel_mode]+unsigned(r.lfe);r.channel_count_known=true;r.channel_configuration=names[r.channel_mode];if(r.lfe)r.channel_configuration+="+LFE";
}
bool expected_mismatch(const FrameResult& r,ContentFormat f){
 if(f==ContentFormat::Ac3)return r.codec!=CodecKind::Ac3;
 if(f==ContentFormat::Eac3)return r.codec!=CodecKind::Eac3;
 if(f==ContentFormat::Dts1||f==ContentFormat::Dts2||f==ContentFormat::Dts3){
  if(r.codec!=CodecKind::DtsCore)return true;
  return r.samples_per_frame && r.samples_per_frame!=(f==ContentFormat::Dts1?512u:f==ContentFormat::Dts2?1024u:2048u);
 }
 return false;
}
FrameResult ac3(Bits& b,FrameResult r){
 if(b.available()<48)return fail(std::move(r),HeaderState::Truncated,"Incomplete AC-3/E-AC-3 common identification header");
 b.position=40;r.bsid=int(b.get(5));b.position=16;
 if(r.bsid==9||r.bsid==10||r.bsid>16)return fail(std::move(r),HeaderState::Unsupported,"Unsupported Dolby bitstream identification (bsid9/10 or future syntax); raw header retained");
 if(r.bsid<=8){
  r.codec=CodecKind::Ac3;r.crc1=CheckState::Truncated;r.frame_crc=CheckState::Truncated;
  b.skip(16);r.sample_rate_code=int(b.get(2));r.frame_size_code=int(b.get(6));b.skip(5);r.bsmod=int(b.get(3));r.channel_mode=int(b.get(3));
  if(b.short_input)return fail(std::move(r),HeaderState::Truncated,"Incomplete AC-3 audio coding mode");
  if(r.sample_rate_code==3||r.frame_size_code>37)return fail(std::move(r),HeaderState::Invalid,"Reserved AC-3 sample rate or frame size code");
  // TS102366 table4.13, frame sizes in16-bit words; 44.1k alternates codes.
  static const unsigned kbps[]={32,40,48,56,64,80,96,112,128,160,192,224,256,320,384,448,512,576,640};
  static const unsigned words441[]={69,87,104,121,139,174,208,243,278,348,417,487,557,696,835,975,1114,1253,1393};
  const unsigned rate=kbps[unsigned(r.frame_size_code)/2];
  r.sample_rate=r.sample_rate_code==0?48000:r.sample_rate_code==1?44100:32000;
  const unsigned words=r.sample_rate_code==0?rate*2:r.sample_rate_code==2?rate*3:words441[unsigned(r.frame_size_code)/2]+unsigned(r.frame_size_code%2);
  r.frame_bytes=words*2;r.samples_per_frame=1536;r.bit_rate=rate*1000;r.bit_rate_kind="nominal";
  if((r.channel_mode&1)&&r.channel_mode!=1)b.skip(2);
  if(r.channel_mode&4)b.skip(2);
  if(r.channel_mode==2)b.skip(2);
  r.lfe=b.get(1)!=0;
  if(b.short_input)return fail(std::move(r),HeaderState::Truncated,"Incomplete AC-3 conditional channel/LFE header");
  channels(r);r.header_bits=b.position;
  const unsigned split=((words>>1)+(words>>3))*2;
  if(b.available()>=split*8)r.crc1=crc16(b,2,split)?CheckState::Failed:CheckState::Passed;
  if(b.available()>=r.frame_bytes*8)r.frame_crc=crc16(b,2,r.frame_bytes)?CheckState::Failed:CheckState::Passed;
  r.evidence="AC-3 common header; CRC1 covers first5/8 excluding sync; frame CRC covers entire frame excluding sync; audio syntax not decoded";
 }else{
  r.codec=CodecKind::Eac3;r.frame_crc=CheckState::Truncated;
  r.stream_type=int(b.get(2));r.substream_id=int(b.get(3));r.frame_size_code=int(b.get(11));r.frame_bytes=unsigned(r.frame_size_code+1)*2;
  r.sample_rate_code=int(b.get(2));unsigned blocks=6;
  if(r.sample_rate_code==3){const unsigned reduced=b.get(2);if(reduced==3)return fail(std::move(r),HeaderState::Invalid,"Reserved E-AC-3 reduced sample rate");r.sample_rate=reduced==0?24000:reduced==1?22050:16000;}
  else{static const unsigned numblocks[]={1,2,3,6};blocks=numblocks[b.get(2)];r.sample_rate=r.sample_rate_code==0?48000:r.sample_rate_code==1?44100:32000;}
  r.channel_mode=int(b.get(3));r.lfe=b.get(1)!=0;b.skip(5);
  if(b.short_input)return fail(std::move(r),HeaderState::Truncated,"Incomplete E-AC-3 fixed header");
  if(r.stream_type==3)return fail(std::move(r),HeaderState::Invalid,"Reserved E-AC-3 stream type");
  channels(r);r.samples_per_frame=blocks*256;r.bit_rate=uint32_t(uint64_t(r.frame_bytes)*8*r.sample_rate/r.samples_per_frame);r.bit_rate_kind="derived per frame";
  // Parse only through the dependent channel map, never infer a presentation
  // layout by adding unrelated independent/dependent substreams together.
  b.skip(5);if(b.get(1))b.skip(8);
  if(r.channel_mode==0){b.skip(5);if(b.get(1))b.skip(8);}
  if(r.stream_type==1&&b.get(1)){
   r.channel_map=uint16_t(b.get(16));
   if(!b.short_input){
    static const unsigned weight[]={1,1,1,1,1,2,2,1,1,2,2,2,1,2,1,1};
    unsigned count=0;for(unsigned i=0;i<16;++i)if(*r.channel_map&(0x8000u>>i))count+=weight[i];
    if(count!=r.channels)return fail(std::move(r),HeaderState::Invalid,"E-AC-3 dependent channel map disagrees with coded channel count");
    static const char* location[]={"L","C","R","Ls","Rs","Lc/Rc","Lrs/Rrs","Cs","Ts","Lsd/Rsd","Lw/Rw","Vhl/Vhr","Vhc","Lts/Rts","LFE2","LFE"};
    r.channel_configuration.clear();for(unsigned i=0;i<16;++i)if(*r.channel_map&(0x8000u>>i)){if(!r.channel_configuration.empty())r.channel_configuration+="+";r.channel_configuration+=location[i];}
    r.channel_configuration+=" (dependent substream)";
   }
  }
  if(b.short_input)return fail(std::move(r),HeaderState::Truncated,"Incomplete E-AC-3 dialnorm/compression/channel map prefix");
  r.header_bits=b.position;
  if(r.frame_bytes<(r.header_bits+7)/8+2)return fail(std::move(r),HeaderState::Invalid,"E-AC-3 declared frame cannot contain parsed header and CRC");
  if(b.available()>=r.frame_bytes*8)r.frame_crc=crc16(b,2,r.frame_bytes)?CheckState::Failed:CheckState::Passed;
  r.evidence="E-AC-3 individual substream header and whole-frame CRC excluding sync; no aggregate presentation or audio decode";
 }
 r.header=HeaderState::Ok;
 r.structure=b.available()>=r.frame_bytes*8?CheckState::Passed:CheckState::Truncated;
 if(r.structure==CheckState::Passed)r.consumed_bytes=r.frame_bytes;
 return r;
}
FrameResult dts(Bits& b,FrameResult r){
 r.codec=CodecKind::DtsCore;r.frame_crc=CheckState::Unsupported;
 // The physical 14-bit marker contains only28 logical sync bits. Check the
 // complete normalized word, including its final nibble in transport word3.
 const auto sync=b.get(32);
 if(b.short_input)return fail(std::move(r),HeaderState::Truncated,"Incomplete normalized DTS synchronization word");
 if(sync!=0x7ffe8001u)return fail(std::move(r),HeaderState::Invalid,"Invalid normalized DTS synchronization word");
 const unsigned normal=b.get(1),deficit=b.get(5);r.crc_present=b.get(1)!=0;
 const unsigned blocks=b.get(7)+1;r.frame_bytes=b.get(14)+1;r.channel_mode=int(b.get(6));r.sample_rate_code=int(b.get(4));r.rate_code=int(b.get(5));const unsigned fixed=b.get(1);
 b.skip(4);b.skip(3);r.extension_audio=b.get(1)!=0;b.skip(1);const unsigned lff=b.get(2);b.skip(1);
 r.header_crc=r.crc_present?CheckState::NotChecked:CheckState::Absent;
 if(r.crc_present)b.skip(16);
 b.skip(1);const unsigned version=b.get(4);b.skip(2);const unsigned pcmr=b.get(3);b.skip(2);b.skip(4);
 if(b.short_input){if(r.crc_present)r.header_crc=CheckState::Truncated;return fail(std::move(r),HeaderState::Truncated,"Incomplete DTS core bitstream header");}
 r.header_bits=b.position;r.termination_frame=!normal;r.deficit_samples=normal?0:deficit+1;r.samples_per_frame=blocks*32;
 static const unsigned sample_rates[]={0,8000,16000,32000,0,0,11025,22050,44100,0,0,12000,24000,48000,0,0};
 static const unsigned rates[]={32000,56000,64000,96000,112000,128000,192000,224000,256000,320000,384000,448000,512000,576000,640000,768000,960000,1024000,1152000,1280000,1344000,1408000,1411200,1472000,1536000};
 r.sample_rate=sample_rates[r.sample_rate_code];r.bit_rate_kind="target transmission";if(r.rate_code<25)r.bit_rate=rates[r.rate_code];
 if(r.frame_bytes<96||blocks<6||!r.sample_rate||lff==3||fixed||pcmr==4||pcmr==7 || (r.rate_code>24&&r.rate_code!=29))return fail(std::move(r),HeaderState::Invalid,"Reserved or out-of-range DTS core header field (size/blocks/rate/LFE/reserved/PCM resolution)");
 if((normal&&deficit!=31)||(!normal&&deficit==31))return fail(std::move(r),HeaderState::Invalid,"DTS frame type contradicts deficit sample count");
 if(normal && blocks!=8&&blocks!=16&&blocks!=32&&blocks!=64&&blocks!=128)return fail(std::move(r),HeaderState::Invalid,"DTS normal frame has unsupported nonstandard block count");
 static const unsigned count[]={1,2,2,2,2,3,3,4,4,5,6,6,6,7,8,8};
 static const char* modes[]={"A (mono)","A+B (dual mono)","L+R (stereo)","(L+R)+(L-R)","Lt+Rt","C+L+R","L+R+S","C+L+R+S","L+R+SL+SR","C+L+R+SL+SR","CL+CR+L+R+SL+SR","C+L+R+LR+RR+OV","CF+CR+LF+RF+LR+RR","CL+C+CR+L+R+SL+SR","CL+CR+L+R+SL1+SL2+SR1+SR2","CL+C+CR+L+R+SL+S+SR"};
 r.lfe=lff!=0;
 if(r.channel_mode<16){r.channel_count_known=true;r.channels=count[r.channel_mode]+unsigned(r.lfe);r.channel_configuration=modes[r.channel_mode];if(r.lfe)r.channel_configuration+="+LFE";}
 else r.channel_configuration="user defined DTS AMODE";
 if(version>7)return fail(std::move(r),HeaderState::Unsupported,"DTS encoder revision is incompatible with the supported specification");
 r.evidence="DTS core bitstream header and physical frame extent only; HCRC deliberately not verified per ETSI102114 5.3.1; extension/audio integrity unsupported";
 unsigned physical_bytes=r.frame_bytes;
 if(b.packed14()){
  // libdcadec frame API defines whole transport words, including for odd
  // normalized sizes. No audio/padding validation is claimed by structure.
  physical_bytes=(r.frame_bytes*8/14)*2;
  r.evidence+="; packed14 boundary uses whole-word libdcadec convention";
 }
 r.header=HeaderState::Ok;r.structure=physical_bytes*8<=b.available()?CheckState::Passed:CheckState::Truncated;
 // available() is logical for packed14; compare the corresponding complete
 // physical words directly, avoiding any normalized-tail rounding claim.
 if(b.packed14())r.structure=(b.available()/14)*2>=physical_bytes?CheckState::Passed:CheckState::Truncated;
 if(r.structure==CheckState::Passed)r.consumed_bytes=physical_bytes;
 if(r.channel_mode>=16){r.header=HeaderState::Unsupported;r.evidence+="; user-defined channel arrangement not interpreted";}
 return r;
}
}
FrameResult inspect_frame(const uint8_t* data,size_t size,uint32_t valid_bits,ContentFormat expected){
 FrameResult r;
 if(size>max_payload_bytes||valid_bits>max_payload_bytes*8||valid_bits>size*8||(!data&&size))return fail(r,HeaderState::Invalid,"Codec input exceeds bounded extent or has invalid pointer/valid-bit count");
 if(valid_bits<16)return fail(r,HeaderState::Truncated,"Incomplete codec synchronization word");
 if(data[0]==0x0b&&data[1]==0x77)r.packing=Packing::BigEndian16;
 else if(data[0]==0x77&&data[1]==0x0b)r.packing=Packing::LittleEndian16;
 else{
  if(valid_bits<32)return fail(r,HeaderState::Truncated,"Incomplete codec sync/header prefix");
  const uint32_t sync=(uint32_t(data[0])<<24)|(uint32_t(data[1])<<16)|(uint32_t(data[2])<<8)|data[3];
  switch(sync){case 0x7ffe8001:r.packing=Packing::BigEndian16;break;case 0xfe7f0180:r.packing=Packing::LittleEndian16;break;case 0x1fffe800:r.packing=Packing::BigEndian14;break;case 0xff1f00e8:r.packing=Packing::LittleEndian14;break;case 0x64582025:return fail(r,HeaderState::Unsupported,"DTS extension substream header unsupported; no core sync at declared boundary");default:return fail(r,HeaderState::Invalid,"No supported codec synchronization word at declared boundary");}
 }
 Bits bits(data,valid_bits,r.packing);
 if(data[0]==0x0b||data[0]==0x77)r=ac3(bits,std::move(r));else r=dts(bits,std::move(r));
 if(r.codec!=CodecKind::Unknown && expected_mismatch(r,expected)){
  r.transport_conflict=true;r.evidence+="; codec header contradicts IEC61937 declared type/duration";
 }
 return r;
}
struct Inspector::Impl {
 Callback callback;bool active=false,done=false,malformed=false;BurstEvent begin{};uint64_t bits=0,last_end=0;
 std::vector<uint8_t> bytes;struct Coverage{uint64_t start,end;bool known;};std::vector<Coverage> coverage;
 explicit Impl(Callback c):callback(std::move(c)){bytes.reserve(max_payload_bytes);coverage.reserve((max_payload_bytes+1)/2);}
 ContentFormat expected()const{switch(begin.info.pc&127){case 1:return ContentFormat::Ac3;case 11:return ContentFormat::Dts1;case 12:return ContentFormat::Dts2;case 13:return ContentFormat::Dts3;case 21:return ContentFormat::Eac3;default:return ContentFormat::Unknown;}}
 void emit(const ContentInfo& transport,bool stream_complete,const char* reason){
  if(!active)return;
  size_t offset=0;uint64_t index=0;
  do{
   CodecInfo info;info.transport=transport;info.frame_index=index++;info.byte_offset=offset;
   const uint32_t remaining=bits>offset*8?uint32_t(bits-offset*8):0;
   info.frame=inspect_frame(bytes.empty()?nullptr:bytes.data()+offset,bytes.size()-offset,remaining,expected());
   if(malformed){info.frame.structure=CheckState::Failed;info.frame.evidence+="; malformed codec input event ordering/length/identity";}
   if(!stream_complete){info.frame.evidence+="; ";info.frame.evidence+=reason;}
   const size_t extent=info.frame.consumed_bytes?info.frame.consumed_bytes:bytes.size()-offset;
   if(extent && !coverage.empty()){
    const size_t first=offset/2,last=std::min((offset+extent-1)/2,coverage.size()-1);
    bool known=first<coverage.size();
    for(size_t word=first;known&&word<=last;++word){
     const auto& interval=coverage[word];
     known=interval.known&&interval.start<=interval.end;
     if(known&&word>first){
      const auto previous=coverage[word-1].end;
      known=previous!=std::numeric_limits<uint64_t>::max()&&interval.start==previous+1;
     }
    }
    if(known){info.start=coverage[first].start;info.end=coverage[last].end;info.source_interval_known=true;}
   }
   if(callback)callback(info);
   if(!info.frame.consumed_bytes||info.frame.header!=HeaderState::Ok||malformed)break;
   offset+=info.frame.consumed_bytes;
  }while(offset<bytes.size());
  active=false;bytes.clear();coverage.clear();bits=0;
 }
 void event(const BurstEvent& e){
  if(done)return;
  if(e.phase==BurstPhase::Begin){
   if(active){auto previous=begin.info;previous.complete=previous.integrity=false;previous.end=last_end;emit(previous,false,"new burst before end confirmation");}
   begin=e;active=true;malformed=!e.supported||e.declared_bits>max_payload_bytes*8;bytes.clear();coverage.clear();bits=0;last_end=e.info.end;return;
  }
  if(!active)return;
  if(e.phase==BurstPhase::Word){
   const uint64_t left=begin.declared_bits>bits?begin.declared_bits-bits:0;
   const unsigned wanted=unsigned(std::min<uint64_t>(16,left));
   if(malformed||e.index!=coverage.size()||!wanted||e.valid_bits!=wanted||e.info.pc!=begin.info.pc||e.info.pd!=begin.info.pd||bytes.size()+2>max_payload_bytes){malformed=true;return;}
   const bool known=last_end!=std::numeric_limits<uint64_t>::max()&&e.info.end>last_end;
   coverage.push_back({known?last_end+1:0,e.info.end,known});last_end=e.info.end;
   // Store only bytes containing valid bits; unused lows never influence CRC.
   bytes.push_back(uint8_t(e.word>>8));if(wanted>8)bytes.push_back(uint8_t(e.word));bits+=wanted;
   return;
  }
  if(e.info.pc!=begin.info.pc||e.info.pd!=begin.info.pd||bits!=begin.declared_bits)malformed=true;
  emit(e.info,e.info.complete&&e.info.integrity&&!malformed,"transport end unconfirmed/incomplete/damaged");
 }
};
Inspector::Inspector(Callback c):impl(new Impl(std::move(c))){}
Inspector::~Inspector()=default;
void Inspector::burst(const BurstEvent& e){impl->event(e);}
void Inspector::finish(){if(impl->done)return;if(impl->active){auto info=impl->begin.info;info.end=impl->last_end;info.complete=info.integrity=false;impl->emit(info,false,"end of input before transport confirmation");}impl->done=true;}
}
