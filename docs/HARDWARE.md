# Physical source validation

The steady-source evidence below was collected on Windows x64 using Saleae Logic 2.4.48 at 100 MHz, with a nominal 48 kHz S/PDIF carrier. D6 and D7 were observed concurrently. Private recordings, raw edges, source audio, device identifiers and compressed payload bytes are excluded from public source files.

Finite playback used the Windows digital S/PDIF endpoint with format tag 146 for AC3 and tag 8 for DTS. Source receipts record completed playback and the nominal 48 kHz carrier. Codec inspection identified stereo 48 kHz frames: AC3 at 192 kbit/s nominal and DTS core at 768 kbit/s target transmission rate, both in 16-bit big-endian received packing. These are observed source settings and codec metadata, not assumptions that every digital endpoint has the same capabilities.

## Steady compressed streams

| Evidence | AC3 / Dolby Digital | DTS core type I |
| --- | ---: | ---: |
| Decoded subframes per path | 25,668 | 19,251 |
| Complete status blocks per path, both channels combined | 132 | 98 |
| Complete D6 payloads independently validated | 8 | 18 |
| Payload bytes per burst | 768 | 1,024 |
| Pc / Pd | 1 / 6,144 bits | 11 / 8,192 bits |
| Nominal repetition period, stereo frames | 1,536 | 512 |
| D6 acquisition duration | 0.26738667 s | 0.20053989 s |

Every accepted D6 payload matched a source payload frame byte for byte. Independent FFmpeg decoding with CRC checking returned success without codec errors. A separate inspection of raw exported subframes recovered the upper 16 payload bits at literal `F872 / 4E1F / Pc / Pd` headers and found all eight complete AC3 payloads and all eighteen complete DTS payloads byte identical between D6 and D7, in order. DTS also had a final header whose payload was truncated by the capture boundary; that remainder is not counted as a validated complete payload.

AC3 codec-frame CRC1 and full-frame CRC passed on complete inspected frames. DTS full-frame CRC remains unsupported by this analyzer; successful independent decoding is separate evidence and is not relabeled a passed DTS CRC. See the [codec support matrix](CODECS.md).

Both paths had zero subframe parity failures, zero V-invalid samples and no reported synchronization losses. U was zero throughout these observed captures. The expected two partial channel-status records and one truncated-tail diagnostic describe finite capture boundaries, not evidence that reconnect or physical signal gaps were exercised. Sampled mean transport-rate estimates were approximately 48,001.05 Hz; they are not calibrated oscillator or jitter measurements.

D6 header intervals were 3,199,930–3,199,931 capture samples for AC3 and 1,066,643–1,066,644 samples for DTS, consistent with the respective nominal repetition periods and the observed sampled carrier rate. These are steady-stream cadence checks.

## Observed D6/D7 declaration difference

Complete parity-good channel-status blocks were constant within each capture and agreed between channels:

- D6: `06 12 00 02`, followed by twenty zero bytes.
- D7: `04 12 00 02`, followed by twenty zero bytes.

The sole difference is byte 0, bit 1 (`0x02`): D6 declares consumer non-audio; D7 declares consumer PCM audio. Both declare 48 kHz and category `0x12`, with audio word length unspecified. The unspecified precision must remain unknown.

D6 therefore supplies corroborating status for the complete AC3/DTS transport headers. D7 carries the same compressed payload bytes while declaring PCM, so it correctly produces **PCM status / IEC 61937 header conflict** evidence. A policy requiring an established expected compressed format fails on D7 for that reason. The byte equality does not justify silently overriding its status declaration or presenting those words as valid PCM audio. Codec-derived header/CRC evidence and transport-status trust remain separate.

## Public regression

[Sanitized provenance](../tests/fixtures/hardware-transport-provenance.json) contains only protocol metadata and evidence counts. [test_hardware_regressions.cpp](../tests/test_hardware_regressions.cpp) uses the literal observed status bytes, Pc/Pd values and nominal periods. It generates new deterministic payload words and new quantized 100 MHz transitions; it includes no captured compressed bytes or private media.

The test verifies exact status assembly, unknown precision, even parity, V/U flags, unchanged raw payload fields, repeated burst lengths/cadence, corroborated compressed identification under D6-like status, and explicit conflict under D7-like status. Both nominal polarities are included. These are **derived synthetic transport regressions**. They do not replace the separate physical-source and independent codec evidence above, and they do not assert that the generated payloads are valid compressed audio.

The standalone target needs the portable core and the test-only fixture helper; it has no Logic, hardware, FFmpeg or runtime fixture-file dependency. Its source is compatible with the other C++17 portable tests.

## Source transitions and malformed close tail

A separate 3.0081024-second, 100 MHz recording exercised finite source playback in the order **PCM → AC3 → PCM → DTS → PCM**, closing and reopening the Windows output stream between segments. Both paths produced 288,783 decoded subframes and 1,502 complete channel-status blocks, with zero subframe parity failures and zero synchronization losses. Host request timestamps document source operations; captured source intervals determine protocol timing.

D6 recorded ten channel-status changes, five per channel: its initial non-audio declaration changed to PCM, AC3 non-audio, PCM, DTS non-audio and PCM. D7 retained its constant PCM declaration and recorded 67,584 subframes with PCM-status/IEC-header conflict evidence. D6 also reported one late-burst interval and one interval missing nominal burst periods. These events describe burst cadence; they do not establish missing carrier edges or lost audio.

Independent checks found all six accepted AC3 payloads source-exact and independently decodable. Of twenty accepted DTS payloads, nineteen were source-exact and independently decodable. The last 1,024-byte DTS payload was malformed: it first differed from the closest source frame at byte offset 468, had 535 differing bytes and contained only zeros from offset 468 onward. Independent FFmpeg decoding failed with a DSYNC error. The captured malformed words were preserved rather than substituted with source bytes.

That malformed tail retained valid subframe parity, a complete declared transport length, and a valid inspected DTS common header and physical extent. The analyzer therefore reports the checked header/extent structure as passed while leaving DTS full-frame CRC **unsupported**. It does not claim that the compressed body decoded successfully. The tail was observed around source close/reopen; this evidence does not isolate the responsible driver, device or component. Transport-complete extraction remains distinct from codec-body validity, as the [codec matrix](CODECS.md) specifies.

Neither recorded path had an edge gap over 20 microseconds during those source operations. Source close/reopen therefore **did not demonstrate physical carrier loss, cable reconnect or decoder reacquisition**. Deliberate unplug/reconnect remains a separate pending physical check.

## Remaining physical evidence

No EAC3 recording is established. A read-only direct WinMM capability query tested IEC 61937 EAC3 subformat `0000000a-0cea-0010-8000-00aa00389b71` at a 192 kHz, 16-bit stereo carrier, with both 40-byte and 52-byte format descriptions. Both returned error 32 (`WAVERR_BADFORMAT`) for the selected Windows S/PDIF endpoint. This limits that endpoint/query combination; it does not prove EAC3 unavailable through every API, device or configuration.

Physical signal gaps, unplug/reconnect and reacquisition still need separately recorded and checked evidence. Keep the broader physical-validation work open for these cases. The generated public regression is transport evidence derived from steady observations, not a replay of the transition recording. Automatic control of source playback, device wiring and Logic acquisition is outside the regression test.
