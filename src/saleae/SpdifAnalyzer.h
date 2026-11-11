#pragma once
#include <Analyzer.h>
#include <AnalyzerResults.h>
#include <AnalyzerHelpers.h>
#include "core/Decoder.h"
#include <mutex>
#include <vector>

class SpdifSettings final : public AnalyzerSettings {
public:
 Channel channel=UNDEFINED_CHANNEL; U32 nominal=0; int display=0; int detail=0; int csv_export=0;
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
};
class SpdifAnalyzer;
class SpdifResults final : public AnalyzerResults {
public:
 SpdifResults(SpdifAnalyzer*,SpdifSettings*);
 void Add(const spdif::Subframe&);
 void Add(const spdif::StatusBlock&);
 void Add(const spdif::Diagnostic&);
 void Add(const spdif::ContentInfo&);
 void GenerateBubbleText(U64,Channel&,DisplayBase) override;
 void GenerateExportFile(const char*,DisplayBase,U32) override;
 void GenerateFrameTabularText(U64,DisplayBase) override;
 void GeneratePacketTabularText(U64,DisplayBase) override;
 void GenerateTransactionTabularText(U64,DisplayBase) override;
private:
 std::string Text(const Frame&);
 SpdifAnalyzer* analyzer; SpdifSettings* settings;
 struct Notification { U8 type; size_t index; };
 std::vector<Notification> pending;
 U64 suppressed_notifications=0;
 U64 PublishPending(const spdif::Subframe&);
 std::mutex mutex;
 std::vector<spdif::StatusBlock> status_blocks;
 std::vector<spdif::Diagnostic> diagnostics;
 std::vector<spdif::ContentInfo> content_events;
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
