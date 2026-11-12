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
 Channel channel=UNDEFINED_CHANNEL;U32 pattern=0,audio_rate=48000;
 Settings(){
  input.SetTitleAndTooltip("Digital channel","QA-only simulated S/PDIF output channel.");input.SetSelectionOfNoneIsAllowed(false);
  choices.SetTitleAndTooltip("Pattern","Independent synthetic transport fixture; compressed payloads are not codec-valid audio.");
  const char* names[]={"PCM ramp","AC3 bursts","DTS I bursts","Status changes","Professional A/B PCM","Partial IEC header",
                       "Professional CRC examples","Professional CRC fault transition","Gap and parity fault","Rate transition"};
  for(unsigned i=0;i<10;++i)choices.AddNumber(i,names[i],names[i]);
  rates.SetTitleAndTooltip("Audio sample rate", "Physical frame rate; CRC examples preserve their literal nominal-rate-not-indicated status bytes.");
  for(U32 rate:{48000u,96000u,192000u})rates.AddNumber(rate,rate==48000?"48000 Hz":rate==96000?"96000 Hz":"192000 Hz", "Independent QA physical audio frame rate.");
  AddInterface(&input);AddInterface(&choices);AddInterface(&rates);AddChannel(channel,"Test signal",false);update();
 }
 bool SetSettingsFromInterfaces() override {
  const auto next=input.GetChannel();const double selected=choices.GetNumber(),selected_rate=rates.GetNumber();
  if(next==UNDEFINED_CHANNEL||next.mDataType!=DIGITAL_CHANNEL||selected<0||selected>9||std::floor(selected)!=selected||(selected_rate!=48000&&selected_rate!=96000&&selected_rate!=192000)){SetErrorText("Select a digital channel and fixture pattern.");return false;}
  channel=next;pattern=static_cast<U32>(selected);audio_rate=static_cast<U32>(selected_rate);ClearChannels();AddChannel(channel,"Test signal",true);return true;
 }
 void LoadSettings(const char* text) override {SimpleArchive a;a.SetString(text);U32 version=0,p=0,selected_rate=48000;Channel c=UNDEFINED_CHANNEL;
  bool valid=(a>>version)&&(version==1||version==2)&&(a>>c)&&(a>>p)&&p<(version==1?6u:10u)&&c.mDataType==DIGITAL_CHANNEL;
  if(valid&&version==2)valid=(a>>selected_rate)&&(selected_rate==48000||selected_rate==96000||selected_rate==192000);
  if(valid){channel=c;pattern=p;audio_rate=selected_rate;}else{channel=UNDEFINED_CHANNEL;pattern=0;audio_rate=48000;}
  ClearChannels();AddChannel(channel,"Test signal",channel!=UNDEFINED_CHANNEL);update();
 }
 const char* SaveSettings() override {SimpleArchive a;a<<U32(2);a<<channel;a<<pattern;a<<audio_rate;return SetReturnString(a.GetString());}
private:
 AnalyzerSettingInterfaceChannel input;AnalyzerSettingInterfaceNumberList choices,rates;
 void update(){input.SetChannel(channel);choices.SetNumber(pattern);rates.SetNumber(audio_rate);}
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
 bool held=false,initialized=false;Channel old_channel=UNDEFINED_CHANNEL;U32 old_pattern=0,old_audio_rate=0,rate=0;
 uint64_t frame=0;double clock_position=100,half_cell=0;
 void level(bool value){
  output.TransitionIfNeeded(value?BIT_HIGH:BIT_LOW);
  clock_position+=half_cell;
  const auto target=static_cast<U64>(std::llround(clock_position));const auto now=output.GetCurrentSampleNumber();
  if(target>now)output.Advance(static_cast<U32>(target-now));
 }
 U32 effective_rate() const {
  if(settings.pattern==9&&(frame/192)%8>=4)return settings.audio_rate==48000?96000:settings.audio_rate==96000?192000:48000;
  return settings.audio_rate;
 }
 void subframe(unsigned channel){
  const U32 audio=effective_rate();half_cell=double(rate)/(128.0*audio);
  // A boundary transition completes the previous P bit before a long quiet
  // interval. The next normal B preamble starts after64 missing frame periods.
  if(settings.pattern==8&&frame%1152==384&&channel==0){
   output.Transition();clock_position+=128.0*half_cell*64;
   const auto target=static_cast<U64>(std::llround(clock_position));
   if(target>output.GetCurrentSampleNumber())output.Advance(static_cast<U32>(target-output.GetCurrentSampleNumber()));
  }
  if(settings.pattern==5&&frame==385&&channel==1){output.Transition();held=true;return;}
  const unsigned position=unsigned(frame%192);
  const uint8_t preamble=channel?0xe4:(position==0?0xe8:0xe2);
  const bool invert=output.GetCurrentBitState()==BIT_HIGH;
  // Literal eight half-cell preamble levels, inverted relative to prior state.
  for(int bit=7;bit>=0;--bit)level(bool((preamble>>bit)&1)^invert);
  std::array<uint8_t,24> status{};status[0]=4;status[3]=audio==48000?2:audio==96000?10:14;status[4]=0x0b;
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
   if(settings.pattern==4){status.fill(0);status[0]=audio==48000?0x85:0x05;status[1]=8;status[2]=0x2c;}
   if(settings.pattern==6||settings.pattern==7){
    // PRIMARY independent constants: AES3-1992(r1997) AnnexB examples1/2.
    // Example1 bytes0=3D,1=02,4=02,23=9B; example2 bytes0=01,23=32.
    // Every other byte is zero. No production CRC/encoder is called here.
    status.fill(0);
    const auto block=frame/192;
    const bool first=settings.pattern==7||(block%4>=2);
    if(first){status[0]=0x3d;status[1]=2;status[4]=2;status[23]=0x9b;}
    else{status[0]=1;status[23]=0x32;}
    if(settings.pattern==7&&(block%6==2||block%6==3))status[23]=0x9a;
   }
   if(settings.pattern==3&&frame>=384&&frame<768){status[0]=8;status[4]=0x0a;payload&=0xfffff0;}
  }
  uint32_t raw=payload;
  if(settings.pattern==1||settings.pattern==2||settings.pattern==5)raw|=1u<<24; // Non-audio V=1 is not a transport parity fault.
  if((status[position/8]>>(position%8))&1)raw|=1u<<26;
  // U is zero. V is one for non-audio fixtures. Independently count the 27 preceding payload/control bits.
  unsigned ones=0;for(unsigned bit=0;bit<27;++bit)if((raw>>bit)&1)++ones;
  if(ones%2)raw|=1u<<27;
  if(settings.pattern==8&&frame%1152==640&&channel==0)raw^=1u<<27;
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
 U32 GetMinimumSampleRateHz() override {
  U32 maximum=settings.audio_rate;
  if(settings.pattern==9)maximum=std::max<U32>(maximum,settings.audio_rate==48000?96000:settings.audio_rate==96000?192000:48000);
  return std::max<U32>(50000000,maximum*1024);
 }
 const char* GetAnalyzerName() const override {return "S/PDIF Test Signal";}
 bool NeedsRerun() override {return false;}
 U32 GenerateSimulationData(U64 requested,U32 device_rate,SimulationChannelDescriptor** channels) override {
  const U32 actual=GetSimulationSampleRate();
  if(!initialized||rate!=actual||old_channel!=settings.channel||old_pattern!=settings.pattern||old_audio_rate!=settings.audio_rate){
   output=SimulationChannelDescriptor();rate=actual;old_channel=settings.channel;old_pattern=settings.pattern;old_audio_rate=settings.audio_rate;frame=0;held=false;clock_position=100;half_cell=double(rate)/(128.0*settings.audio_rate);
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
