# Portable export writers

These writers consume the decoder's existing events and use no Saleae API. Every writer requires a **new output directory**; existing directories are rejected. `INCOMPLETE` remains until an explicit successful `finish()` or bundle completion. An interrupted writer may leave temporary `.part` files, which are never valid exports. I/O and malformed-input failures throw exceptions; callers must surface them.

## WAV

Feed `status`, `subframe`, and structural `diagnostic` events in decoder order. Finish with the **exclusive capture-end sample index**. Segment coverage in `index.jsonl` remains inclusive, matching `Subframe.start/end`; capture sample timestamps are decimal strings to preserve values above 2^53.

Only confirmed PCM with good parity, V=0, a known/explicit bit layout, and stereo interpretation is eligible. A/B must arrive in that order at adjacent intervals. Both channels must agree on layout and selected rate. Unknown channel mode requires explicit `assume_stereo`; that option does not bypass PCM, parity, or V checks. A 16..24-bit override resolves unknown precision only; it does not replace an existing declaration. A rate override is explicit and recorded even when it differs from status. Without an override, each channel needs a supported rate declaration; noisy per-subframe clock estimates never become the WAV header rate. No resampling is performed.

Output is little-endian integer **WAVEFORMATEXTENSIBLE**, stereo A-left/B-right with a 24-bit container and explicit valid precision. `pcm_value` extracts the significant declared bits, excluding unused auxiliary/low bits, then left-aligns them in the container. Negative values are multiplied in a widened signed type, avoiding undefined negative shifts. Segment boundaries cover gaps, unpaired/excluded samples, incompatible rate/layout changes, and the RIFF size limit. Sidecar entries give original coverage, precision, selected and declared rates, override flags, excluded intervals, and close reasons. There is no silence substitution or compressed-audio decompression.

Wave format reference: [Microsoft WAVEFORMATEXTENSIBLE](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ksmedia/ns-ksmedia-waveformatextensible).

## IEC 61937 payloads

Feed `BurstEvent` Begin/Word/End in source order. The core's upper-16-bit transport words are written **big-endian, in received A/B order**. Each payload Word has 1..16 valid **high bits**. The unused low bits of the last word are zeroed; the final word remains two bytes even when fewer bits are valid. The index records declared/observed bits and final-word precision, so file byte length alone is not the valid bit length.

Known Pd units are checked independently: AC-3/DTS lengths are bits; E-AC-3 lengths are bytes. No Pa/Pb scan determines the end. Words stream into `.part`; a supported, complete, integrity-good End with the same Pc/Pd and an actual supported codec classification is required before renaming to `.bin`. Provisional/conflicting/unsupported/incomplete/damaged data produces an index rejection with no valid payload file. Null/pause and unknown types do not become codec files. The index includes original interval, stream number, Pc/Pd, format, exact bit count, and integrity evidence. This is transport extraction, not codec validation or decompression.

## Four-CSV snapshot bundle

`write_bundle` accepts four `CsvSource` cursors in order: subframes, channel status, diagnostics, content. Each source supplies its existing CSV header and exactly one row per cursor call, without newline. The caller freezes row counts and event coverage consistently before calling; callbacks must continue to address that immutable view. The writer visits each row once and never copies the complete history. Callers using SDK storage must obey SDK locking rules and avoid holding their metadata mutex across SDK calls.

`BundleSpec.settings_json` must be a valid JSON object produced by the caller's serializer. The manifest records schema/analyzer version, input channel, capture rate, settings, original coverage, frozen row counts, written row counts, and completion state. A consumer must require both `manifest.complete == true` **and absence of `INCOMPLETE`**. Cancellation records `complete:false` and `state:cancelled`; exceptions leave an incomplete marker and attempt to record `state:failed`. Manifest replacement is protected by the incomplete marker, rather than a cross-platform assumption about atomic overwriting. Existing individual CSV APIs remain separate and unchanged.
