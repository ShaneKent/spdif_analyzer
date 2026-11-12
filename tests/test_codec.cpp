#include "codec/Codec.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace spdif;
using namespace spdif::codec;
namespace {
unsigned checks=0;
void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
std::vector<uint8_t> hex(const std::string& s){std::vector<uint8_t> v;for(size_t i=0;i<s.size();i+=2)v.push_back(uint8_t(std::stoul(s.substr(i,2),nullptr,16)));return v;}
// Independent encoder output, FFmpeg6.1.1 Ubuntu13gcc. Generated with lavfi
// anullsrc=r=48000:cl=stereo, -frames:a1 -c:a ac3/eac3 -b:a32k. First128bytes.
// ffprobe independently reports48kHz,2channels,32kbit/s,1536samples,128bytes.
const auto ac3_reference=hex("0b773f11004043e106f00108080821010104571f3e7cf9f3e7cf9f3e7cfbfe757cf9f3e7cf9f3e7cf8ff9d5f3e7cf9f3e7cf9f3e32fede4c99200000f1e35ad680035ac3000000078f1ad6b4001ad6180000003c78d6b5a000d6b0c0000001e3c6b5ad0006b5860000000f1e35ad680035ac3000000078f1ad6b4001ad60e48e");
const auto eac3_reference=hex("0b77003f3487c00020000000388000040404010101018f9f3e7cf9f3e7cf9f3e7dff3abe7cf9f3e7cf9f3e7c7fceaf9f3e7cf9f3e7cf9f000001de35ad6b4001dad6b000003bc6b5ad68003b5ad600000778d6b5ad00076b5ac00000ef1ad6b5a000ed6b5800001de35ad6b4001dad6b000003bc6b5ad68003b5ad600000d5c4");
// FFmpeg dca -strict -2 -b:a384k, first complete512byte frame. These bytes are
// reference data, not a parser/encoder algorithm copied from FFmpeg.
const auto dts_reference=hex("7ffe8001fc3c1ff0b54001380003ef7fe006c91e1e0db6db6000000000200000000000000001eebae5cb972ebaebdef7bdb3355000001eebae5cb972ebaebdef7bdb335500000000000000000000000020408102040810200000000000000000000000000000000000000000204081020408102000000000000000000000000000000000000002492492222222222222222000000000000000000000000000000000000000000000000000000000000000022222222222222222222222224924924924924924924924924924915556aaaaaaa80000000000000000000000000002492492222222222222222000000000000000000000000000000000000000000000000000000000000000022222222222222222222222224924924924924924924924924924915556aaaaaaa80000000000000000000000000002492492222222222222222000000000000000000000000000000000000000000000000000000000000000022222222222222222222222224924924924924924924924924924915556aaaaaaa80000000000000000000000000002492492222222222222222000000000000000000000000000000000000000000000000000000000000000022222222222222222222222224924924924924924924924924924915556aaaaaaa80000000000000000000000000003fffc000000000000000000000000000000");
FrameResult inspect(const std::vector<uint8_t>& v,ContentFormat f=ContentFormat::Unknown){return inspect_frame(v.data(),v.size(),uint32_t(v.size()*8),f);}
void field(std::vector<uint8_t>& v,unsigned offset,unsigned n,unsigned value){for(unsigned i=0;i<n;++i){const unsigned bit=offset+i;const uint8_t mask=uint8_t(0x80u>>(bit%8));if((value>>(n-i-1))&1)v[bit/8]|=mask;else v[bit/8]&=uint8_t(~mask);}}
std::vector<uint8_t> swap_words(std::vector<uint8_t> v){for(size_t i=0;i+1<v.size();i+=2)std::swap(v[i],v[i+1]);return v;}
void reference_checks(){
 check(ac3_reference.size()==128&&eac3_reference.size()==128&&dts_reference.size()==512,"Reference lengths");
 for(auto v:{ac3_reference,swap_words(ac3_reference)}){
  auto r=inspect(v,ContentFormat::Ac3);check(r.header==HeaderState::Ok&&r.codec==CodecKind::Ac3,"Independent AC3 header");
  check(r.frame_bytes==128&&r.consumed_bytes==128&&r.sample_rate==48000&&r.samples_per_frame==1536,"AC3 reference duration/extent");
  check(r.channels==2&&!r.lfe&&r.channel_mode==2&&r.bsid==8&&r.bsmod==0&&r.bit_rate==32000,"AC3 reference fields");
  check(r.structure==CheckState::Passed&&r.crc1==CheckState::Passed&&r.frame_crc==CheckState::Passed&&r.header_crc==CheckState::NotApplicable,"Independent AC3 CRC checks");
 }
 for(auto v:{eac3_reference,swap_words(eac3_reference)}){
  auto r=inspect(v,ContentFormat::Eac3);check(r.header==HeaderState::Ok&&r.codec==CodecKind::Eac3,"Independent EAC3 header");
  check(r.frame_bytes==128&&r.consumed_bytes==128&&r.sample_rate==48000&&r.samples_per_frame==1536,"EAC3 reference duration/extent");
  check(r.channels==2&&!r.lfe&&r.bsid==16&&r.stream_type==0&&r.substream_id==0&&r.bit_rate==32000,"EAC3 reference fields");
  check(r.frame_crc==CheckState::Passed&&r.crc1==CheckState::NotApplicable,"Independent EAC3 CRC check");
 }
 for(auto v:{dts_reference,swap_words(dts_reference)}){
  auto r=inspect(v,ContentFormat::Dts1);check(r.header==HeaderState::Ok&&r.codec==CodecKind::DtsCore,"Independent DTS header");
  check(r.frame_bytes==512&&r.consumed_bytes==512&&r.sample_rate==48000&&r.samples_per_frame==512,"DTS reference duration/extent");
  check(r.channels==2&&r.channel_mode==2&&r.bit_rate==384000&&r.structure==CheckState::Passed,"DTS reference fields");
  check(r.header_crc==CheckState::Absent&&r.frame_crc==CheckState::Unsupported,"DTS absent CRC never verified");
 }
 for(unsigned mutation:{20u,100u,127u}){
  auto v=ac3_reference;v[mutation]^=1;auto r=inspect(v);check(r.frame_crc==CheckState::Failed,"AC3 corruption caught");check(r.crc1==(mutation<80?CheckState::Failed:CheckState::Passed),"AC3 correct5/8 word boundary");
  v=eac3_reference;v[mutation]^=1;r=inspect(v);check(r.frame_crc==CheckState::Failed,"EAC3 corruption caught");
 }
 check(inspect(ac3_reference,ContentFormat::Eac3).transport_conflict,"AC3/EAC3 declaration contradiction");
 check(inspect(dts_reference,ContentFormat::Dts2).transport_conflict,"DTS duration declaration contradiction");
 auto unknown_mode=dts_reference;field(unknown_mode,60,6,63);check(inspect(unknown_mode,ContentFormat::Ac3).transport_conflict,"Unsupported layout still reports known codec family conflict");
 auto dts_prefix=dts_reference;dts_prefix.resize(4);check(!inspect(dts_prefix,ContentFormat::Dts2).transport_conflict,"Truncated unknown duration does not invent duration conflict");
 check(!inspect(ac3_reference).transport_conflict,"No declaration means no conflict");
}
void malformed_and_headers(){
 for(const auto& v:{ac3_reference,eac3_reference,dts_reference}){
  for(size_t n=0;n<v.size();++n){const auto r=inspect_frame(v.data(),n,uint32_t(n*8));check(r.structure!=CheckState::Passed&&r.frame_crc!=CheckState::Passed,"Every byte truncation rejects complete frame");check(r.consumed_bytes==0,"Truncated frame must not advance");}
  for(unsigned n=0;n<112;++n){const auto r=inspect_frame(v.data(),v.size(),n);check(r.frame_crc!=CheckState::Passed&&r.consumed_bytes==0,"Bit-prefix truncation remains bounded");}
 }
 auto v=ac3_reference;v.resize(80);auto r=inspect(v);check(r.header==HeaderState::Ok&&r.crc1==CheckState::Passed&&r.frame_crc==CheckState::Truncated,"Partial AC3 CRC1 independent of fullframe CRC");
 v=ac3_reference;field(v,32,2,3);check(inspect(v).header==HeaderState::Invalid,"AC3 reserved fscod");
 for(unsigned bsid:{9u,10u,17u,31u}){v=ac3_reference;field(v,40,5,bsid);r=inspect(v);check(r.header==HeaderState::Unsupported&&r.frame_crc!=CheckState::Passed,"Unsupported Dolby BSID not verified");}
 for(unsigned code:{0u,1u}){v=ac3_reference;field(v,32,2,1);field(v,34,6,code);r=inspect(v);check(r.frame_bytes==(code?140u:138u)&&r.sample_rate==44100,"Handchecked44.1k frame sizes");}
 v=ac3_reference;field(v,32,2,2);r=inspect(v);check(r.frame_bytes==192&&r.sample_rate==32000,"Handchecked32k frame size");
 for(unsigned mode=0;mode<8;++mode){v=ac3_reference;field(v,48,3,mode);unsigned pos=51;if((mode&1)&&mode!=1)pos+=2;if(mode&4)pos+=2;if(mode==2)pos+=2;field(v,pos,1,1);r=inspect(v);check(r.header==HeaderState::Ok&&r.lfe&&r.header_bits==pos+1,"All conditional AC3 modes/LFE");}
 v=eac3_reference;field(v,16,2,3);check(inspect(v).header==HeaderState::Invalid,"Reserved EAC3 stream type");
 for(unsigned code=0;code<4;++code){v=eac3_reference;field(v,34,2,code);r=inspect(v);const unsigned samples[]={256,512,768,1536};check(r.samples_per_frame==samples[code],"Handchecked EAC3 blocks");}
 v=eac3_reference;field(v,32,2,3);field(v,34,2,2);r=inspect(v);check(r.sample_rate==16000&&r.samples_per_frame==1536,"EAC3 reduced rate");
 v=eac3_reference;field(v,32,2,3);field(v,34,2,3);check(inspect(v).header==HeaderState::Invalid,"EAC3 reserved reduced rate");
 // Dependent stereo, substream3, custom Lrs/Rrs pair at field bit6 (0x0200).
 v=eac3_reference;field(v,16,2,1);field(v,18,3,3);field(v,45,5,0);field(v,50,1,0);field(v,51,1,1);field(v,52,16,0x0200);r=inspect(v);
 check(r.header==HeaderState::Ok&&r.stream_type==1&&r.substream_id==3&&r.channel_map==0x0200&&r.channels==2&&r.channel_configuration=="Lrs/Rrs (dependent substream)","Dependent EAC3 handcheckedmap");
 field(v,52,16,0x8000);check(inspect(v).header==HeaderState::Invalid,"Dependent map count contradiction");
 v=dts_reference;field(v,38,1,1); // CPF changes optionalheader presence, preserve following prefix theninsert16bits.
 // Handchecked complete header with CPF1, zero HCRC, version7; no payload CRC claim.
 v=hex("7ffe8001fe3c1ff0b5400000013800");v.resize(512);r=inspect(v);check(r.crc_present&&r.header_crc==CheckState::NotChecked,"DTS CPF1 CRC deliberately not tested");
 v=dts_reference;field(v,66,4,0);check(inspect(v).header==HeaderState::Invalid,"DTS reserved sample rate");
 v=dts_reference;field(v,60,6,63);r=inspect(v);check(r.header==HeaderState::Unsupported&&!r.channel_count_known,"DTS user-defined mode preserved");
 v=dts_reference;field(v,75,1,1);check(inspect(v).header==HeaderState::Invalid,"DTS reserved fixedbit");
 v=dts_reference;field(v,85,2,3);check(inspect(v).header==HeaderState::Invalid,"DTS reserved LFE");
 v=dts_reference;field(v,89,4,15);check(inspect(v).header==HeaderState::Unsupported,"DTS future revision not silently accepted");
 v=dts_reference;field(v,32,1,0);field(v,33,5,4);r=inspect(v);check(r.header==HeaderState::Ok&&r.termination_frame&&r.deficit_samples==5,"DTS termination metadata separate");
 std::array<uint8_t,4> junk{{0,1,2,3}};check(inspect_frame(junk.data(),4,32).header==HeaderState::Invalid,"No syncsearch in junk");
 check(inspect_frame(nullptr,1,8).header==HeaderState::Invalid,"Null pointer rejected");
 check(inspect_frame(nullptr,0,1).header==HeaderState::Invalid,"Invalid bitcount rejected");
 check(inspect_frame(junk.data(),max_payload_bytes+1,0).header==HeaderState::Invalid,"Size cap before reading");
 r=inspect_frame(ac3_reference.data(),ac3_reference.size(),1023);check(r.frame_crc==CheckState::Truncated&&!r.consumed_bytes,"One missing valid CRC bit cannot pass");
}
// This packing helper is test-only. Header fields/physical byte count below are
// also independently checked from literal14-bit sync and the3584/4096CD example.
std::vector<uint8_t> pack14(const std::vector<uint8_t>& v){std::vector<uint8_t> out;for(size_t bit=0;bit+14<=v.size()*8;bit+=14){unsigned word=0;for(unsigned i=0;i<14;++i)word=(word<<1)|((v[(bit+i)/8]>>(7-(bit+i)%8))&1);if(out.size()==2)word|=0xc000;out.push_back(uint8_t(word>>8));out.push_back(uint8_t(word));}return out;}
void packed_dts(){
 auto v=dts_reference;v.resize(3584);field(v,46,14,3583);auto p=pack14(v);check(p.size()==4096,"IndependentCD14bit physicalextent");
 check(p[0]==0x1f&&p[1]==0xff&&p[2]==0xe8&&p[3]==0&&p[4]==7&&(p[5]&0xf0)==0xf0,"Literal14bit sync pattern");
 for(auto bytes:{p,swap_words(p)}){auto r=inspect(bytes,ContentFormat::Dts1);check(r.header==HeaderState::Ok&&r.frame_bytes==3584&&r.consumed_bytes==4096&&r.sample_rate==48000&&r.channels==2,"DTS14 BE/LE metadata and independent extent");bytes.resize(4094);r=inspect(bytes);check(r.structure==CheckState::Truncated&&!r.consumed_bytes,"DTS14 truncated extent");}
 auto p2=pack14(dts_reference);auto r=inspect(p2);check(p2.size()==584&&r.consumed_bytes==584&&r.frame_bytes==512,"DTS14 nonintegral whole-word convention");
 // ETSI5.3.1 and the independent normalized DTS parser require all32 sync
 // bits. The four-byte physical marker spans only28 logical bits in14packing.
 auto damaged=p2;damaged[4]^=0x04;
 check(damaged[0]==0x1f&&damaged[1]==0xff&&damaged[2]==0xe8&&damaged[3]==0,"Damaged logical final sync nibble preserves physical marker");
 for(auto bytes:{damaged,swap_words(damaged)}){
  r=inspect(bytes);check(r.header==HeaderState::Invalid&&r.structure==CheckState::Failed&&!r.consumed_bytes,"DTS14 BE/LE normalized final sync nibble must be checked");
 }
 for(auto bytes:{p2,swap_words(p2)}){
  r=inspect_frame(bytes.data(),bytes.size(),32);check(r.header==HeaderState::Truncated&&!r.consumed_bytes,"Physical14bit marker without complete normalized sync remains truncated");
 }
}
void send(Inspector& inspector,const std::vector<uint8_t>& data,unsigned type=1,bool good=true,uint64_t start=1000,unsigned bits=0){
 if(!bits)bits=unsigned(data.size()*8);
 BurstEvent e;e.phase=BurstPhase::Begin;e.supported=true;e.info.start=start;e.info.end=start+99;e.info.pc=uint16_t(type);e.info.pd=uint16_t(type==21?bits/8:bits);e.declared_bits=bits;inspector.burst(e);
 for(unsigned bit=0;bit<bits;bit+=16){e.phase=BurstPhase::Word;e.index=bit/16;e.valid_bits=uint8_t(std::min(16u,bits-bit));e.word=uint16_t(data[bit/8])<<8;if(e.valid_bits>8)e.word|=data[bit/8+1];e.info.end+=100;inspector.burst(e);}
 e.phase=BurstPhase::End;e.info.complete=good;e.info.integrity=good;inspector.burst(e);
}
void streaming(){
 std::vector<CodecInfo> events;Inspector parser([&](const CodecInfo& e){events.push_back(e);});
 auto two=eac3_reference;two.insert(two.end(),eac3_reference.begin(),eac3_reference.end());send(parser,two,21);
 check(events.size()==2&&events[0].frame_index==0&&events[1].frame_index==1&&events[1].byte_offset==128,"Two EAC3 frames no syncsearch");
 check(events[0].frame.frame_crc==CheckState::Passed&&events[1].frame.frame_crc==CheckState::Passed,"Perframe CRC remains separate");
 check(events[0].start==1100&&events[0].end==7499&&events[1].start==7500&&events[1].end==13899&&events[0].source_interval_known,"Word coverage maps exact source intervals");
 check(events[0].transport.pc==21&&events[0].transport.complete&&events[0].transport.integrity,"Transport evidence retained");
 events.clear();auto truncated=ac3_reference;truncated.resize(100);send(parser,truncated,1,false);check(events.size()==1&&events[0].frame.structure==CheckState::Truncated&&events[0].frame.frame_crc==CheckState::Truncated,"Truncated burst emits incompletecodec");
 events.clear();send(parser,ac3_reference,21);check(events.size()==1&&events[0].frame.transport_conflict&&events[0].transport.pc==21,"Declared transport type never overwritten");
 events.clear();auto trailing=ac3_reference;trailing.push_back(0);trailing.push_back(0);send(parser,trailing);check(events.size()==2&&events[1].byte_offset==128&&events[1].frame.header!=HeaderState::Ok,"Trailing data explicit no silent discard");
 events.clear();BurstEvent e;e.supported=true;e.info.start=500;e.info.end=599;e.info.pc=1;e.info.pd=1024;e.declared_bits=1024;parser.burst(e);e.phase=BurstPhase::Word;e.index=0;e.valid_bits=16;e.word=0x0b77;e.info.end=699;parser.burst(e);check(events.empty(),"Live pending input not finalized");parser.finish();check(events.size()==1&&!events[0].transport.complete&&events[0].frame.header==HeaderState::Truncated,"Explicit EOF emits partialheader");parser.finish();check(events.size()==1,"finish idempotent");
 events.clear();Inspector malformed([&](const CodecInfo& info){events.push_back(info);});e.phase=BurstPhase::Begin;malformed.burst(e);e.phase=BurstPhase::Word;e.index=3;malformed.burst(e);e.phase=BurstPhase::End;malformed.burst(e);check(events.size()==1&&events[0].frame.structure==CheckState::Failed,"Malformed word index not verified");
}
void source_coverage(){
 // Word1 is interior to the complete frame: checking just the first/last
 // timestamps would hide its backwards interval while CRC correctly passes.
 for(unsigned scenario=0;scenario<5;++scenario){
  std::vector<CodecInfo> output;Inspector parser([&](const CodecInfo& c){output.push_back(c);});
  BurstEvent e;e.phase=BurstPhase::Begin;e.supported=true;e.info.start=1000;e.info.end=1099;e.info.pc=1;e.info.pd=1024;e.declared_bits=1024;
  if(scenario==3)e.info.end=std::numeric_limits<uint64_t>::max();
  parser.burst(e);
  for(size_t n=0;n<ac3_reference.size()/2;++n){
   e.phase=BurstPhase::Word;e.index=n;e.valid_bits=16;
   e.word=uint16_t(uint16_t(ac3_reference[n*2])<<8)|ac3_reference[n*2+1];
   e.info.end=1199+n*100;
   if(scenario==1&&n==1)e.info.end=1100;
   if(scenario==2&&n==1)e.info.end=1199;
   if(scenario==4&&n==1)e.info.end=std::numeric_limits<uint64_t>::max();
   parser.burst(e);
  }
  e.phase=BurstPhase::End;e.info.complete=e.info.integrity=true;parser.burst(e);
  check(output.size()==1,"Coverage fixture retains one codec frame");
  const auto& info=output.front();
  check(info.frame.structure==CheckState::Passed&&info.frame.frame_crc==CheckState::Passed,"Source mapping uncertainty does not falsify independent byte integrity");
  check(info.source_interval_known==(scenario==0),"Every covering word needs established monotonic source coverage");
  if(scenario==0)check(info.start==1100&&info.end==7499,"Valid contiguous source coverage is preserved");
  else check(info.start==0&&info.end==0,"Unknown coverage never fabricates frame endpoints");
 }
}
}
int main(){try{reference_checks();malformed_and_headers();packed_dts();streaming();source_coverage();std::cout<<"PASS "<<checks<<" codec checks\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}}
