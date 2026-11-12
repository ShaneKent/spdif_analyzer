# S/PDIF Analyzer for Saleae Logic 2

Native digital S/PDIF decoder for Windows x64, with a portable C++17 decoder core. Windows build, native loading, simulated waveform decoding, and exported sample verification have passed. See the validation record for remaining platform and hardware checks.

Version 0.2 retains BMC decoding, B/M/W preambles in either polarity, sample/field annotations, V/U/C/P flags, parity, channel status, and AC-3/DTS/E-AC-3 identification. It adds capture-health and PCM statistics, status changes, burst cadence checks, supported professional AES3 status, segmented PCM WAV, raw burst extraction, snapshot CSV bundles, structured offline policies, and two-stream comparison. Compressed-audio decompression remains outside scope.

## Build

Windows requires Visual Studio 2019 or newer with C++, a Windows SDK, CMake 3.21+, and Git. Tests and offline scripts require Python 3.8+ (or build with `-DSPDIF_BUILD_TESTS=OFF` when only the plugin is needed). In PowerShell:

```powershell
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The build fetches the Saleae Analyzer SDK at pinned commit `e3c4b4ffda1c162c024a5913520aed17c6edb238`. For an offline checkout pass `-DSPDIF_SDK_DIR=<path>`.

Core-only tests (GCC, Clang, or MSVC):

```sh
cmake -S . -B build-core -DSPDIF_BUILD_PLUGIN=OFF
cmake --build build-core
ctest --test-dir build-core --output-on-failure
```

## References and dependencies

- [Saleae Analyzer SDK](https://github.com/saleae/AnalyzerSDK) and [sample analyzer](https://github.com/saleae/SampleAnalyzer): native plugin interface and build guidance. SDK remains a separate build dependency; see its licensing information.
- [TI McASP guide](https://www.ti.com/lit/pdf/spruf91), pages 18–20: S/PDIF framing, preambles, parity and clock relationship.
- [Linux ALSA interface definitions](https://github.com/torvalds/linux/blob/master/include/sound/asoundef.h): cross-reference for consumer channel-status fields; implementation does not copy kernel code.

## Install and inspect

See [installation and capture requirements](docs/INSTALL.md), [result and CSV definitions](docs/RESULTS.md), [v0.2 features](docs/FEATURES.md), [offline tools](docs/OFFLINE.md), and the [validation record](docs/VALIDATION.md). Only digitized S/PDIF is supported; coaxial/optical receiver conditioning is external.

The native Windows DLL is `build-win/Analyzers/Release/SpdifAnalyzer.dll` when using `scripts/build_windows.ps1`. Package it with `python scripts/package.py`; the ignored `artifacts` directory receives a ZIP and SHA-256 checksum.

`benchmark_decoder` measures one minute of 192 kHz traffic with a counting sink. It is separate from ordinary CTest runs. Cross-platform build and sanitizer checks are configured in `.github/workflows/build.yml`.


## Waveform fields and content

The default **Fields** view annotates preamble, auxiliary bits, main audio field, validity V, user U, channel status C, and parity P separately at their actual decoded edge boundaries. **Waveform detail → Samples** selects a compact view. A parity failure is attached to the P field; V=1 is explicitly labeled invalid for conversion.

PCM identification uses confirmed consumer channel status. Compressed-format identification uses IEC 61937 synchronization, type and length information, corroborated by non-audio channel status. Headers without confirmed status are provisional; disagreement with PCM status is a conflict. Codec names describe the transport type, not successful decompression or codec payload validation. See [result definitions](docs/RESULTS.md).

For offline inspection of a Saleae digital transition CSV:

```sh
python scripts/csv_to_edges.py digital.csv --channel 6 --sample-rate 100000000 > edges.txt
build-core/decode_edges 100000000 < edges.txt
```

The sample rate must match the original capture. This utility prints status, content evidence and diagnostic counts without retaining every sample. Raw captures remain local.

To repeat the demo application check, configure this analyzer on D0 **before** starting a simulated capture, save the capture, install Saleae's `logic2-automation` package in a test environment, then run `python scripts/smoke_logic.py demo.sal --port 10430`. The API creates a new capture before analyzers are added; retake a demo capture in the configured tab so the plugin can supply its simulation waveform.


## Analysis and export selection

Keep **Export output → Selected CSV** for the original CSV workflow. Other choices provide health JSON, status-change JSONL, PCM-statistics JSONL, segmented WAV, IEC61937 payload files, or one bundle containing all four CSV datasets. **Analysis window (ms)** controls summary windows; **PCM bits override** resolves unspecified precision explicitly. WAV rate/stereo overrides are optional and never bypass content, parity, or validity checks.

New directory exports never overwrite an existing directory and remain marked `INCOMPLETE` after cancellation or I/O failure. Existing settings archives migrate with their original waveform/CSV choices preserved. Details and limitations are in [feature definitions](docs/FEATURES.md).
