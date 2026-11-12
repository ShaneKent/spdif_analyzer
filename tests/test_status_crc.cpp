// Primary reference AES3-1992(r1997), AnnexB examples1/2 and section5.2.1.
// CRC expectations below are literal published bits, not our encoder's output.
#include "Fixtures.h"
#include "analysis/Analysis.h"
#include "export/Exports.h"
#include "report/Events.h"
#include <filesystem>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace spdif;
namespace {
unsigned checks=0;
void check(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
std::array<uint8_t,24> good(){std::array<uint8_t,24> b{};b[0]=0x85;b[1]=2;b[2]=0x2c;b[23]=0x6d;return b;}
}
int main(){try{
 std::array<uint8_t,24> first{};first[0]=0x3d;first[1]=2;first[4]=2;first[23]=0x9b;
 check(professional_crc(first)==0x9b&&status_crc(first)==IntegrityCheck::Passed,"AnnexB example1 literal0x9B");
 std::array<uint8_t,24> second{};second[0]=1;second[23]=0x32;
 check(professional_crc(second)==0x32&&status_crc(second)==IntegrityCheck::Passed,"AnnexB example2 literal0x32");
 for(unsigned bit=1;bit<192;++bit){auto damaged=first;damaged[bit/8]^=uint8_t(1u<<(bit%8));check(status_crc(damaged)==IntegrityCheck::Failed,"Detect each single-bit corruption while professional flag remains set");}
 auto minimum=second;minimum[23]=0;
 check(status_crc(minimum)==IntegrityCheck::Absent,"AES3minimumdefaultzeroCRCC is absent, not a false pass");
 auto unverified=good();unverified[23]=0;
 check(status_crc(unverified)==IntegrityCheck::NotChecked&&status_info(unverified).layout.known,"ExtendedminimumzeroCRCC explicitly unverified, preserves compatibility");
 auto zero=good();zero[3]=0xa5;zero[23]=0;
 check(professional_crc(zero)==0&&status_crc(zero)==IntegrityCheck::Passed,"A valid zero CRC must still pass");
 auto consumer=first;consumer[0]&=0xfe;
 check(status_crc(consumer)==IntegrityCheck::NotApplicable,"Never applyprofessionalCRCtoconsumerbits");
 auto bad=good();bad[23]^=1;
 check(!status_info(bad).supported&&!status_info(bad).layout.known,"BadCRC invalidates status-derived layout");
 check(status_info(bad).word_length==24&&status_info(bad).sample_rate==48000,"Untrusted raw declarations retained");
 fixture::Signal signal(100000000,48000);const auto valid=good();
 for(unsigned frame=0;frame<800;++frame)for(unsigned ch=0;ch<2;++ch){const auto& bytes=(frame>=192&&frame<384&&ch==0)?bad:valid;const unsigned pos=frame%192;
  signal.subframe(ch?'W':pos?'M':'B',fixture::raw(frame,false,false,(bytes[pos/8]>>(pos%8))&1));}
 const auto decoded=fixture::decode(signal);
 check(decoded.frames.at(383*2).content==ContentFormat::Pcm,"Previous confirmed evidence retained until bad CRCC proven");
 check(decoded.frames.at(384*2).content==ContentFormat::Unknown&&!decoded.frames.at(384*2).layout.known,"BadCRCconfirmationclearsPCM");
 check(decoded.frames.at(576*2).content==ContentFormat::Pcm,"FreshverifiedblockrestoresPCM");
 check(std::count_if(decoded.diagnostics.begin(),decoded.diagnostics.end(),[](const auto& d){return d.code=="status_crc";})==1,"Onechannel-specificCRCdiagnostic");
 const auto block=std::find_if(decoded.blocks.begin(),decoded.blocks.end(),[](const auto& b){return b.crc==IntegrityCheck::Failed;});
 check(block!=decoded.blocks.end()&&block->integrity,"BadCRCdoesnotrewriteparityintegrity");
 check(json::record(*block).find("\"status_crc\":\"failed\"")!=std::string::npos,"LayeredCRCJSONrecord");
 Analysis analysis({100000000,1,0,0},{});for(const auto& b:decoded.blocks)analysis.add(b);analysis.finish(signal.edges.back()+1);const auto health=analysis.summary();
 check(health.channels[0].status_crc_failed==1&&health.channels[1].status_crc_failed==0,"CRCfaultattributedtotheactualchannel");
 check(health.channels[0].parity_failures==0&&health.channels[0].status_damaged==0,"CRChealthseparatefromparity");
 check(health.channels[0].has_fault&&!health.channels[1].has_fault,"Nofalsefaultontheotherchannel");
 // A rate/precision override cannot turn a proven bad status CRC into audio.
 const auto dir=std::filesystem::temp_directory_path()/("spdif-status-crc-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 {exports::WavWriter writer(dir,{100000000,24,48000,true});for(unsigned ch=0;ch<2;++ch){StatusBlock b;b.channel=ch;b.c=bad;writer.status(b);}
  for(unsigned ch=0;ch<2;++ch){StatusBlock damaged;damaged.channel=ch;damaged.c[0]=4;damaged.integrity=false;writer.status(damaged);}
  for(unsigned i=0;i<2;++i){Subframe f;f.start=100+i*100;f.end=199+i*100;f.channel=i;f.preamble=i?'W':'B';f.content=ContentFormat::Pcm;f.parity_ok=true;f.layout={24,true,true,true};writer.subframe(f);}writer.finish(300);}
 check(std::none_of(std::filesystem::directory_iterator(dir),std::filesystem::directory_iterator{},[](const auto& f){return f.path().extension()==".wav";}),"OverridecannotbypassbadstatusCRC");
 std::filesystem::remove_all(dir);
 std::cout<<"PASS "<<checks<<" status CRC checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
