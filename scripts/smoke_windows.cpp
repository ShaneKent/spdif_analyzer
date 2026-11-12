#ifdef _WIN32
#include <windows.h>
#include <iostream>
#include <Analyzer.h>
#include <AnalyzerHelpers.h>
#include <cstring>
int main(int argc,char** argv){
 if(argc!=2){std::cerr<<"Usage: smoke_windows <plugin.dll>\n";return 2;}
 HMODULE dll=LoadLibraryExA(argv[1],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
 if(!dll){std::cerr<<"LoadLibrary failed: "<<GetLastError()<<'\n';return 1;}
 auto name=reinterpret_cast<const char* (__cdecl*)()>(GetProcAddress(dll,"GetAnalyzerName"));
 auto create=reinterpret_cast<Analyzer* (__cdecl*)()>(GetProcAddress(dll,"CreateAnalyzer"));
 auto destroy=reinterpret_cast<void (__cdecl*)(Analyzer*)>(GetProcAddress(dll,"DestroyAnalyzer"));
 if(!name||!create||!destroy){std::cerr<<"Missing analyzer exports\n";return 1;}
 Analyzer* analyzer=create();
 if(!analyzer||std::strcmp(name(),"S/PDIF Analyzer")!=0){std::cerr<<"Unexpected analyzer instance/name\n";return 1;}
 // Settings archive round trip exercises SDK allocations and persistent defaults.
 auto* settings=analyzer->GetAnalyzerSettings();
 std::string saved=settings->SaveSettings();settings->LoadSettings(saved.c_str());
 std::string roundtrip=settings->SaveSettings();
 if(saved!=roundtrip){std::cerr<<"Settings archive round trip failed\n";destroy(analyzer);return 1;}
 if(settings->GetSettingsInterfacesCount()!=11){std::cerr<<"Expected eleven analyzer settings interfaces\n";destroy(analyzer);return 1;}
 auto* detail=static_cast<AnalyzerSettingInterfaceNumberList*>(settings->GetSettingsInterface(3));
 if(std::strcmp(detail->GetTitle(),"Waveform detail")!=0 || detail->GetNumber()!=0){std::cerr<<"Expected Fields default\n";destroy(analyzer);return 1;}
 auto* input=static_cast<AnalyzerSettingInterfaceChannel*>(settings->GetSettingsInterface(0));
 auto* csv=static_cast<AnalyzerSettingInterfaceNumberList*>(settings->GetSettingsInterface(4));
 if(std::strcmp(csv->GetTitle(),"CSV export")!=0 || csv->GetNumber()!=0){std::cerr<<"Expected subframes CSV default\n";destroy(analyzer);return 1;}
 auto* output=static_cast<AnalyzerSettingInterfaceNumberList*>(settings->GetSettingsInterface(5));
 auto* window=static_cast<AnalyzerSettingInterfaceInteger*>(settings->GetSettingsInterface(6));
 auto* bits=static_cast<AnalyzerSettingInterfaceInteger*>(settings->GetSettingsInterface(7));
 auto* rate=static_cast<AnalyzerSettingInterfaceInteger*>(settings->GetSettingsInterface(8));
 auto* stereo=static_cast<AnalyzerSettingInterfaceBool*>(settings->GetSettingsInterface(9));
 auto* table=static_cast<AnalyzerSettingInterfaceBool*>(settings->GetSettingsInterface(10));
 if(std::strcmp(table->GetTitle(),"Structured sample rows")!=0||!table->GetValue()){std::cerr<<"Expected full structured sample rows by default\n";destroy(analyzer);return 1;}
 if(output->GetNumber()!=0||window->GetInteger()!=1000||bits->GetInteger()!=0||rate->GetInteger()!=0||stereo->GetValue()||!table->GetValue()){std::cerr<<"Unexpected v4 defaults\n";destroy(analyzer);return 1;}
 output->SetNumber(7);window->SetInteger(250);bits->SetInteger(20);rate->SetInteger(48000);stereo->SetValue(true);table->SetValue(false);
 input->SetChannel(Channel(1,0,DIGITAL_CHANNEL));detail->SetNumber(1);csv->SetNumber(3);
 if(!settings->SetSettingsFromInterfaces()){std::cerr<<"Unable to select compact waveform view\n";destroy(analyzer);return 1;}
 std::string compact=settings->SaveSettings();settings->LoadSettings(compact.c_str());
 if(detail->GetNumber()!=1 || csv->GetNumber()!=3 || output->GetNumber()!=7 || window->GetInteger()!=250 || bits->GetInteger()!=20 || rate->GetInteger()!=48000 || !stereo->GetValue() || table->GetValue()){std::cerr<<"Compact waveform setting round trip failed\n";destroy(analyzer);return 1;}
 SimpleArchive legacy;Channel c(1,0,DIGITAL_CHANNEL);legacy<<U32(1);legacy<<c;legacy<<U32(48000);legacy<<S32(1);
 settings->LoadSettings(legacy.GetString());
 if(detail->GetNumber()!=0 || csv->GetNumber()!=0 || !table->GetValue()){std::cerr<<"Version1 migration must default to Fields/subframes\n";destroy(analyzer);return 1;}
 SimpleArchive version2;version2<<U32(2);version2<<c;version2<<U32(48000);version2<<S32(0);version2<<S32(1);
 settings->LoadSettings(version2.GetString());
 if(detail->GetNumber()!=1 || csv->GetNumber()!=0 || !table->GetValue()){std::cerr<<"Version2 migration must preserve Samples and default to subframes CSV\n";destroy(analyzer);return 1;}
 SimpleArchive version3;version3<<U32(3);version3<<c;version3<<U32(48000);version3<<S32(0);version3<<S32(0);version3<<S32(3);
 settings->LoadSettings(version3.GetString());
 if(csv->GetNumber()!=3||output->GetNumber()!=0||window->GetInteger()!=1000||bits->GetInteger()!=0||rate->GetInteger()!=0||stereo->GetValue()||!table->GetValue()){std::cerr<<"Version3 migration must preserve CSV and default v4 options\n";destroy(analyzer);return 1;}
 SimpleArchive version4;version4<<U32(4);version4<<c;version4<<U32(48000);version4<<S32(1);version4<<S32(1);version4<<S32(2);version4<<S32(6);version4<<U32(100);version4<<U32(24);version4<<U32(48000);version4<<bool(true);
 settings->LoadSettings(version4.GetString());
 if(output->GetNumber()!=6||window->GetInteger()!=100||bits->GetInteger()!=24||!stereo->GetValue()||detail->GetNumber()!=1||csv->GetNumber()!=2||!table->GetValue()){std::cerr<<"Version4 migration failed\\n";destroy(analyzer);return 1;}
 table->SetValue(false);if(!settings->SetSettingsFromInterfaces()){std::cerr<<"Unable to select sparse table before migration\n";destroy(analyzer);return 1;}
 SimpleArchive version5;version5<<U32(5);version5<<c;version5<<U32(48000);version5<<S32(1);version5<<S32(1);version5<<S32(3);version5<<S32(7);version5<<U32(250);version5<<U32(20);version5<<U32(48000);version5<<bool(true);
 settings->LoadSettings(version5.GetString());
 if(output->GetNumber()!=7||window->GetInteger()!=250||bits->GetInteger()!=20||rate->GetInteger()!=48000||!stereo->GetValue()||detail->GetNumber()!=1||csv->GetNumber()!=3||!table->GetValue()){std::cerr<<"Version5 migration must preserve codec selection and default full structured rows\n";destroy(analyzer);return 1;}
 std::cout<<"Loaded "<<name()<<"; entry points, create/destroy, archivev6 sparse-table round trip and v1/v2/v3/v4/v5 full-table migration passed\n";
 destroy(analyzer);FreeLibrary(dll);return 0;
}
#else
int main(){return 77;}
#endif
