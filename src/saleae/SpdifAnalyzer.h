#pragma once
#include <Analyzer.h>
#include <AnalyzerResults.h>
#include <AnalyzerHelpers.h>
#include "core/Decoder.h"
#include "analysis/Analysis.h"
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <vector>

class SpdifSettings final : public AnalyzerSettings {
public:
 Channel channel=UNDEFINED_CHANNEL; U32 nominal=0; int display=0; int detail=0; int csv_export=0; int output=0; U32 window_ms=1000,pcm_bits=0,wav_rate=0; bool assume_stereo=false;
 SpdifSettings();
 bool SetSettingsFromInterfaces() override;
 void LoadSettings(const char*) override;
 const char* SaveSettings() override;
 void Update();
private:
 AnalyzerSettingInterfaceChannel channel_ui;
 AnalyzerSettingInterfaceInteger rate_ui;
 AnalyzerSettingInterfaceNumberList display_ui;
 AnalyzerSettingInterfaceNumberList detail_ui;
 AnalyzerSettingInterfaceNumberList export_ui;
 AnalyzerSettingInterfaceNumberList output_ui;
 AnalyzerSettingInterfaceInteger window_ui,bits_ui,wav_rate_ui;
 AnalyzerSettingInterfaceBool stereo_ui;
};
struct SpdifAnalysisBatch {
 std::vector<spdif::HealthWindow> health;
 std::vector<spdif::StatusChange> changes;
 std::vector<spdif::PcmWindow> pcm;
};
class SpdifAnalyzer;
class SpdifResults final : public AnalyzerResults {
public:
 SpdifResults(SpdifAnalyzer*,SpdifSettings*);
 void Add(const spdif::Subframe&);
 void Add(const spdif::StatusBlock&);
 void Add(const spdif::Diagnostic&);
 void Add(const spdif::ContentInfo&);
 void Add(const spdif::BurstEvent&);
 void PublishAnalysis(SpdifAnalysisBatch&,const spdif::HealthWindow&,const std::vector<spdif::PcmWindow>&);
 void PublishSnapshot(U64 observed_sample,const std::optional<spdif::BurstEvent>& pending_header={});
 void GenerateBubbleText(U64,Channel&,DisplayBase) override;
 void GenerateExportFile(const char*,DisplayBase,U32) override;
 void GenerateFrameTabularText(U64,DisplayBase) override;
 void GeneratePacketTabularText(U64,DisplayBase) override;
 void GenerateTransactionTabularText(U64,DisplayBase) override;
private:
 struct BurstRecord {spdif::BurstEvent begin{},end{};U64 first_payload=0,words=0;bool closed=false;};
 struct Snapshot {
  U64 frames=0,samples=0,start=0,end=0;
  size_t status=0,diagnostics=0,content=0,changes=0,pcm=0,health=0,bursts=0;
  spdif::HealthWindow summary{};std::vector<spdif::PcmWindow> current_pcm;
  std::optional<spdif::BurstEvent> pending_header;
  BurstRecord tail_burst{};
  int detail=0,output=0,csv_export=0;
  U32 capture_rate=0,channel=0,pcm_bits=0,wav_rate=0;
  U64 trigger=0;
  bool assume_stereo=false,ready=false;
  std::string settings_json;
 };
 Snapshot published_snapshot{};
 Snapshot Freeze();
 spdif::Subframe ReadSubframe(U64 sample_index,const Snapshot&);
 std::string CsvHeader(unsigned) const;
 void CsvRow(unsigned,U64,std::ostream&,const Snapshot&);
 void ExportOutput(const char*,const Snapshot&);
 std::string Text(const Frame&);
 SpdifAnalyzer* analyzer; SpdifSettings* settings;
 struct Notification { U8 type; size_t index; };
 std::vector<Notification> pending;
 U64 suppressed_notifications=0,legacy_frames_written=0,last_decoded_end=0,metadata_end=0;
 U64 PublishPending(const spdif::Subframe&);
 std::mutex mutex;
 std::vector<spdif::StatusBlock> status_blocks;
 std::vector<spdif::Diagnostic> diagnostics;
 std::vector<spdif::ContentInfo> content_events;
 std::vector<spdif::HealthWindow> health_windows;
 std::vector<spdif::StatusChange> status_changes;
 std::vector<spdif::PcmWindow> pcm_windows,current_pcm;
 spdif::HealthWindow health_summary{};
 std::vector<BurstRecord> burst_records;
};
class SpdifAnalyzer final : public Analyzer2 {
public:
 SpdifAnalyzer(); ~SpdifAnalyzer() override;
 void SetupResults() override;
 void WorkerThread() override;
 U32 GenerateSimulationData(U64,U32,SimulationChannelDescriptor**) override;
 U32 GetMinimumSampleRateHz() override;
 const char* GetAnalyzerName() const override;
 bool NeedsRerun() override;
private:
 void SimSubframe(unsigned);
 void SimLevel(bool);
 SpdifSettings settings;
 std::unique_ptr<SpdifResults> results;
 SimulationChannelDescriptor simulation;
 bool simulation_initialized=false;
 U32 simulation_nominal=0; Channel simulation_channel=UNDEFINED_CHANNEL;
 U32 simulation_rate=0; double sim_time=0,sim_half=0;
 uint64_t sim_frame=0;
};
extern "C" {
 ANALYZER_EXPORT const char* __cdecl GetAnalyzerName();
 ANALYZER_EXPORT Analyzer* __cdecl CreateAnalyzer();
 ANALYZER_EXPORT void __cdecl DestroyAnalyzer(Analyzer*);
}
