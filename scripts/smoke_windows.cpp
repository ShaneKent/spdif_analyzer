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
 if(settings->GetSettingsInterfacesCount()!=4){std::cerr<<"Expected waveform detail setting\n";destroy(analyzer);return 1;}
 auto* detail=static_cast<AnalyzerSettingInterfaceNumberList*>(settings->GetSettingsInterface(3));
 if(std::strcmp(detail->GetTitle(),"Waveform detail")!=0 || detail->GetNumber()!=0){std::cerr<<"Expected Fields default\n";destroy(analyzer);return 1;}
 auto* input=static_cast<AnalyzerSettingInterfaceChannel*>(settings->GetSettingsInterface(0));
 input->SetChannel(Channel(1,0,DIGITAL_CHANNEL));detail->SetNumber(1);
 if(!settings->SetSettingsFromInterfaces()){std::cerr<<"Unable to select compact waveform view\n";destroy(analyzer);return 1;}
 std::string compact=settings->SaveSettings();settings->LoadSettings(compact.c_str());
 if(detail->GetNumber()!=1){std::cerr<<"Compact waveform setting round trip failed\n";destroy(analyzer);return 1;}
 SimpleArchive legacy;Channel c(1,0,DIGITAL_CHANNEL);legacy<<U32(1);legacy<<c;legacy<<U32(48000);legacy<<S32(1);
 settings->LoadSettings(legacy.GetString());
 if(detail->GetNumber()!=0){std::cerr<<"Version1 migration must default to Fields\n";destroy(analyzer);return 1;}
 std::cout<<"Loaded "<<name()<<"; entry points, create/destroy and settings round trip, waveform selection and v1 migration passed\n";
 destroy(analyzer);FreeLibrary(dll);return 0;
}
#else
int main(){return 77;}
#endif
