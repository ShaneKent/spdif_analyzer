// QA-only waveform source. This module intentionally has no production decoder
// or encoder dependency: all preamble levels, BMC transitions and parity here
// are independent test data. It is not part of the release package.
#include <Analyzer.h>
#include <AnalyzerChannelData.h>
#include <AnalyzerHelpers.h>
#include <AnalyzerResults.h>
#include <array>
#include <algorithm>
#include <limits>
#include <cmath>
#include <memory>
namespace test_signal {
class Settings final : public AnalyzerSettings {
public:
 Channel channel=UNDEFINED_CHANNEL;U32 pattern=0;
 Settings(){
  input.SetTitleAndTooltip("Digital channel","QA-only simulated S/PDIF output channel.");input.SetSelectionOfNoneIsAllowed(false);
  choices.SetTitleAndTooltip("Pattern","Independent synthetic transport fixture; compressed payloads are not codec-valid audio.");
  const char* names[]={"PCM ramp","AC3 bursts","DTS I bursts","Status changes","Professional A/B PCM","Partial IEC header"};
  for(unsigned i=0;i<6;++i)choices.AddNumber(i,names[i],names[i]);
  AddInterface(&input);AddInterface(&choices);AddChannel(channel,"Test signal",false);update();
 }
 bool SetSettingsFromInterfaces() override {
  const auto next=input.GetChannel();const double selected=choices.GetNumber();
  if(next==UNDEFINED_CHANNEL||next.mDataType!=DIGITAL_CHANNEL||selected<0||selected>5||std::floor(selected)!=selected){SetErrorText("Select a digital channel and fixture pattern.");return false;}
  channel=next;pattern=static_cast<U32>(selected);ClearChannels();AddChannel(channel,"Test signal",true);return true;
 }
 void LoadSettings(const char* text) override {SimpleArchive a;a.SetString(text);U32 version=0,p=0;Channel c=UNDEFINED_CHANNEL;
  if((a>>version)&&version==1&&(a>>c)&&(a>>p)&&p<6&&c.mDataType==DIGITAL_CHANNEL){channel=c;pattern=p;}else{channel=UNDEFINED_CHANNEL;pattern=0;}
  ClearChannels();AddChannel(channel,"Test signal",channel!=UNDEFINED_CHANNEL);update();
 }
 const char* SaveSettings() override {SimpleArchive a;a<<U32(1);a<<channel;a<<pattern;return SetReturnString(a.GetString());}
private:
 AnalyzerSettingInterfaceChannel input;AnalyzerSettingInterfaceNumberList choices;
 void update(){input.SetChannel(channel);choices.SetNumber(pattern);}
};
class Results final : public AnalyzerResults {
public:
 void GenerateBubbleText(U64,Channel&,DisplayBase) override {ClearResultStrings();}
 void GenerateExportFile(const char*,DisplayBase,U32) override {}
 void GenerateFrameTabularText(U64,DisplayBase) override {ClearTabularText();}
 void GeneratePacketTabularText(U64,DisplayBase) override {ClearTabularText();}
 void GenerateTransactionTabularText(U64,DisplayBase) override {ClearTabularText();}
};
class Signal final : public Analyzer2 {
 Settings settings;std::unique_ptr<Results> results;
 SimulationChannelDescriptor output;
 bool held=false,initialized=false;Channel old_channel=UNDEFINED_CHANNEL;U32 old_pattern=0,rate=0;
 uint64_t frame=0;double clock_position=100,half_cell=0;
 void level(bool value){
  output.TransitionIfNeeded(value?BIT_HIGH:BIT_LOW);
  clock_position+=half_cell;
  const auto target=static_cast<U64>(std::llround(clock_position));const auto now=output.GetCurrentSampleNumber();
  if(target>now)output.Advance(static_cast<U32>(target-now));
 }
 void subframe(unsigned channel){
  if(settings.pattern==5&&frame==385&&channel==1){output.Transition();held=true;return;}
  const unsigned position=unsigned(frame%192);
  const uint8_t preamble=channel?0xe4:(position==0?0xe8:0xe2);
  const bool invert=output.GetCurrentBitState()==BIT_HIGH;
  // Literal eight half-cell preamble levels, inverted relative to prior state.
  for(int bit=7;bit>=0;--bit)level(bool((preamble>>bit)&1)^invert);
  std::array<uint8_t,24> status{};status[0]=4;status[3]=2;status[4]=0x0b;
  uint32_t payload=0;
  if(settings.pattern==1||settings.pattern==2||settings.pattern==5){
   status[0]=6;status[4]=0;
   if(frame>=384){
    const uint64_t period=settings.pattern==2?512:1536;
    const auto word_index=2*((frame-384)%period)+channel;
    const uint16_t words[]={0xf872,0x4e1f,uint16_t(settings.pattern==2?0x0b:1),64,0x0b77,0x1234,0x5678,0x9abc};
    if(word_index<8)payload=uint32_t(words[word_index])<<8;
   }
  }else{
   int32_t value=int32_t((frame*257)&0x7fffff);if(channel)value=-value;
   payload=uint32_t(value)&0xffffff;
   if(settings.pattern==4){status.fill(0);status[0]=0x85;status[1]=8;status[2]=0x2c;}
   if(settings.pattern==3&&frame>=384&&frame<768){status[0]=8;status[4]=0x0a;payload&=0xfffff0;}
  }
  uint32_t raw=payload;
  if(settings.pattern==1||settings.pattern==2||settings.pattern==5)raw|=1u<<24; // Non-audio V=1 is not a transport parity fault.
  if((status[position/8]>>(position%8))&1)raw|=1u<<26;
  // U is zero. V is one for non-audio fixtures. Independently count the 27 preceding payload/control bits.
  unsigned ones=0;for(unsigned bit=0;bit<27;++bit)if((raw>>bit)&1)++ones;
  if(ones%2)raw|=1u<<27;
  bool state=output.GetCurrentBitState()==BIT_HIGH;
  for(unsigned bit=0;bit<28;++bit){state=!state;level(state);if((raw>>bit)&1)state=!state;level(state);}
 }
public:
 Signal(){SetAnalyzerSettings(&settings);}
 ~Signal() override {KillThread();}
 void SetupResults() override {results.reset(new Results);SetAnalyzerResults(results.get());}
 void WorkerThread() override {
  auto* input=GetAnalyzerChannelData(settings.channel);
  U64 count=0;
  for(;;){CheckIfThreadShouldExit();if(!input->DoMoreTransitionsExistInCurrentData())ReportProgress(input->GetSampleNumber());input->AdvanceToNextEdge();if((++count&1023)==0)ReportProgress(input->GetSampleNumber());}
 }
 U32 GetMinimumSampleRateHz() override {return 50000000;}
 const char* GetAnalyzerName() const override {return "S/PDIF Test Signal";}
 bool NeedsRerun() override {return false;}
 U32 GenerateSimulationData(U64 requested,U32 device_rate,SimulationChannelDescriptor** channels) override {
  const U32 actual=GetSimulationSampleRate();
  if(!initialized||rate!=actual||old_channel!=settings.channel||old_pattern!=settings.pattern){
   output=SimulationChannelDescriptor();rate=actual;old_channel=settings.channel;old_pattern=settings.pattern;frame=0;held=false;clock_position=100;half_cell=double(rate)/(128*48000.0);
   output.SetChannel(settings.channel);output.SetSampleRate(rate);output.SetInitialBitState(BIT_LOW);output.Advance(100);initialized=true;
  }
  const auto target=AnalyzerHelpers::AdjustSimulationTargetSample(requested,device_rate,rate);
  while(!held&&output.GetCurrentSampleNumber()<target){subframe(0);subframe(1);++frame;}
  while(held&&output.GetCurrentSampleNumber()<target){const auto remaining=target-output.GetCurrentSampleNumber();output.Advance(static_cast<U32>(std::min<U64>(remaining,std::numeric_limits<U32>::max())));}
  *channels=&output;return 1;
 }
};
}
extern "C" {
 ANALYZER_EXPORT const char* __cdecl GetAnalyzerName(){return "S/PDIF Test Signal";}
 ANALYZER_EXPORT Analyzer* __cdecl CreateAnalyzer(){return new test_signal::Signal;}
 ANALYZER_EXPORT void __cdecl DestroyAnalyzer(Analyzer* analyzer){delete analyzer;}
}
