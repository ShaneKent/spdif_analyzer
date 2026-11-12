# Validation record — 2026-11-11

## Version 0.2 verification

The feature suite builds the same SDK-independent decoder, analysis and export libraries used by Logic. Separate executables cover the original **131,997 decoder checks**, **175 professional/cadence/streaming checks**, **2,536 analysis checks**, and **95 export checks**. Python subprocess suites cover offline JSONL/policy behavior and two-stream comparison. The Windows build runs all six CTest targets; the DLL lifecycle harness additionally verifies all ten settings, v4 persistence, and migrations from v1/v2/v3.

The portable export suite includes a deliberately hostile global C++ locale with decimal commas and digit grouping. JSON indexes/manifests, cursor CSV numbers, raw hexadecimal strings and generated filenames use explicit locale-independent formatting. Release and local ASan/UBSan export runs pass. Independent review also exercises periodic-stream ambiguity with invalid/missing timeline positions; missing samples must never manufacture a unique delay.

Feature foundation commit `1c825d6` passed Windows, Linux, macOS 15 and sanitizer CI: [run 43132950806](https://github.com/ShaneKent/spdif_analyzer/actions/runs/43132950806). Subsequent audit fixes require their own CI and native application evidence before release; this foundation run does not prove those later changes.

The locale/report milestone `50ee4fb` also passed all four CI jobs: [run 43135185575](https://github.com/ShaneKent/spdif_analyzer/actions/runs/43135185575). Its native Logic 2.4.44 artifacts pass the strengthened offline verifier for PCM, status changes, AC3 and DTS I. PCM accounts for 9,814 source subframes as 4,715 written stereo frames plus 384 explicit exclusions. The changing-status fixture accounts for 13,086 subframes, with 5,971 stereo frames across 24/20/24-bit segments and 1,144 exclusions, plus four correctly masked status changes. AC3 yields three exact payload files; DTS I yields nine, with correct type, length, cadence, source intervals and bytes. Their WAV exports complete with every source subframe excluded and no audio files. Professional independent A/B produces no WAV without an explicit stereo assumption; that assumption yields 6,351 exact stereo frames. The partial-header fixture retains 771 subframes and one rejected candidate, with no payload/audio file. All six fixtures pass the full source-accounting verifier. The final reliability revision still requires a fresh DLL/table smoke and CI before release.

A fresh 100 MS/s D6/D7 recording was acquired after rechecking the real Logic 8 connection. Each input produced **12,834 subframes and 64 confirmed status records**, with **zero parity failures and zero synchronization losses**. Both identify as consumer 48 kHz PCM with unspecified effective precision. Explicit 24-bit precision was used for numerical analysis, without claiming the source has 24 effective bits. Comparison across a ±16-frame search reported ambiguous alignment, with equal-valued accepted data at multiple offsets; no unique hardware delay was inferred. Partial offline endings remained explicit.

The independent `SpdifTestSignal` module generates six native Logic fixtures: PCM ramp, status changes, AC3, DTS I, professional independent A/B PCM, and a Pa/Pb/Pc-only candidate. It has no production decoder/encoder dependency. The initial attempted demo shortcut left generic data in the capture; that recording was rejected as test evidence. The verified UI retake produced the expected transport waveform and confirmed AC3 with V=1 and parity intact in offline decoding. These transport payloads are intentionally synthetic, not codec-valid compressed audio.

`scripts/smoke_features.py` validates actual Logic exports against those saved waveforms. Its acceptance checks include four-dataset manifest counts, health count agreement and retained windows, independent PCM mean/RMS, exact stereo WAV bytes and valid precision, changed-status masks, byte-exact payload extraction, and rejection of partial/non-PCM/independent-channel audio without the required evidence or assumption. The output report is complete only when it says `PASS`; files written before then are intermediate evidence. Raw captures and reports remain local.

## Preserved v0.1 validation

## Automated verification

The independent portable suite passes **131,997 checks** in GCC Release, Windows MSVC Release, and address/undefined-behavior sanitizer builds. It covers timing recovery, both polarities, the supported rate matrix, exact waveform field boundaries, status completeness, parity, faults and recovery, and IEC 61937 transport identification. See [fixture methodology](../tests/README.md).

Local LeakSanitizer cannot run under the traced sandbox; local sanitizer tests used `detect_leaks=0`. This is not a successful local leak check. The CI sanitizer job runs without that override.

Cross-platform CI for implementation commit `4dcd05d` passed Windows, Linux, macOS 15 and sanitizer jobs: [run 43123434218](https://github.com/ShaneKent/spdif_analyzer/actions/runs/43123434218). This proves compilation and core tests on those platforms, not native Logic application loading on Linux/macOS.

The Windows target was also built locally with MSVC 14.29, Windows SDK 10.0.19041 and CMake 3.21. The DLL lifecycle harness passed dependency loading, three exported entry points, create/destroy, settings persistence, Fields/Samples and CSV selections, and settings archive v1/v2 migration.

## Logic 2 application verification

Tests used an isolated Logic **2.4.44** profile and simulated devices or saved captures, preserving the user's existing sessions. The plugin loaded and rendered waveform annotations. A saved demo capture reopened successfully; a fresh analyzer exported **16,357 subframes**, all checked against its expected signed stereo ramp, channel alternation, valid parity and V=0. Structured export contained **84** complete, integrity-good channel-status records declaring 48 kHz/24-bit. Exported structured intervals were monotonic and nonoverlapping. Removing analyzers/closing captures completed without a crash.

A standalone field-annotation unit experiment required host clock initialization and failed with the SDK's “SampleRate should never be 0” error before assertions ran. No private SDK state was fabricated. Field behavior is verified through actual Logic loading, native exports, visual inspection and independent core boundary fixtures instead.

## Real hardware

User-connected **D6 and D7** were captured using a Logic 8 at **100 MS/s**. Each stream decoded **16,042 subframes and 82 complete status records**, without parity or synchronization errors. Both declare **48 kHz consumer PCM**, with unspecified effective word length. Mean sampled transport-rate estimates were approximately 48,001.05 Hz; these are not precision clock-calibration measurements.

Logic field-mode exports preserved exactly 16,042 CSV rows for each stream. Visual inspection confirmed separate waveform labels for V/U/C/P, including `V=0 valid` and `P=0 parity OK`, and PCM labels once status was established. Raw source captures and screenshots remain local and are excluded from Git.

No real Dolby Digital or DTS source has yet been verified. Compressed-format tests use independent synthetic IEC 61937 transport fixtures, including error, truncation, conflict and format-change cases. Codec names identify the signaled transport type; payload decompression and codec CRC validation are outside scope.

## Performance and limits

The current counting-sink benchmark processed **60 seconds of 192 kHz traffic**: 1,025,280,036 edges, 23,039,999 subframes (one unclosed tail), and three expected EOF diagnostics in **150.205 seconds** (0.399454 capture seconds per wall second) in the Linux environment. This excludes Logic's result storage, GUI rendering and export overhead and is not a real-time claim.

Fields view creates seven legacy frames per subframe; Samples view uses one. Result publishing is batched with a flush before an edge wait. Metadata locks never enclose SDK calls. Results retained by Logic and metadata retained for export grow with capture length; the protocol decoder's acquisition and burst buffers are bounded.

The SDK offers no reliable capture-end callback during live analysis. Offline EOF handling is tested; a trailing unconfirmed block is not emitted as complete in Logic. No analog signal-quality assessment, precision jitter measurement, formal IEC certification, or native Linux/macOS application-loading claim is made.

The final DLL's four CSV choices passed actual Logic legacy-export checks. Fields and compact exports each contained 16,042 valid-parity subframes; status export contained 82 integrity-good records; content export reported PCM; the clean hardware diagnostic export was empty as expected. A deliberately insufficient 50 MS/s / manual 192 kHz configuration produced the expected capture_rate_low diagnostic CSV even without decoded subframes. Request times were approximately 55–70 seconds, separate from core throughput. The final local report is retained under ignored artifacts/final-smoke/report.json.
