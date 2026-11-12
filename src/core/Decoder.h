#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
namespace spdif {
enum class ContentFormat : uint8_t { Unknown, Pcm, NonPcm, Ac3, Dts1, Dts2, Dts3, Eac3, Null, Pause, Unsupported, Provisional, Conflict };
const char* content_name(ContentFormat format);
struct ContentInfo {
 uint64_t start=0,end=0; ContentFormat format=ContentFormat::Unknown;
 std::string evidence; uint16_t pc=0,pd=0;
 bool burst=false,complete=false,integrity=false;
};
struct SampleLayout {
 unsigned bits=0; bool known=false,stereo=false,professional=false;
};
struct StatusInfo {
 bool professional=false,supported=false,non_audio=false,stereo=false;
 unsigned channel_mode=0,emphasis_code=0,sample_rate=0,word_length=0;
 SampleLayout layout{};
};
// Stateless field interpretation; caller must establish block completeness/integrity.
StatusInfo status_info(const std::array<uint8_t,24>& bytes);
int32_t pcm_value(uint32_t raw,const SampleLayout& layout);
// Every Begin receives one End; rejected/unsupported lengths produce no Word.
// Partial headers abandoned on loss/EOF produce unsupported Begin/End forensic
// candidates with zero declared_bits, unconfirmed Pc/Pd, and no payload.
// Word retains raw upper-16 payload ordering; valid_bits occupy its HIGH bits.
// End.info.complete/integrity plus supported and corroborated codec govern export.
enum class BurstPhase { Begin, Word, End };
struct BurstEvent {
 BurstPhase phase=BurstPhase::Begin; ContentInfo info{};
 uint64_t index=0; uint16_t word=0; uint8_t valid_bits=0;
 uint32_t declared_bits=0; bool supported=false;
};
struct Subframe {
 uint64_t start=0,end=0; char preamble='?'; unsigned channel=0; uint32_t raw=0;
 int32_t sample=0; bool validity=false,user=false,status=false,parity=false,parity_ok=false;
 // Actual edge timestamps delimiting preamble, auxiliary nibble, main sample,
 // V, U, C and P. The final boundary is exclusive; start/end remain inclusive.
 std::array<uint64_t,8> field_boundaries{};
 ContentFormat content=ContentFormat::Unknown;
 SampleLayout layout{};
 int block_position=-1; double sample_rate=0;
};
struct StatusBlock {
 uint64_t start=0,end=0; unsigned channel=0; std::array<uint8_t,24> c{},u{};
 bool integrity=true; std::string interpretation;
};
struct Diagnostic { uint64_t start=0,end=0; std::string code,message; };
struct Sink {
 std::function<void(const Subframe&)> subframe;
 std::function<void(const StatusBlock&)> status;
 std::function<void(const Diagnostic&)> diagnostic;
 std::function<void(const ContentInfo&)> content;
 std::function<void(const BurstEvent&)> burst{};
};
struct Config { double capture_rate=0; double nominal_rate=0; };
class Decoder {
 public:
 Decoder(Config config, Sink sink); ~Decoder();
 Decoder(const Decoder&)=delete; Decoder& operator=(const Decoder&)=delete;
 void edge(uint64_t sample); void finish(uint64_t end);
 // Read-only producer-thread snapshot of a decoded Pa[/Pb/Pc] candidate.
 // No events are emitted; continuation and explicit EOF semantics are unchanged.
 std::optional<BurstEvent> pending_header() const;
 private: struct Impl; Impl* impl;
};
std::string interpret_status(const std::array<uint8_t,24>& bytes);
}
