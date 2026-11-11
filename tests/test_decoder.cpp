#include "Fixtures.h"
#include <iostream>
#include <stdexcept>
#include <string>
using namespace fixture;
static unsigned checks=0;
static void check(bool v,const std::string&m){++checks;if(!v)throw std::runtime_error(m);}
static void exact(const Signal&s,const Results&r,unsigned exclude_tail=2,bool diagnostics=true){
 check(r.frames.size()>=s.expected.size()-exclude_tail,"clean frame count "+std::to_string(r.frames.size())+" expected "+std::to_string(s.expected.size()-exclude_tail));
 for(const auto& f:r.frames){auto it=std::find_if(s.expected.begin(),s.expected.end(),[&](const Expected&e){return e.start==f.start;});check(it!=s.expected.end(),"invented frame start");check(it->raw==f.raw,"raw mismatch");check(it->preamble==f.preamble,"preamble mismatch");check(f.channel==(f.preamble=='W'?1u:0u),"channel");check(f.parity_ok,"clean parity");uint32_t p=f.raw&0xffffffu;int32_t sign=p&0x800000u?int32_t(p)-0x1000000:int32_t(p);check(f.sample==sign,"sign extension");check(f.validity==bool((f.raw>>24)&1),"V");check(f.user==bool((f.raw>>25)&1),"U");check(f.status==bool((f.raw>>26)&1),"C");check(f.parity==bool((f.raw>>27)&1),"P");check(std::abs(f.sample_rate-s.fs)<s.fs*.02,"measured rate");}
 if(diagnostics) for(const auto& d:r.diagnostics)check(d.code=="incomplete_status"||d.code=="partial_block"||d.code=="truncated_subframe"||d.code=="incomplete_block"||d.code=="truncated","clean diagnostic "+d.code);
}
static void literal_vectors(){
 // Hand-reviewed intervals in half cells: B=11101000 gives edges 0,3,4,5;
 // M=11100010 gives 0,3,6,7; W=11100100 gives 0,3,5,6.
 // Zero word has 28 zero bits and even parity: boundary edges at 8,10,...62.
 // Word 0x08000001 sets payload bit0 and parity bit27: mids at9 and63.
 Signal s(49152000,48000);s.edges.clear();s.expected.clear();
 const char pres[]={'B','W','M','W'};const uint32_t words[]={0,0x08000001u,0x08800000u,0x087fffffu};
 for(unsigned n=0;n<4;++n){uint64_t base=800+n*512;const unsigned b[]={0,3,4,5},m[]={0,3,6,7},w[]={0,3,5,6};const unsigned* p=pres[n]=='B'?b:pres[n]=='M'?m:w;for(unsigned i=0;i<4;++i)s.edges.push_back(base+8*p[i]);for(unsigned bit=0;bit<28;++bit){s.edges.push_back(base+8*(8+2*bit));if((words[n]>>bit)&1)s.edges.push_back(base+8*(9+2*bit));}s.expected.push_back({base,words[n],pres[n]});}
 s.edges.push_back(800+4*512);auto r=decode(s);exact(s,r,1);check(r.frames.size()>=3,"literal acquisition");
}
static void clean_rates(){for(double rate:{8000.,11025.,12000.,16000.,22050.,24000.,32000.,44100.,48000.,88200.,96000.,176400.,192000.})for(bool inv:{false,true})for(double oversample:{4.17,8.17}){Signal s(rate*128*oversample,rate,inv);s.frames(16);auto r=decode(s);exact(s,r);auto manual=decode(s,rate);exact(s,manual);}}
static void extrema_and_flags(){Signal s(100000000,48000);const uint32_t values[]={0,1,0xffffff,0x800000,0x7fffff,0xaaaaaa,0x555555};for(unsigned n=0;n<28;++n){s.subframe(n==0?'B':n%2?'W':'M',raw(values[n%7],n%3==0,n%3==1,n%3==2));}exact(s,decode(s));check(decode(s,96000).frames.empty(),"wrong manual rate rejects signal");}
static void jitter(){for(double rate:{8000.,48000.,192000.})for(double oversample:{8.,8.17})for(unsigned seed=1;seed<9;++seed){Signal s(rate*128*oversample,rate);s.jitter=.1;s.noise=seed;s.frames(40);exact(s,decode(s));}}
static void status(){std::array<uint8_t,24>a{},b{};a[0]=0x04;a[1]=0x12;a[2]=0x21;a[3]=0x02;a[4]=0x0b;b[0]=0x02;b[1]=0x34;b[3]=0x00;b[23]=0x80;Signal s(100000000,48000);s.frames(193,0,&a,&b);auto r=decode(s);check(r.blocks.size()==2,"two complete per-channel status blocks");for(auto&block:r.blocks){check(block.c==(block.channel?b:a),"channel status bytes/ordering");check(block.integrity,"clean status integrity");check(!block.interpretation.empty(),"status interpretation");}Signal truncated(100000000,48000);truncated.frames(192,0,&a,&b);check(decode(truncated).blocks.empty(),"unconfirmed final block must not be complete");a[0]=1;check(!spdif::interpret_status(a).empty(),"professional identified");a[0]=0xc0;check(!spdif::interpret_status(a).empty(),"unsupported mode identified");}
static void faults(){
 Signal s(100000000,48000);s.frames(210); // Toggle one payload bit but not parity, preserving structural timing.
 Signal bad(s.capture,s.fs);bad.subframe('B',raw(123)^0x08000000u);bad.subframe('W',raw(456));bad.frames(12,1);auto p=decode(bad);check(std::any_of(p.frames.begin(),p.frames.end(),[](const spdif::Subframe&f){return !f.parity_ok;}),"parity corruption visible");check(!p.diagnostics.empty(),"parity diagnostic");
 for(unsigned mutation=0;mutation<3;++mutation){auto damaged=s;size_t at=damaged.edges.size()/3;if(mutation==0)damaged.edges.erase(damaged.edges.begin()+at);else if(mutation==1)damaged.edges.insert(damaged.edges.begin()+at,damaged.edges[at-1]+1);else for(size_t i=at;i<at+12;++i)damaged.edges[i]+=i%2?2:0;auto r=decode(damaged);check(!r.diagnostics.empty(),"edge fault diagnosed");check(!r.frames.empty(),"fault recovery");for(const auto& f:r.frames){check(f.end>=f.start,"monotonic spans");check(std::any_of(s.expected.begin(),s.expected.end(),[&](const Expected&e){return e.start==f.start;}),"fault cannot invent subframe boundary");}check(r.frames.back().start>=s.expected[s.expected.size()-8].start,"reacquisition tail");}
 // Acquisition at every half-cell offset into an otherwise clean block.
 for(unsigned half=0;half<64;++half){uint64_t begin=s.expected[0].start+uint64_t(std::llround(half*s.capture/(128*s.fs)));auto r=decode(s,0,begin);check(r.frames.size()>s.expected.size()-10,"midstream lock within eight subframes");for(auto&f:r.frames)check(f.parity_ok,"midstream exact parity");}
 Signal order(s.capture,s.fs);order.subframe('B',raw(0));order.subframe('M',raw(1));order.frames(200,1);check(!decode(order).diagnostics.empty(),"channel order diagnosed");
 auto cut=decode(s,0,0,s.expected[10].start+30);check(cut.blocks.empty(),"truncation no full block");
 Signal idle(s.capture,s.fs);check(decode(idle).frames.empty(),"idle no invented samples");
}
static void recovery_and_blocks(){
 Signal first(100000000,48000);first.frames(12);Signal second(100000000,96000);second.frames(12);
 uint64_t shift=first.edges.back()+100000-second.edges.front();Signal combined=first;
 for(auto e:second.edges)combined.edges.push_back(e+shift);
 auto r=decode(combined);check(r.frames.size()>30,"gap/rate change decodes both segments");
 check(std::abs(r.frames.back().sample_rate-96000)<1000,"new rate recovered");
 auto newstart=std::find_if(r.frames.begin(),r.frames.end(),[&](const spdif::Subframe&f){return f.start>=second.edges.front()+shift;});
 check(newstart!=r.frames.end()&&newstart->start<=second.expected[7].start+shift,"gap/rate acquisition within eight");
 check(!r.diagnostics.empty(),"gap diagnosed");
 Signal damaged(100000000,48000);for(unsigned i=0;i<193;++i)for(unsigned ch=0;ch<2;++ch){uint32_t x=raw(i,false,false,i%7==0);if(i==7&&ch==1)x^=1u<<27;damaged.subframe(ch?'W':i%192?'M':'B',x);}
 auto blocks=decode(damaged).blocks;check(blocks.size()==2,"parity damaged block retained");check(std::any_of(blocks.begin(),blocks.end(),[](const spdif::StatusBlock&b){return !b.integrity;}),"status parity integrity uncertain");
 Signal early(100000000,48000);for(unsigned i=0;i<30;++i){early.subframe(i%10==0?'B':'M',raw(i));early.subframe('W',raw(i));}auto er=decode(early);check(er.blocks.empty(),"early B cannot create complete block");check(!er.diagnostics.empty(),"early B diagnosed");
 Signal late(100000000,48000);for(unsigned i=0;i<200;++i){late.subframe(i==0||i==199?'B':'M',raw(i));late.subframe('W',raw(i));}auto lr=decode(late);check(lr.blocks.empty(),"late B cannot create complete block");check(!lr.diagnostics.empty(),"late B diagnosed");
 Signal drifting(100000000,48000);drifting.frames(100);double last=double(drifting.edges.back());for(auto&e:drifting.edges)e+=uint64_t(.001*double(e)*double(e)/last);auto dr=decode(drifting);check(dr.frames.size()>=drifting.expected.size()-2,"slow drift retains frames");for(auto&f:dr.frames)check(f.parity_ok,"slow drift parity");
}
static void interpretation(){std::array<uint8_t,24>b{};for(unsigned maximum:{0u,1u})for(unsigned code:{1u,2u,4u,5u,6u}){b[4]=uint8_t(maximum|(code<<1));unsigned m=maximum?24:20;unsigned bits=code==1?m-4:code==2?m-2:code==4?m-1:code==5?m:m-3;check(spdif::interpret_status(b).find("word_length="+std::to_string(bits))!=std::string::npos,"declared word length");}b[0]=1;check(spdif::interpret_status(b).find("professional")!=std::string::npos,"professional label");b[0]=2;check(spdif::interpret_status(b).find("non-audio")!=std::string::npos,"non-audio label");b[0]=0;b[3]=15;check(spdif::interpret_status(b).find("unknown(15)")!=std::string::npos,"unknown frequency retained");}
static void random_faults(){std::mt19937 rng(0x958);Signal s(100000000,48000);s.frames(30);for(unsigned n=0;n<50;++n){auto bad=s;size_t k=1+rng()%(bad.edges.size()-2);bad.edges.erase(bad.edges.begin()+k);auto a=decode(bad),b=decode(bad);check(a.frames.size()==b.frames.size(),"fault determinism count");check(a.diagnostics.size()==b.diagnostics.size(),"diagnostic determinism");for(size_t i=0;i<a.frames.size();++i)check(a.frames[i].raw==b.frames[i].raw&&a.frames[i].start==b.frames[i].start,"deterministic event");}}
int main(){try{literal_vectors();clean_rates();extrema_and_flags();jitter();status();faults();recovery_and_blocks();interpretation();random_faults();std::cout<<"PASS "<<checks<<" checks\n";return 0;}catch(const std::exception&e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<"\n";return 1;}}
