#pragma once
#include "core/Decoder.h"
#include <array>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
namespace spdif::exports {
struct WavConfig {
 double capture_rate=0;
 unsigned bits_override=0; // 16..24, used only when declared precision is unknown.
 unsigned rate_override=0; // Explicit integer rate; recorded even if declaration differs.
 bool assume_stereo=false; // Never bypasses PCM, parity or validity gating.
};
class WavWriter {
public:
 WavWriter(const std::filesystem::path& new_directory,WavConfig config);
 ~WavWriter();
 WavWriter(const WavWriter&)=delete;WavWriter& operator=(const WavWriter&)=delete;
 void status(const StatusBlock&);void subframe(const Subframe&);void diagnostic(const Diagnostic&);
 void finish(uint64_t end_exclusive_sample);
private:struct Impl;std::unique_ptr<Impl> impl;
};
class BurstWriter {
public:
 explicit BurstWriter(const std::filesystem::path& new_directory);
 ~BurstWriter();
 BurstWriter(const BurstWriter&)=delete;BurstWriter& operator=(const BurstWriter&)=delete;
 void burst(const BurstEvent&);void finish();
private:struct Impl;std::unique_ptr<Impl> impl;
};
enum class Dataset : unsigned { Subframes,Status,Diagnostics,Content };
struct CsvSource {
 std::string header; // Existing public CSV header, without a trailing newline.
 uint64_t row_count=0; // Frozen count, not a moving live view.
 std::function<void(uint64_t,std::ostream&)> write_row; // Exactly one CSV row, without newline.
};
struct BundleSpec {
 std::string analyzer_version="0.2.0",schema_version="1",input_channel;
 double capture_rate=0;
 std::string settings_json="{}"; // Caller-provided JSON object, included verbatim.
 uint64_t coverage_start=0,coverage_end=0; // Inclusive source sample indices.
};
struct BundleResult { bool complete=false;std::array<uint64_t,4> rows_written{}; };
// Source callbacks must address one immutable snapshot; no history is copied.
// Requires a new directory. Cancellation keeps manifest complete:false and an
// INCOMPLETE marker. I/O/source exceptions also preserve that incomplete state.
BundleResult write_bundle(const std::filesystem::path& new_directory,const BundleSpec&,
 const std::array<CsvSource,4>&,std::function<bool()> cancelled={});
}
