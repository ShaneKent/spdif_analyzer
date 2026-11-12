#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <vector>
namespace spdif::json {
inline std::string quote(const std::string& s){
 static const char h[]="0123456789abcdef";std::string out="\"";
 for(unsigned char c:s){switch(c){case '"':out+="\\\"";break;case '\\':out+="\\\\";break;case '\b':out+="\\b";break;case '\f':out+="\\f";break;case '\n':out+="\\n";break;case '\r':out+="\\r";break;case '\t':out+="\\t";break;default:if(c<32){out+="\\u00";out+=h[c>>4];out+=h[c&15];}else out+=char(c);}}
 return out+'"';
}
inline std::string array(const std::vector<std::string>& values){std::string out="[";for(size_t i=0;i<values.size();++i){if(i)out+=',';out+=values[i];}return out+']';}
class Object {
 std::string body;
public:
 Object& raw(const std::string& key,const std::string& value){if(!body.empty())body+=',';body+=quote(key)+':'+value;return *this;}
 Object& text(const std::string& key,const std::string& value){return raw(key,quote(value));}
 Object& boolean(const std::string& key,bool value){return raw(key,value?"true":"false");}
 Object& integer(const std::string& key,uint64_t value){return raw(key,std::to_string(value));}
 Object& signed_integer(const std::string& key,int64_t value){return raw(key,std::to_string(value));}
 // Sample indices are decimal strings: JSON number consumers may lose >53 bits.
 Object& sample(const std::string& key,uint64_t value){return text(key,std::to_string(value));}
 Object& number(const std::string& key,double value){if(!std::isfinite(value))return raw(key,"null");std::ostringstream s;s.imbue(std::locale::classic());s<<std::setprecision(std::numeric_limits<double>::max_digits10)<<value;return raw(key,s.str());}
 std::string str()const{return '{'+body+'}';}
};
inline Object event(const char* type){Object o;o.integer("schema_version",1).text("event",type);return o;}
template<size_t N>std::string hex(const std::array<uint8_t,N>& bytes){static const char h[]="0123456789abcdef";std::string out;out.reserve(N*2);for(auto b:bytes){out+=h[b>>4];out+=h[b&15];}return out;}
}
