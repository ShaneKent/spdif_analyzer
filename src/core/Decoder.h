#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
namespace spdif {
struct Subframe {
 uint64_t start=0,end=0; char preamble='?'; unsigned channel=0; uint32_t raw=0;
 int32_t sample=0; bool validity=false,user=false,status=false,parity=false,parity_ok=false;
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
};
struct Config { double capture_rate=0; double nominal_rate=0; };
class Decoder {
 public:
 Decoder(Config config, Sink sink); ~Decoder();
 Decoder(const Decoder&)=delete; Decoder& operator=(const Decoder&)=delete;
 void edge(uint64_t sample); void finish(uint64_t end);
 private: struct Impl; Impl* impl;
};
std::string interpret_status(const std::array<uint8_t,24>& bytes);
}
