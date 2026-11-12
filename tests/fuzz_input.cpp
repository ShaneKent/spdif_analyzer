#include "input/EdgeInput.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
namespace {
void require(bool value){if(!value)std::abort();}
void check(std::string_view text){
 bool valid=!text.empty();uint64_t expected=0;
 for(unsigned char c:text){if(c<'0'||c>'9'){valid=false;break;}const unsigned digit=c-'0';if(expected>(UINT64_MAX-digit)/10){valid=false;break;}expected=expected*10+digit;}
 try{const auto actual=spdif::input::unsigned_integer(text);require(valid&&actual==expected);}catch(const std::invalid_argument&){require(!valid);}
}
}
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data,size_t size){
 if(size>4096)return 0;
 const std::string text(reinterpret_cast<const char*>(data),size);check(text);
 // Caller whitespace tokenization is deliberately independent of strict token
 // parsing; binary NUL/non-ASCII bytes remain inside a token and must fail.
 std::istringstream input(text);input.imbue(std::locale::classic());std::string token;
 while(input>>token)check(token);
 uint64_t number=0;for(size_t i=0;i<std::min<size_t>(size,8);++i)number|=uint64_t(data[i])<<(i*8);
 const auto canonical=std::to_string(number);require(spdif::input::unsigned_integer(canonical)==number);check(canonical);
 check("18446744073709551615");check("18446744073709551616");check("-1");check("+1");check(" 1");check("1 ");
 return 0;
}
