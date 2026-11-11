#ifdef _WIN32
#include <windows.h>
#include <iostream>
#include <Analyzer.h>
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
 std::cout<<"Loaded "<<name()<<"; entry points, create/destroy and settings round trip passed\n";
 destroy(analyzer);FreeLibrary(dll);return 0;
}
#else
int main(){return 77;}
#endif
