#include "core/Decoder.h"
#include <iostream>
#include <iomanip>
#include <map>
#include <cstdlib>
// Input: one strictly increasing capture-sample edge index per line.
// Numeric timestamps avoid binary floating point precision loss in interchange.
int main(int argc,char**argv){
 if(argc!=2){std::cerr<<"Usage: decode_edges <capture-rate-Hz> < edge-indices.txt\n";return 2;}
 std::map<std::string,uint64_t> errors; uint64_t count=0,blocks=0,last=0;double sum=0;
 spdif::Sink sink;
 sink.subframe=[&](const spdif::Subframe& f){++count;sum+=f.sample_rate;};
 sink.content=[&](const spdif::ContentInfo& c){std::cout<<"content format="<<spdif::content_name(c.format)<<" complete="<<c.complete<<" integrity="<<c.integrity<<" "<<c.evidence<<'\n';};
 sink.status=[&](const spdif::StatusBlock& b){++blocks;std::cout<<"status channel="<<b.channel<<" integrity="<<b.integrity<<" "<<b.interpretation<<'\n';};
 sink.diagnostic=[&](const spdif::Diagnostic& d){++errors[d.code];if(errors[d.code]<=3)std::cerr<<d.code<<" at "<<d.start<<": "<<d.message<<'\n';};
 try {
 spdif::Decoder decoder({std::stod(argv[1]),0},sink);
 while(std::cin>>last)decoder.edge(last);
 if(!std::cin.eof())return 2;
 decoder.finish(last);
 std::cout<<"subframes="<<count<<" status_records="<<blocks<<" mean_transport_rate="<<std::setprecision(12)<<(count?sum/count:0)<<'\n';
 for(const auto& e:errors)std::cout<<"diagnostics "<<e.first<<'='<<e.second<<'\n';
 return count?0:1;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}
}
