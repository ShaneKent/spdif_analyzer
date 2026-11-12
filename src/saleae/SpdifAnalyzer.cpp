#include "SpdifAnalyzer.h"
#include <AnalyzerChannelData.h>
#include <cmath>
#include <algorithm>

SpdifAnalyzer::SpdifAnalyzer(){ SetAnalyzerSettings(&settings); UseFrameV2(); }
SpdifAnalyzer::~SpdifAnalyzer(){ KillThread(); }
void SpdifAnalyzer::SetupResults(){ results.reset(new SpdifResults(this,&settings)); SetAnalyzerResults(results.get()); results->AddChannelBubblesWillAppearOn(settings.channel); }
void SpdifAnalyzer::WorkerThread(){
 auto* input=GetAnalyzerChannelData(settings.channel);
 SpdifAnalysisBatch batch;
 spdif::AnalysisSink analysis_sink;
 analysis_sink.health=[&](const spdif::HealthWindow& h){batch.health.push_back(h);};
 analysis_sink.status_change=[&](const spdif::StatusChange& c){batch.changes.push_back(c);};
 analysis_sink.pcm=[&](const spdif::PcmWindow& p){batch.pcm.push_back(p);};
 spdif::Analysis analysis({double(GetSampleRate()),settings.window_ms/1000.0,0,settings.pcm_bits},analysis_sink);
 const auto publish=[&]{results->PublishAnalysis(batch,analysis.summary(),analysis.current_pcm());};
 spdif::codec::Inspector codec([&](const spdif::codec::CodecInfo& c){results->Add(c);});
 spdif::Sink sink;
 sink.subframe=[&](const spdif::Subframe& f){results->Add(f);analysis.add(f);publish();};
 sink.status=[&](const spdif::StatusBlock& b){results->Add(b);analysis.add(b);publish();};
 sink.content=[&](const spdif::ContentInfo& c){results->Add(c);analysis.add(c);publish();};
 sink.diagnostic=[&](const spdif::Diagnostic& d){results->Add(d);analysis.add(d);publish();};
 sink.burst=[&](const spdif::BurstEvent& b){results->Add(b);codec.burst(b);};
 if(settings.nominal && GetSampleRate()<settings.nominal*512u)
  sink.diagnostic(spdif::Diagnostic{0,0,"capture_rate_low","Capture rate is below four samples per half-cell for the manual nominal rate; increase the capture rate."});
 results->PublishSnapshot(input->GetSampleNumber());results->CommitResults();
 spdif::Decoder decoder({double(GetSampleRate()),double(settings.nominal)},std::move(sink));
 U64 edges=0;
 for(;;){
  CheckIfThreadShouldExit();
  // Commit a short tail before an edge wait, including temporary live idle.
  // Do not terminate the decoder when the currently buffered data is empty.
  if(!input->DoMoreTransitionsExistInCurrentData()){
   results->PublishSnapshot(input->GetSampleNumber(),decoder.pending_header());results->CommitResults();ReportProgress(input->GetSampleNumber());
  }
  input->AdvanceToNextEdge(); decoder.edge(input->GetSampleNumber());
  if((++edges&1023)==0){ results->PublishSnapshot(input->GetSampleNumber(),decoder.pending_header());results->CommitResults(); ReportProgress(input->GetSampleNumber()); }
 }
}
U32 SpdifAnalyzer::GetMinimumSampleRateHz(){ return std::max<U32>(50000000, (settings.nominal?settings.nominal:48000)*1024); }
const char* SpdifAnalyzer::GetAnalyzerName() const { return "S/PDIF Analyzer"; }
bool SpdifAnalyzer::NeedsRerun(){ return false; }
void SpdifAnalyzer::SimLevel(bool level){
 simulation.TransitionIfNeeded(level?BIT_HIGH:BIT_LOW);
 sim_time+=sim_half;
 const U64 target=static_cast<U64>(std::llround(sim_time));
 const U64 current=simulation.GetCurrentSampleNumber();
 if(target>current) simulation.Advance(static_cast<U32>(target-current));
}
void SpdifAnalyzer::SimSubframe(unsigned channel){
 const unsigned pos=unsigned(sim_frame%192);
 const uint8_t pre=channel?0xe4:(pos==0?0xe8:0xe2);
 const bool invert=simulation.GetCurrentBitState()==BIT_HIGH;
 for(int i=7;i>=0;--i) SimLevel(bool((pre>>i)&1)^invert);
 // Independent deterministic stereo ramp, left positive and right negative.
 const int32_t sample=channel?-int32_t((sim_frame*257)&0x7fffff):int32_t((sim_frame*257)&0x7fffff);
 uint32_t raw=uint32_t(sample)&0xffffff;
 // Consumer mode 0: 48 kHz (or selected declared rate), 24-bit word length.
 std::array<uint8_t,24> c{}; c[0]=0x04; c[3]=2; c[4]=0x0b;
 const U32 nominal=settings.nominal?settings.nominal:48000;
 switch(nominal){case 44100:c[3]=0;break;case 32000:c[3]=3;break;case 88200:c[3]=8;break;case 96000:c[3]=10;break;case 176400:c[3]=12;break;case 192000:c[3]=14;break;case 48000:break;default:c[3]=1;break;}
 if((c[pos/8]>>(pos%8))&1) raw|=1u<<26;
 unsigned ones=0; for(unsigned i=0;i<27;++i) ones+=(raw>>i)&1;
 raw|=(ones&1)<<27;
 bool level=simulation.GetCurrentBitState()==BIT_HIGH;
 for(unsigned i=0;i<28;++i){ level=!level; SimLevel(level); if((raw>>i)&1)level=!level; SimLevel(level); }
}
U32 SpdifAnalyzer::GenerateSimulationData(U64 requested,U32 rate,SimulationChannelDescriptor** channels){
 if(!simulation_initialized || simulation_nominal!=settings.nominal || simulation_channel!=settings.channel || simulation_rate!=GetSimulationSampleRate()){
  simulation=SimulationChannelDescriptor(); sim_frame=0; simulation_nominal=settings.nominal; simulation_channel=settings.channel;
  simulation_rate=GetSimulationSampleRate(); simulation.SetChannel(settings.channel); simulation.SetSampleRate(simulation_rate); simulation.SetInitialBitState(BIT_LOW);
  simulation.Advance(100); sim_time=100; sim_half=double(simulation_rate)/(128.0*(settings.nominal?settings.nominal:48000)); simulation_initialized=true;
 }
 const U64 target=AnalyzerHelpers::AdjustSimulationTargetSample(requested,rate,simulation_rate);
 while(simulation.GetCurrentSampleNumber()<target){ SimSubframe(0); SimSubframe(1); ++sim_frame; }
 *channels=&simulation; return 1;
}
const char* __cdecl GetAnalyzerName(){ return "S/PDIF Analyzer"; }
Analyzer* __cdecl CreateAnalyzer(){ return new SpdifAnalyzer; }
void __cdecl DestroyAnalyzer(Analyzer* a){ delete a; }
