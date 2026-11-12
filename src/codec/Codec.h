#pragma once
#include "core/Decoder.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace spdif::codec {
constexpr size_t max_payload_bytes=24568;
using CheckState=spdif::IntegrityCheck;
enum class HeaderState { Ok, Truncated, Unsupported, Invalid };
enum class CodecKind { Unknown, Ac3, Eac3, DtsCore };
enum class Packing { Unknown, BigEndian16, LittleEndian16, BigEndian14, LittleEndian14 };
const char* header_state_name(HeaderState);
const char* codec_name(CodecKind);
const char* packing_name(Packing);
struct FrameResult {
 HeaderState header=HeaderState::Truncated;
 CodecKind codec=CodecKind::Unknown; Packing packing=Packing::Unknown;
 // Header syntax and declared frame extent only; not audio-data validation.
 CheckState structure=CheckState::NotChecked,crc1=CheckState::NotApplicable,
            header_crc=CheckState::NotApplicable,frame_crc=CheckState::NotChecked;
 unsigned frame_bytes=0,consumed_bytes=0,header_bits=0,sample_rate=0,samples_per_frame=0;
 unsigned channels=0; bool channel_count_known=false,lfe=false;
 std::string channel_configuration,evidence;
 // AC-3 nominal bitrate, E-AC-3 per-frame derived bitrate, DTS target bitrate.
 std::optional<uint32_t> bit_rate;
 std::string bit_rate_kind;
 int bsid=-1,bsmod=-1,channel_mode=-1,stream_type=-1,substream_id=-1;
 int sample_rate_code=-1,frame_size_code=-1,rate_code=-1;
 std::optional<uint16_t> channel_map;
 // crc_present is the DTS CPF header flag only, not AC3/EAC3 CRC availability.
 bool crc_present=false,extension_audio=false,termination_frame=false;
 unsigned deficit_samples=0;
 bool transport_conflict=false;
};
// Byte order is the received transport word order, high byte first. Input may
// contain following frames. valid_bits are HIGH bits of the last byte; at most
// size*8 and max_payload_bytes*8. No sync searching or external dependencies.
// consumed_bytes is zero when the next frame boundary cannot be trusted.
FrameResult inspect_frame(const uint8_t* data,size_t size,uint32_t valid_bits,
                          ContentFormat expected=ContentFormat::Unknown);
struct CodecInfo {
 FrameResult frame{}; ContentInfo transport{};
 uint64_t start=0,end=0,frame_index=0,byte_offset=0;
 bool source_interval_known=false;
};
class Inspector {
 public:
 using Callback=std::function<void(const CodecInfo&)>;
 explicit Inspector(Callback callback); ~Inspector();
 Inspector(const Inspector&)=delete; Inspector& operator=(const Inspector&)=delete;
 void burst(const BurstEvent&);
 // Explicit EOF only. Pending live input remains unverified until continuation.
 void finish();
 private: struct Impl; std::unique_ptr<Impl> impl;
};
}
