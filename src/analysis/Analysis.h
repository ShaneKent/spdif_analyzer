#pragma once
#include "core/Decoder.h"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace spdif {
// All intervals are capture-sample [start,end), never seconds rounded to samples.
// Per-channel known coverage is an interval between two consecutive occurrences
// of that channel with uninterrupted alternating A/B transport in between.
// Initial and final cycles and gaps remain unknown; global coverage is the union
// of all decoded subframe intervals, including parity-damaged subframes.
struct ChannelHealth {
 uint64_t decoded_subframes=0,decoded_coverage_samples=0,unknown_coverage_samples=0;
 uint64_t parity_failures=0,v_invalid=0,sync_losses=0,status_good=0,status_damaged=0;
 uint64_t status_crc_passed=0,status_crc_failed=0,status_crc_unverified=0;
 uint64_t rate_measurements=0;
 bool has_fault=false; uint64_t first_fault=0,last_fault=0;
 double mean_sample_rate=0,min_sample_rate=0,max_sample_rate=0;
};
struct HealthWindow {
 uint64_t start=0,end=0,decoded_stream_samples=0,unknown_stream_samples=0;
 uint64_t sync_losses=0,late_events=0;
 std::array<ChannelHealth,2> channels{};
};
struct FieldChange { std::string name,old_value,new_value; };
struct StatusChange {
 unsigned channel=0; uint64_t previous_start=0,previous_end=0,start=0,end=0;
 std::array<uint8_t,24> previous{},current{},changed_mask{};
 std::vector<FieldChange> fields;
};
struct PcmWindow {
 unsigned channel=0,bits=0; bool layout_override=false;
 uint64_t window_start=0,window_end=0,start=0,end=0,segment=0;
 // Exclusions are mutually exclusive in priority order parity, V=1, unknown
 // content, non-PCM content, ambiguous layout. Their sum+accepted is input count.
 uint64_t accepted=0,excluded_parity=0,excluded_invalid=0,excluded_unknown=0,excluded_non_pcm=0,excluded_layout=0;
 int32_t minimum=0,maximum=0; uint64_t peak=0;
 double mean=0,rms=0,peak_normalized=0,rms_normalized=0;
 uint64_t silence_samples=0,full_scale_samples=0,full_scale_runs=0,max_full_scale_run=0;
 double silence_seconds=0;
};
struct AnalysisConfig {
 double capture_rate=0,window_seconds=1;
 uint64_t origin=0;
 // Resolves an unknown layout only; does not override declared layout or PCM,
 // integrity or validity evidence. Zero disables; supported explicit range16..24.
 unsigned layout_override_bits=0;
};
struct AnalysisSink {
 std::function<void(const HealthWindow&)> health;
 std::function<void(const StatusChange&)> status_change;
 std::function<void(const PcmWindow&)> pcm;
};
class Analysis {
public:
 Analysis(AnalysisConfig,AnalysisSink);
 ~Analysis();
 Analysis(const Analysis&)=delete;Analysis& operator=(const Analysis&)=delete;
 void add(const Subframe&);void add(const StatusBlock&);void add(const Diagnostic&);void add(const ContentInfo&);
 // Offline only: end must be at/after all observed subframes. Live callers let
 // complete windows flush as new events establish a conservative watermark.
 void finish(uint64_t end_exclusive);
 HealthWindow summary() const;
 // Read-only copies of the at-most-two open PCM segments; no EOF or emission.
 std::vector<PcmWindow> current_pcm() const;
private:
 struct Impl;std::unique_ptr<Impl> impl;
};
}
