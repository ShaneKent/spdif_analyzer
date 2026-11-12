#include "Decoder.h"
#include <sstream>
#include <stdexcept>
namespace spdif {
namespace {
const char* frequency(unsigned code) {
 switch(code){case 0:return "44100";case 1:return "not indicated";case 2:return "48000";case 3:return "32000";case 4:return "22050";case 6:return "24000";case 8:return "88200";case 9:return "768000";case 10:return "96000";case 12:return "176400";case 14:return "192000";default:return nullptr;}
}
const char* original(unsigned code) {
 switch(code){case 0:return "not indicated";case 1:return "192000";case 2:return "12000";case 3:return "176400";case 5:return "96000";case 6:return "8000";case 7:return "88200";case 8:return "16000";case 9:return "24000";case 10:return "11025";case 11:return "22050";case 12:return "32000";case 13:return "48000";case 15:return "44100";default:return nullptr;}
}
}
const char* integrity_name(IntegrityCheck state) {
 switch(state){case IntegrityCheck::NotApplicable:return "not_applicable";case IntegrityCheck::NotChecked:return "not_checked";case IntegrityCheck::Absent:return "absent";case IntegrityCheck::Unsupported:return "unsupported";case IntegrityCheck::Truncated:return "truncated";case IntegrityCheck::Failed:return "failed";case IntegrityCheck::Passed:return "passed";}return "unsupported";
}
uint8_t professional_crc(const std::array<uint8_t,24>& bytes) {
 // AES3 Annex B: bytes0..22, LSB transmitted first, all stages initially1.
 // Reflected x^8+x^4+x^3+x^2+1; both published examples independently tested.
 uint8_t crc=0xff;
 for(size_t i=0;i<23;++i){crc^=bytes[i];for(unsigned bit=0;bit<8;++bit)crc=uint8_t((crc>>1)^((crc&1)?0xb8:0));}
 return crc;
}
IntegrityCheck status_crc(const std::array<uint8_t,24>& bytes) {
 if(!(bytes[0]&1))return IntegrityCheck::NotApplicable;
 if(bytes[23]==professional_crc(bytes))return IntegrityCheck::Passed;
 // Section5.2.1 permits minimum implementations without a generated CRCC,
 // including additional partially implemented fields. Zero alone cannot prove
 // corruption or distinguish an unimplemented CRC from a corrupted zero.
 if(bytes[23]==0){bool minimum=bytes[0]==1;for(size_t i=1;i<23;++i)minimum&=bytes[i]==0;return minimum?IntegrityCheck::Absent:IntegrityCheck::NotChecked;}
 return IntegrityCheck::Failed;
}
StatusInfo status_info(const std::array<uint8_t,24>& b) {
 StatusInfo info;info.professional=(b[0]&1)!=0;info.non_audio=(b[0]&2)!=0;
 unsigned maximum=0,code=0;
 if(info.professional){
  info.expected_crc=professional_crc(b);info.crc=status_crc(b);
  info.channel_mode=b[1]&15;info.audio_fields_reliable=(b[22]&0x10)==0;
  info.supported=(info.channel_mode==2 || info.channel_mode==8)&&info.audio_fields_reliable&&info.crc!=IntegrityCheck::Failed;
  info.stereo=info.channel_mode==2;info.emphasis_code=(b[0]>>2)&7;
  const unsigned rates[]={0,44100,48000,32000};info.sample_rate=rates[b[0]>>6];
  switch(b[2]&7){case 2:maximum=20;break;case 4:maximum=24;break;default:break;}
  code=(b[2]>>3)&7;
  if(maximum)switch(code){case 2:info.word_length=maximum-2;break;case 4:info.word_length=maximum-1;break;case 5:info.word_length=maximum;break;case 6:info.word_length=maximum-4;break;default:break;}
 }else{
  info.channel_mode=b[0]>>6;info.supported=info.channel_mode==0;info.stereo=true;
  info.emphasis_code=(b[0]>>3)&7;
  const unsigned rates[]={44100,0,48000,32000,22050,0,24000,0,88200,768000,96000,0,176400,0,192000,0};
  info.sample_rate=(b[3]&128)?0:rates[b[3]&15];maximum=(b[4]&1)?24:20;code=(b[4]>>1)&7;
  switch(code){case 1:info.word_length=maximum-4;break;case 2:info.word_length=maximum-2;break;case 4:info.word_length=maximum-1;break;case 5:info.word_length=maximum;break;case 6:info.word_length=maximum-3;break;default:break;}
 }
 info.layout.professional=info.professional;info.layout.stereo=info.stereo;
 info.layout.known=info.supported&&!info.non_audio&&info.word_length!=0;
 info.layout.bits=info.layout.known?info.word_length:0;
 return info;
}
int32_t pcm_value(uint32_t raw,const SampleLayout& layout) {
 if(!layout.known || layout.bits==0 || layout.bits>24)throw std::invalid_argument("PCM sample layout is unknown or outside 1..24 bits");
 const uint32_t mask=(uint32_t(1)<<layout.bits)-1;
 const uint32_t value=(raw>>(24-layout.bits))&mask;
 return (value&(uint32_t(1)<<(layout.bits-1)))?int32_t(value)-int32_t(uint32_t(1)<<layout.bits):int32_t(value);
}
std::string interpret_status(const std::array<uint8_t,24>& b) {
 std::ostringstream s;
 if(b[0]&1){
  const auto info=status_info(b);s<<"professional; "<<(info.non_audio?"non-audio":"audio");
  s<<"; emphasis=";switch(info.emphasis_code){case 0:s<<"not indicated";break;case 1:s<<"none";break;case 3:s<<"50/15 us";break;case 7:s<<"CCITT J.17";break;default:s<<"unknown("<<info.emphasis_code<<")";}
  s<<"; source_frequency_unlocked="<<unsigned((b[0]>>5)&1)<<"; nominal_rate=";
  if(info.sample_rate)s<<info.sample_rate;else s<<"not indicated";
  s<<"; channel_mode=";switch(info.channel_mode){case 0:s<<"not indicated";break;case 2:s<<"stereophonic (A left / B right)";break;case 4:s<<"single channel (unsupported)";break;case 8:s<<"two independent channels A/B";break;case 12:s<<"primary/secondary (unsupported)";break;case 15:s<<"byte-3 extension (unsupported)";break;default:s<<"unknown("<<info.channel_mode<<")";}
  s<<"; sample_coordination="<<unsigned(b[2]&7)<<"; word_length=";
  if(info.word_length)s<<info.word_length;else s<<"unknown (coordination="<<unsigned(b[2]&7)<<", code="<<unsigned((b[2]>>3)&7)<<")";
  s<<"; audio_fields_reliable="<<(info.audio_fields_reliable?"true":"false (bytes 0..5 declared unreliable)");
  s<<"; status_crc="<<integrity_name(info.crc)<<"; status_crc_received="<<unsigned(b[23])<<"; status_crc_expected="<<unsigned(info.expected_crc);
  if(info.crc==IntegrityCheck::NotChecked||info.crc==IntegrityCheck::Absent)s<<" (zero CRCC: minimum/unimplemented or unverified, not a passed check)";
  return s.str();
 }
 s<<"consumer; ";
 unsigned mode=b[0]>>6;
 if(mode){s<<"unsupported mode="<<mode;return s.str();}
 s<<((b[0]&2)?"non-audio":"PCM audio")<<"; copyright="<<((b[0]&4)?"not asserted":"asserted");
 unsigned emphasis=(b[0]>>3)&7;
 s<<"; emphasis=";if(emphasis==0)s<<"none";else if(emphasis==1)s<<"50/15 us";else s<<"unknown("<<emphasis<<")";
 s<<"; category="<<unsigned(b[1]&0x7f)<<"; category_L="<<unsigned(b[1]>>7);
 s<<"; source="<<unsigned(b[2]&15)<<"; channel="<<unsigned(b[2]>>4);
 s<<"; nominal_rate=";auto f=frequency(b[3]&15);if(f)s<<f;else s<<"unknown("<<unsigned(b[3]&15)<<")";
 unsigned accuracy=(b[3]>>4)&3;
 s<<"; clock_accuracy=";switch(accuracy){case 0:s<<"level II (1000 ppm)";break;case 1:s<<"level I (50 ppm)";break;case 2:s<<"level III (variable pitch)";break;default:s<<"unknown(3)";}
 s<<"; word_length=";
 unsigned code=(b[4]>>1)&7;unsigned maximum=(b[4]&1)?24:20;
 switch(code){case 0:s<<"not indicated";break;case 1:s<<maximum-4;break;case 2:s<<maximum-2;break;case 4:s<<maximum-1;break;case 5:s<<maximum;break;case 6:s<<maximum-3;break;default:s<<"unknown("<<code<<")";}
 s<<"; original_rate=";f=original(b[4]>>4);if(f)s<<f;else s<<"unknown("<<unsigned(b[4]>>4)<<")";
 return s.str();
}
}
