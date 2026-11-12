# Analysis and exports in v0.2

The decoder, Logic adapter, and offline tools share typed status/layout evidence and decoded events. These features extend existing samples, fields, flags, content identification and four CSV formats.

| GitHub request | Capability | Main verification |
| --- | --- | --- |
| [#1](https://github.com/ShaneKent/spdif_analyzer/issues/1) | Per-channel capture-health totals and configurable windows | Analysis fixtures and Logic health export |
| [#2](https://github.com/ShaneKent/spdif_analyzer/issues/2) | Sparse, trusted channel-status differences | Independent status transitions and gap/damage cases |
| [#3](https://github.com/ShaneKent/spdif_analyzer/issues/3) | AC-3/DTS burst cadence and overlap evidence | Transport-frame cadence and legal header-shaped payload vectors |
| [#4](https://github.com/ShaneKent/spdif_analyzer/issues/4) | Segmented PCM WAV plus timestamp index | Byte-level WAV round trips and exclusion/segmentation fixtures |
| [#5](https://github.com/ShaneKent/spdif_analyzer/issues/5) | Complete trusted burst payload extraction | Exact payload bytes, partial final words and rejected candidate indices |
| [#6](https://github.com/ShaneKent/spdif_analyzer/issues/6) | PCM peak/RMS/DC/silence/full-scale statistics | Independent numeric vectors and gating/reset cases |
| [#7](https://github.com/ShaneKent/spdif_analyzer/issues/7) | Relative two-stream PCM comparison | Delay, mismatch, holes, ambiguity, layout/rate and clock-drift cases |
| [#8](https://github.com/ShaneKent/spdif_analyzer/issues/8) | Supported professional AES3 status interpretation | Hand-checked professional field and classification vectors |
| [#9](https://github.com/ShaneKent/spdif_analyzer/issues/9) | Versioned offline JSONL and failure policies | Black-box parser, timestamp and exit-code tests |
| [#10](https://github.com/ShaneKent/spdif_analyzer/issues/10) | Four-CSV bundle from one frozen snapshot | Cursor counts, manifest, cancellation and application export checks |

## Health and PCM statistics

Health separates structural decoded coverage from unknown intervals and payload validity. Counts include parity errors, raw V=1, synchronization losses and complete/damaged status blocks. V=1 is not automatically a transport-corruption indicator, especially for non-PCM content. Per-channel coverage is established conservatively from uninterrupted A/B cycles; initial and final uncertainty remain explicit. Timing estimates describe sampled digital data, not calibrated clock jitter.

PCM statistics operate on confirmed PCM with good parity, V=0 and a supported declared precision or explicit unknown-precision override. Rejected data is counted by reason. Values are in declared integer PCM units, with normalized peak/RMS also available. Silence means exact zero-valued accepted samples; full-scale runs are digital observations, not proof of analog clipping. Windows and contiguous segments reset across discontinuities and relevant changes. A measured-rate jump guard avoids splitting ordinary sample-quantization fluctuations; status/layout changes are tracked independently.

Live reports use a read-only snapshot of current statistics. They do not invent an EOF or extend a held sample through an unknown gap. In JSON, source windows and analysis intervals are half-open; decoder subframe and raw block source ends remain explicitly inclusive.

Logic health JSON includes the cumulative live summary and a `health_windows` array containing every completed window in its frozen snapshot. Sparse table notifications may summarize a large event batch; this export retains the window records. Standalone reports include analyzer version, input channel, capture rate, settings and observed coverage; status-change and PCM JSONL begin with a metadata record.

## Status changes and professional streams

Changes compare successive complete, parity-good blocks separately for A and B. They include raw changed-bit masks and old/new interpreted values. Identical blocks produce no change event; initial acquisition is a baseline. Damaged/incomplete blocks and synchronization loss prevent stale comparisons.

Supported professional status includes emphasis, channel mode, nominal frequency and word-length coordination. Stereo and two-independent-channel modes have distinct meanings; independent A/B channels are not universally labeled left/right. Unsupported codes remain numeric and raw bytes are retained. An explicit unreliable-format flag (byte 22 bit 4) prevents PCM/layout trust and resets the semantic status-change baseline. Other reliability flags concern separate status regions; changing addresses or CRC bytes does not invalidate unchanged format fields. Professional channel-status CRC is explicitly not checked; integrity currently refers to the contributing subframe parity and structural evidence.

Primary field references: [ALSA IEC958 definitions](https://github.com/torvalds/linux/blob/master/include/sound/asoundef.h) and [Cirrus AN22, Tables 2–3](https://statics.cirrus.com/pubs/appNote/an22.pdf). Professional word-length encodings are interpreted separately from consumer encodings.

## Compressed-transport diagnostics and extraction

AC-3/DTS cadence uses transport-frame spacing for each stream number. Early/late/missing cadence is established by a subsequent supported header, not by an incomplete capture ending. Gaps and relevant changes reset evidence. Unknown, null and pause types do not inherit unsupported cadence rules.

A header-shaped sequence can legally appear inside payload data. An overlap candidate is therefore explicitly ambiguous and does not split the payload. Declared payload capacity and observed cadence provide separate diagnostics. Length units and transport periods follow the supported type definitions; E-AC-3 length units differ from AC-3/DTS.

Payload writers stage data and commit a file only after supported, complete, integrity-good transport evidence. The index retains rejected, provisional and incomplete cases without presenting their files as valid audio. Output is **big-endian 16-bit transport words in received A/B order**. Valid bits occupy the high part of a partial final word; unused low bits are zero, the final two-byte word is retained, and the exact declared bit count is in the index. No searching for Pa/Pb is used to infer unsupported payload endings. Codec decompression and codec CRC validation are outside scope.

Primary transport references: [FFmpeg type definitions](https://github.com/FFmpeg/FFmpeg/blob/master/libavformat/spdif.h) and [transport decoder](https://github.com/FFmpeg/FFmpeg/blob/master/libavformat/spdifdec.c). No FFmpeg implementation or runtime dependency is included.

## Audio and bundle exports

WAV exports use a 24-bit integer WAVEFORMATEXTENSIBLE stereo container, with declared/explicit valid precision recorded. Actual signed PCM values are left aligned; auxiliary/padding bits are not silently turned into audio. Only complete compatible A/B pairs are written. Gaps, excluded samples and incompatible rate/layout changes split files, with source intervals and reasons in `index.jsonl`. A declared rate is required unless an explicit rate override is selected. Overrides are recorded and do not bypass PCM/parity/validity gates or resample data.

The CSV bundle preserves all four individual formats in one snapshot. Its manifest records version, input, sampling/settings, coverage and snapshot/written row counts. It streams frozen cursors rather than copying the subframe history. A bundle is complete only when its manifest says so and the `INCOMPLETE` marker is absent. Cancellation or I/O failure leaves explicit incomplete state. Existing directories are never overwritten.

In Logic, **Export output** selects reports or directory exports; **Selected CSV** preserves the existing **CSV export** choice. Directory exports append `.wav-export`, `.bursts`, or `.bundle` to the selected output filename. Errors are written to a separate `.error.json`; check this and the directory completion marker rather than relying only on the host's export toast.
