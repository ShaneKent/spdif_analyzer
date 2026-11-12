#pragma once
#include "Events.h"
#include "codec/Codec.h"
namespace spdif::json {
inline std::string record(const codec::CodecInfo& info){
 const auto& f=info.frame;auto o=event("codec_frame");
 o.sample("start_sample",info.start).sample("end_sample_inclusive",info.end).boolean("source_interval_known",info.source_interval_known)
  .integer("frame_index",info.frame_index).integer("byte_offset",info.byte_offset)
  .text("codec",codec::codec_name(f.codec)).text("packing",codec::packing_name(f.packing)).text("header_state",codec::header_state_name(f.header))
  .text("structure",integrity_name(f.structure)).text("crc1",integrity_name(f.crc1)).text("header_crc",integrity_name(f.header_crc)).text("frame_crc",integrity_name(f.frame_crc))
  .integer("frame_bytes",f.frame_bytes).integer("consumed_bytes",f.consumed_bytes).integer("header_bits",f.header_bits)
  .integer("sample_rate_hz",f.sample_rate).integer("samples_per_frame",f.samples_per_frame).integer("channels",f.channels).boolean("channel_count_known",f.channel_count_known)
  .text("channel_configuration",f.channel_configuration).boolean("lfe",f.lfe).text("bit_rate_kind",f.bit_rate_kind)
  .signed_integer("bsid",f.bsid).signed_integer("bsmod",f.bsmod).signed_integer("channel_mode",f.channel_mode).signed_integer("stream_type",f.stream_type).signed_integer("substream_id",f.substream_id)
  .signed_integer("sample_rate_code",f.sample_rate_code).signed_integer("frame_size_code",f.frame_size_code).signed_integer("rate_code",f.rate_code)
  .boolean("extension_audio",f.extension_audio).boolean("termination_frame",f.termination_frame).integer("deficit_samples",f.deficit_samples)
  .boolean("transport_conflict",f.transport_conflict).text("evidence",f.evidence)
  .sample("transport_start_sample",info.transport.start).sample("transport_end_sample_inclusive",info.transport.end)
  .text("transport_format",content_name(info.transport.format)).integer("Pc",info.transport.pc).integer("Pd",info.transport.pd).integer("stream_number",info.transport.pc>>13)
  .boolean("transport_complete",info.transport.complete).boolean("transport_integrity",info.transport.integrity);
 if(f.codec==codec::CodecKind::DtsCore)o.boolean("dts_crc_present",f.crc_present);else o.raw("dts_crc_present","null");
 if(f.bit_rate)o.integer("bit_rate",*f.bit_rate);else o.raw("bit_rate","null");
 if(f.channel_map)o.integer("channel_map",*f.channel_map);else o.raw("channel_map","null");
 return o.str();
}
}
