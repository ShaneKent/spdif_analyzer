# Validation record — 2026-11-11

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
