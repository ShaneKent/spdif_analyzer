# Decoder verification

`test_decoder.cpp` is a standalone executable using the public core API. It throws on the first failed check and exits nonzero; successful execution prints a check count. No Saleae SDK or test framework is required.

```sh
g++ -std=c++17 -O2 -Isrc/core -Itests tests/test_decoder.cpp src/core/Decoder.cpp src/core/Status.cpp -o /tmp/spdif-tests
/tmp/spdif-tests
```

`Fixtures.h` generates transition timestamps, not decoded records. Preamble run lengths and payload BMC are encoded separately from the decoder. Seeded jitter is added before timestamp quantization, bounded to ±0.1 half cell. The independently written `literal_vectors` test supplies explicit preamble transition offsets and four hand-checked words, bypassing the fixture encoder:

| Preamble | Edge half-cell offsets before payload |
| --- | --- |
| B | 0, 3, 4, 5 |
| M | 0, 3, 6, 7 |
| W | 0, 3, 5, 6 |

Payload boundaries follow at half-cell 8, 10, …, 62. A one adds its midpoint edge. Literal words include zero, payload one plus parity, signed minimum plus parity, and signed maximum plus parity. The next subframe's first edge closes the last payload cell.

The polarity setting changes the generated level; an inverted signal necessarily has identical transition timestamps. Thus both polarity cases exercise the decoder's intentionally level-independent input contract, not different timestamp inputs.

Coverage includes all approved rates and exactly four-sample and noninteger sampling, auto/manual acquisition, extreme values and flags, jitter at both rate endpoints, separate channel-status data, confirmation of full blocks, partial blocks, corrupted parity, missing/extra edges, glitches, every initial half-cell offset, channel order, gaps with rate change, early/late block starts, slow drift, declared word lengths, unknown/professional/non-audio interpretation, 64-bit timestamps beyond 2^54, exact measured field boundaries at integer/fractional clocks with jitter, and deterministic seeded faults. Fault checks require relevant error codes and recovery within eight subframes, rather than accepting unrelated end-of-capture diagnostics. Clean fixtures permit incomplete-status/truncation diagnostics at their intentionally unconfirmed endings.

The suite tests recovery and structural integrity; it cannot prove analog signal quality, precision jitter performance, GUI behavior, or physical hardware compatibility. Tests do not claim that every arbitrary corruption is detectable: even parity cannot detect every multiple-bit error.

`benchmark.cpp` streams a minute of 192 kHz traffic into a counting sink without retaining the capture or results. It reports throughput and frame count. This excludes Logic's result table, bubble rendering, and CSV storage overhead. Build it in Release/optimized mode; it is intentionally separate from ordinary unit tests.

For memory/undefined behavior checks, compile the test executable with `-fsanitize=address,undefined -fno-omit-frame-pointer`. Some traced sandboxes cannot run LeakSanitizer; disabling leak detection in those environments does not disable address or undefined-behavior checks, but must not be reported as a successful leak check.

## Initial local results (2026-11-11)

The Linux `g++ -O2` suite passed 131,997 checks. The one-minute counting-sink benchmark processed 1,025,280,036 edges and emitted 23,039,999 subframes in 145.87 seconds (0.411 capture seconds per wall second). One final subframe lacks its closing boundary edge; the three diagnostics are two unconfirmed status blocks and the truncated ending. This is a local sandbox measurement, not a Windows or Logic 2 throughput claim.


`ContentTests.h` independently exercises IEC 61937 header identification for AC-3 and DTS types I/II/III, E-AC-3 byte-count lengths, unknown extended types, null/pause, Pc error bit 7 versus valid AC-3 type-dependent bit 8, parity corruption, zero/oversized/odd/truncated lengths, status conflicts, channel order, format change, gap reset, parity invalidation of stale PCM, and stale-burst expiry. Expected formats are checked on both structured events and subsequent subframes. The payload words are synthetic transport fixtures: complete payload extent and header evidence are verified; codec syntax, CRC, decompression, and valid audible output are not.

Type constants and E-AC-3 length units were independently checked against the FFmpeg project's `libavformat/spdif.h` and `libavformat/spdifdec.c`; Pc error placement was cross-checked with its `spdifenc.c` description. No FFmpeg source code or runtime dependency is included.
