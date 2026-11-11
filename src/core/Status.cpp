#include "Decoder.h"
#include <sstream>
namespace spdif {
namespace {
const char* frequency(unsigned code) {
 switch(code){case 0:return "44100";case 1:return "not indicated";case 2:return "48000";case 3:return "32000";case 4:return "22050";case 6:return "24000";case 8:return "88200";case 9:return "768000";case 10:return "96000";case 12:return "176400";case 14:return "192000";default:return nullptr;}
}
const char* original(unsigned code) {
 switch(code){case 0:return "not indicated";case 1:return "192000";case 2:return "12000";case 3:return "176400";case 5:return "96000";case 6:return "8000";case 7:return "88200";case 8:return "16000";case 9:return "24000";case 10:return "11025";case 11:return "22050";case 12:return "32000";case 13:return "48000";case 15:return "44100";default:return nullptr;}
}
}
std::string interpret_status(const std::array<uint8_t,24>& b) {
 std::ostringstream s;
 if(b[0]&1){s<<"professional; "<<((b[0]&2)?"non-audio":"audio")<<"; remaining professional fields unsupported";return s.str();}
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
