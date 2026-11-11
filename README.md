# S/PDIF Analyzer for Saleae Logic 2

Native digital S/PDIF decoder for Windows x64, with a portable C++17 decoder core. Development is in progress; release validation will be documented before completion.

The first release targets automatic BMC clock recovery, B/M/W preambles in either polarity, raw and signed sample containers, V/U/C/P flags, parity and stream diagnostics, separate channel-status blocks, and CSV exports. WAV and IEC 61937 decoding are outside this release.

## Build

Windows requires Visual Studio 2019 or newer with C++, a Windows SDK, CMake 3.21+, and Git. In PowerShell:

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
