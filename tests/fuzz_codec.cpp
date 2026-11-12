#include "codec/Codec.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <tuple>
namespace {
void require(bool value){if(!value)std::abort();}
auto fields(const spdif::codec::FrameResult& f){return std::tie(f.header,f.codec,f.packing,f.structure,f.crc1,f.header_crc,f.frame_crc,f.frame_bytes,f.consumed_bytes,f.header_bits,f.sample_rate,f.samples_per_frame,f.channels,f.channel_count_known,f.lfe,f.channel_configuration,f.evidence,f.bit_rate,f.bit_rate_kind,f.bsid,f.bsmod,f.channel_mode,f.stream_type,f.substream_id,f.sample_rate_code,f.frame_size_code,f.rate_code,f.channel_map,f.crc_present,f.extension_audio,f.termination_frame,f.deficit_samples,f.transport_conflict);}
}
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data,size_t size){
 if(!size||size>spdif::codec::max_payload_bytes+1)return 0;
 const auto* payload=data+1;const size_t bytes=size-1;
 const spdif::ContentFormat expected[]={spdif::ContentFormat::Unknown,spdif::ContentFormat::Ac3,spdif::ContentFormat::Eac3,spdif::ContentFormat::Dts1,spdif::ContentFormat::Dts2,spdif::ContentFormat::Dts3};
 const unsigned partial=bytes?std::min<unsigned>((data[0]>>3)&7,unsigned(bytes*8)):0;
 const uint32_t valid=uint32_t(bytes*8)-partial;
 const auto first=spdif::codec::inspect_frame(payload,bytes,valid,expected[(data[0]&7)%6]);
 const auto second=spdif::codec::inspect_frame(payload,bytes,valid,expected[(data[0]&7)%6]);
 require(fields(first)==fields(second));
 require(first.consumed_bytes<=bytes&&uint64_t(first.consumed_bytes)*8<=valid);
 if(first.consumed_bytes)require(first.structure==spdif::IntegrityCheck::Passed&&first.frame_bytes>0);
 if(first.header==spdif::codec::HeaderState::Ok)require(first.codec!=spdif::codec::CodecKind::Unknown&&first.frame_bytes>0&&first.header_bits>0&&first.sample_rate>0);
 // Verify advertised public bounds and null-pointer rejection without exposing
 // an oversized read. Invalid extents must be rejected before payload access.
 const auto excessive=spdif::codec::inspect_frame(payload,spdif::codec::max_payload_bytes+1,0);
 require(excessive.header==spdif::codec::HeaderState::Invalid&&excessive.consumed_bytes==0);
 const auto null=spdif::codec::inspect_frame(nullptr,1,8);
 require(null.header==spdif::codec::HeaderState::Invalid&&null.consumed_bytes==0);
 return 0;
}
