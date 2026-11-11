#include "SpdifAnalyzer.h"
SpdifSettings::SpdifSettings() {
 channel_ui.SetTitleAndTooltip("S/PDIF", "Digitized S/PDIF input; either polarity is supported.");
 channel_ui.SetSelectionOfNoneIsAllowed(false);
 rate_ui.SetTitleAndTooltip("Nominal sample rate (Hz, 0 = automatic)", "Audio frame rate, not BMC bit rate. Manual range: 8000 to 192000 Hz.");
 rate_ui.SetMin(0); rate_ui.SetMax(192000);
 display_ui.SetTitleAndTooltip("Sample display", "Signed values are numeric 24-bit container interpretations, not a claim of valid PCM.");
 display_ui.AddNumber(0,"Signed 24-bit container", "Includes auxiliary bits; effective resolution may be lower.");
 display_ui.AddNumber(1,"Hexadecimal payload", "All 24 payload bits, including auxiliary bits.");
 detail_ui.SetTitleAndTooltip("Waveform detail", "Show each S/PDIF field at its actual position, or a compact whole-subframe sample bubble.");
 detail_ui.AddNumber(0,"Fields", "Preamble, auxiliary nibble, main sample, V, U, C, and P.");
 detail_ui.AddNumber(1,"Samples", "One sample bubble per subframe. CSV and structured results remain identical.");
 export_ui.SetTitleAndTooltip("CSV export", "Choose the contents of the generic analyzer CSV export.");
 export_ui.AddNumber(0,"Subframes", "One row per decoded subframe.");
 export_ui.AddNumber(1,"Channel status", "Complete channel-status blocks, raw bits, and interpretation.");
 export_ui.AddNumber(2,"Diagnostics", "Errors and partial-block evidence with original timestamps.");
 export_ui.AddNumber(3,"Content classification", "PCM/non-PCM classification and IEC 61937 burst evidence.");
 AddInterface(&channel_ui); AddInterface(&rate_ui); AddInterface(&display_ui); AddInterface(&detail_ui); AddInterface(&export_ui);
 const char* options[]={"Subframes CSV", "Channel status CSV", "Diagnostics CSV", "Content classification CSV"};
 for(U32 i=0;i<4;++i){ AddExportOption(i,options[i]); AddExportExtension(i,"CSV","csv"); }
 AddChannel(channel,"S/PDIF",false); Update();
}
void SpdifSettings::Update(){ channel_ui.SetChannel(channel); rate_ui.SetInteger(static_cast<int>(nominal)); display_ui.SetNumber(display); detail_ui.SetNumber(detail); export_ui.SetNumber(csv_export); }
bool SpdifSettings::SetSettingsFromInterfaces(){
 const auto next=channel_ui.GetChannel(); const int rate=rate_ui.GetInteger();
 if(next==UNDEFINED_CHANNEL){ SetErrorText("Select a digital input channel."); return false; }
 if(rate!=0 && (rate<8000 || rate>192000)){ SetErrorText("Use 0 for automatic rate or 8000–192000 Hz."); return false; }
 channel=next; nominal=static_cast<U32>(rate); display=display_ui.GetNumber()==1?1:0; detail=detail_ui.GetNumber()==1?1:0; csv_export=static_cast<int>(export_ui.GetNumber());
 if(csv_export<0 || csv_export>3)csv_export=0;
 ClearChannels(); AddChannel(channel,"S/PDIF",true); return true;
}
void SpdifSettings::LoadSettings(const char* text){
 SimpleArchive a; a.SetString(text); U32 version=0; S32 d=0, waveform=0, selected_export=0; U32 rate=0; Channel c=UNDEFINED_CHANNEL;
 if((a>>version) && (version>=1 && version<=3) && (a>>c) && (a>>rate) && (a>>d) && (rate==0 || (rate>=8000&&rate<=192000)) && (d==0||d==1)){
 if(version>=2 && (!(a>>waveform) || (waveform!=0 && waveform!=1)))waveform=0;
 if(version>=3 && (!(a>>selected_export) || selected_export<0 || selected_export>3))selected_export=0;
 channel=c; nominal=rate; display=d; detail=waveform; csv_export=selected_export;
 } else { channel=UNDEFINED_CHANNEL; nominal=0; display=0; detail=0; csv_export=0; }
 ClearChannels(); AddChannel(channel,"S/PDIF",channel!=UNDEFINED_CHANNEL); Update();
}
const char* SpdifSettings::SaveSettings(){ SimpleArchive a; a<<U32(3); a<<channel; a<<nominal; a<<S32(display); a<<S32(detail); a<<S32(csv_export); return SetReturnString(a.GetString()); }
