# Decoded results

The `subframe` table record exposes channel A/B (conventionally left/right), B/M/W preamble, raw slots 4–31, 24-bit hexadecimal payload, signed 24-bit numeric container, auxiliary nibble, main 20-bit field, V/U/C/P flags, parity check, measured frame rate, and block position when established.

`coverage_start_sample` and `coverage_end_sample` are the original **inclusive** capture sample indices. Legacy bubbles and subframe CSV retain these exact intervals. Structured metadata notifications reserve a few samples at the beginning of the following subframe to meet Saleae's ordered, nonoverlapping FrameV2 requirement. The table's displayed start/duration for these notifications is their publication location; use coverage fields for the actual covered interval.

`channel_status` contains separate 24-byte C and U arrays for each channel. Byte zero holds the first eight bits in transmission order, earliest bit in the least significant position. A confirming next B is required before a block is complete. Integrity false means at least one contributing subframe failed parity. Interpretation covers consumer mode zero and supported professional fields; unknown field codes remain numeric. Professional status CRC is explicitly unchecked.

`diagnostic` contains a stable code and explanation. Codes include `searching`, `sync_loss`, `parity`, `channel_order`, `block_length`, `incomplete_status`, `truncated` (offline EOF), and `capture_rate_low` (manual-rate plugin check). Partial-status diagnostic text includes the channel, collected bit count, and raw zero-padded C/U bytes. Padding does not imply those uncollected bits were zero.

Queued metadata notifications are bounded. Excess notifications become a `notification_summary` row; original events remain in their respective CSV or analysis-report exports. Without a following decoded subframe, metadata remains export-only. Noise acquisition diagnostics are rate-limited. The decoder's buffers are bounded, while Logic's stored results and metadata for export necessarily grow with the capture.

## CSV exports

The analyzer-specific export menu offers **Subframes CSV**, **Channel status CSV**, **Diagnostics CSV**, and **Content classification CSV**. Times are seconds relative to the capture trigger; end times reference the last inclusive sample. The subframe CSV uses `signed_container_24` and `sample_meaning=numeric container only` to avoid treating unknown/non-audio content as valid PCM.

Parity failure does not discard the raw payload. V=1 means invalid for conversion. Effective word length is declared channel-status information, not a property inferred from zero bits in a sample. Transport sample rate is measured from digital timing; it is distinct from a nominal rate declared in channel status.

The automated legacy-export API invokes the generic first SDK export option. The analyzer's **Export output** and **CSV export** settings select its contents, including metadata CSVs and v0.2 reports. Explicit named nonzero SDK export options retain their own CSV format. Structured records are also available through the data-table export API.


## Content classification

`content` structured records describe format evidence and transport bursts: format, explanation, raw Pc/Pd, burst/completeness/integrity flags and original coverage. Each subframe also carries its current `content` label, which appears in the main-field or compact sample bubble and subframe CSV.

PCM requires confirmed, parity-good supported status with agreeing audio flags on both channels. Supported professional stereo and independent-channel modes are interpreted separately from consumer mode zero. Independent A/B channels do not imply left/right stereo. Non-audio status alone yields Non-PCM (unidentified). Unsupported modes, missing status, or damaged evidence remain unknown.

The detector reads the upper 16 payload bits in A/B order and recognizes IEC 61937 Pa/Pb/Pc/Pd. Supported transport identifiers are AC-3, DTS I/II/III and E-AC-3; null, pause and unsupported identifiers remain explicit. Pc bit 7 is the error flag; type-specific bits 8–12 are not an error flag. Pc/Pd are preserved. AC-3/DTS payload lengths are in bits; E-AC-3 lengths are in bytes. Lengths are bounded by supported burst capacity, without allocating codec payload buffers.

A header with unconfirmed channel status is provisional. A header disagreeing with PCM status reports a conflict. A known format becomes the per-sample classification only after its declared payload extent is observed with appropriate status and integrity. `complete` means the transport extent was observed; it does not validate codec CRCs, frame internals or audible content. Raw DTS hidden in a stream marked PCM is not heuristically relabeled as DTS.

Discontinuities, damaged parity, conflicting status, and expired burst evidence clear stale classification. Channel status can change at block boundaries; the core clears a previous classification as soon as the relevant newly received status differs. EOF invalidates unconfirmed partial evidence in offline tools.

Primary transport references: [FFmpeg IEC 61937 identifiers](https://github.com/FFmpeg/FFmpeg/blob/master/libavformat/spdif.h), [FFmpeg transport muxer](https://github.com/FFmpeg/FFmpeg/blob/master/libavformat/spdifenc.c), and [SMPTE ST 338](https://pub.smpte.org/pub/st338/st0338-2016.pdf). These are interoperability references; no FFmpeg runtime dependency or implementation code is included.

Choose **CSV export** in the analyzer settings before using the generic Export to TXT/CSV action or the automation API. Choices are Subframes, Channel status, Diagnostics, and Content classification. This also makes metadata exports available in Logic versions whose generic export action exposes only the first SDK option. Explicit named nonzero SDK export commands retain their own format.

## Analysis results

The subframe CSV appends `layout_bits`, `layout_known`, `layout_stereo`, and `layout_professional`, preserving the original columns. The effective audio word length is independent of the 24-bit numeric container. A known layout may be cleared as soon as a relevant incoming status bit changes, before a replacement block is trusted.

Sparse `health_window`, `status_change`, and `pcm_window` structured notifications include useful flat columns and the complete versioned JSON record. Use the [analysis exports](FEATURES.md) for all retained records. Health JSON contains cumulative counts and completed windows; current PCM windows have `live_snapshot:true`. Analysis intervals use explicitly named exclusive ends, while raw subframe/status coverage retains inclusive ends. FrameV2 publication positions are not the source analysis intervals.
