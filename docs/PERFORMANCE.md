# Pipeline performance and bounded fuzzing

The portable benchmark measures the real decoder, analysis, codec inspector, WAV/burst writers, and CSV bundle writer. It does not model Saleae SDK storage, FrameV2 publishing, waveform rendering, USB acquisition, or Logic's export scheduler. Native Logic measurements must report Fields/Samples settings, capture duration/rate, process memory, and UI/export time separately before making claims about interactive performance.

## Reproduce a portable measurement

```sh
cmake -S . -B build-perf -DSPDIF_BUILD_PLUGIN=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-perf --target benchmark_pipeline --parallel
python3 scripts/benchmark_pipeline.py build-perf/benchmark_pipeline \
  --output artifacts/pipeline-run-001 --seconds .1 --repeats 3 \
  --long-seconds 30 --build-label Release
```

For a Windows multi-configuration build, use `--config Release` and the executable under `build-perf/Release`. Every output directory must be new. The runner writes an incremental `report.json`; only a final `result: PASS` represents a complete matrix. Fixtures and export files are removed after measurement by default; `--keep-fixtures --keep-outputs` retains them for byte-level inspection. The report records executable/fixture SHA-256, compiler, host, source clock, counts, result checksum, wall time, peak memory, and retained-history capacity. Record compiler options and machine/load conditions with the chosen build label. Do not compare Debug, sanitizer, and Release timings.

The default matrix has 48/96/192 kHz sources, three repetitions of analysis and export modes, and four deterministic scenarios:

| Scenario | Source conditions |
|---|---|
| `clean` | Signed PCM ramp, complete consumer PCM status, noninteger capture-to-half-clock ratios |
| `faults` | Bad parity, missing edges, extra short glitches, and a 2 ms signal gap |
| `transitions` | PCM to AC-3 to DTS, with a transport-rate change at a block boundary |
| `burst` | Non-audio status, V=1, complete IEC 61937 AC-3 bursts containing independent reference codec frames |

AC-3 and DTS reference payloads match the independently encoded fixtures in `test_codec.cpp`; codec callbacks and successful structure/CRC counts are reported separately. Unsupported DTS CRC coverage is not counted as passed. Source generation is independent BMC encoding with literal preamble patterns, not the decoder's private parser. This is a scaling workload, not a replacement for independent protocol conformance vectors.

The binary also exposes phases directly:

```sh
build-perf/benchmark_pipeline --generate /tmp/pcm.edges --rate 48000 --seconds .1 --scenario clean
build-perf/benchmark_pipeline --input /tmp/pcm.edges --mode analysis
build-perf/benchmark_pipeline --input /tmp/pcm.edges --mode exports --output /tmp/pcm-export-new
build-perf/benchmark_pipeline --online generator --rate 48000 --seconds 30 --scenario transitions
build-perf/benchmark_pipeline --online analysis --rate 48000 --seconds 30 --scenario transitions
```

Generation/file writing is timed separately. Input mode times file reading plus decoding/analysis, and optionally WAV/burst writing and retained CSV history. Bundle CSV writing is a second explicit phase. Online mode avoids a large edge file: its analysis time includes generation, decoding, analysis, and codec inspection. The generator-only run is a separate reference cost; subtracting two runs does not establish exact decoder CPU time.

Binary fixtures are capped at 512 MiB by default (`--max-fixture-bytes`); generation refuses the next write before exceeding the cap. A failed partial fixture has an invalid header and is rejected by the reader. The input cap also applies to reading. Durations are limited to 3600 seconds. At high edge densities, use online mode for long stress runs rather than increasing the fixture cap casually. `--capture-rate` permits an explicit capture clock; defaults are 100 MHz for sources through 96 kHz and 250 MHz for 192 kHz. At least four samples per half-cell are required.

Each export case additionally cancels its CSV bundle at row 100 and checks `complete: false` plus the `INCOMPLETE` marker. The WAV/burst phases precede bundle cancellation; this case measures bundle cancellation, not cancellation of every writer. Existing portable export tests cover writer failure and incomplete-payload behavior.

## Measured baseline (2026-11-11)

GCC 13.3, `-O2`, Linux x86-64 under WSL2 6.18.58.1, eight visible CPUs. The host was shared; measurements are advisory. A complete optimized matrix contained 86 successful runs: 72 repeated matrix cases, 12 bundle cancellations, and two 30-second online cases. Repeated counts/checksums were identical; analysis and export modes agreed. Filesystem location matters: the following matrix used Linux `/tmp`.

| Clean source | Edges / subframes | Analysis + input median | Export pipeline + bundle median | Export peak RSS | Retained subframe capacity |
|---|---:|---:|---:|---:|---:|
| 48 kHz, 0.1 s | 427,097 / 9,600 | 0.168 s | 0.235 s | 6.26 MiB | 2 MiB |
| 96 kHz, 0.1 s | 854,367 / 19,200 | 0.287 s | 0.421 s | 8.32 MiB | 4 MiB |
| 192 kHz, 0.1 s | 1,709,605 / 38,400 | 0.719 s | 1.123 s | 12.24 MiB | 8 MiB |

The 30-second online transition run processed 174,861,185 edges, 4,800,000 subframes, 24,996 status blocks, and 2,500 codec frames in 35.46 seconds with a 4.09 MiB peak RSS and no retained subframe history or edge file. Its generator-only companion took 4.47 seconds. These are measured processing costs, not a real-time guarantee. Dense corruption and rate/content changes are represented in the matrix; a 30-second continuous adversarial-noise memory bound is not established by this run.

The measured CSV issue was repeated `ostream::imbue` for every sample row. Moving it to stream setup preserved byte-identical output. On the same Windows-mounted workspace filesystem, three-repetition 48 kHz clean bundle time fell from **10.668 s to 0.621 s** (about 17.2x for that phase), with the same 9,600 subframes, checksum, and output size. A focused 1,000-row control produced identical SHA-256 with per-row locale setup versus one setup. This finding applies to that measured CSV phase/filesystem; it is not a claim of a 17x decoder or Logic speedup. The pre-change full matrix also completed 86 runs. Native Logic confirmation remains a separate measurement.

Local raw reports are ignored artifacts: `artifacts/pipeline-baseline-v1/report.json`, `artifacts/pipeline-optimized-hostfs/report.json`, and `/tmp/spdif-pipeline-optimized-matrix/report.json`. These paths are evidence locations for this session, not files distributed in a release.

Linux memory uses `/proc/self/status` `VmHWM`; Windows uses `PeakWorkingSetSize`; macOS/fallback uses `getrusage`. The method is recorded. In this environment `ru_maxrss` inherited the Python launcher's pre-exec high-water floor, so earlier pre-change memory numbers must not be treated as a measured memory improvement. Retained object capacity reports vector object storage, not allocator overhead, pointed-to string allocations, SDK storage, or the whole process RSS. Export mode intentionally retains subframes for a single immutable CSV cursor; online analysis mode does not. Memory growth in the export mode is therefore expected and disclosed.

## Native Logic measurements and table density

Windows x64, Logic 2.4.48, MSVC Release, the same saved physical D6 AC-3 capture at 100 MS/s: 25,668 subframes over approximately 0.267 seconds. Each analyzer was removed after export. The RPC includes native analysis and Logic database ingestion; it is not isolated decoder CPU time.

| Table setting | Waveform detail | Add-analyzer RPC | CSV export request | CSV rows |
|---|---|---:|---:|---:|
| Full structured sample rows | Samples | 85.807 s | 0.697 s | 25,668 |
| Full structured sample rows | Fields | 86.010 s | 0.498 s | 25,668 |
| Sparse structured table | Samples | 2.521 s | 0.494 s | 25,668 |
| Sparse structured table | Fields | 2.344 s | 0.610 s | 25,668 |

All four CSV files have SHA-256 `15ed0e8285e03507952f989332c5837f7ecfb8fc5fad66ede8aba31c45e15ba4` and 3,458,340 bytes. The explicit sparse choice reduces per-sample data-table storage; it retains every legacy waveform frame and CSV sample, plus sparse codec/status/diagnostic/analysis records. Full structured sample rows remain enabled by default. These are single runs on a shared host, not a statistical speedup guarantee. A separate pre-locale-change run took 91.882/87.273 seconds to add the analyzer and approximately 0.497/0.498 seconds to export. The locale change alone did not demonstrate a native improvement.

Logic's launch PID does not account for its backend child. A read-only snapshot during a full-table run found approximately 1.585 GB working set and 2.053 GB private usage counter in the busy backend, while the launch process used approximately 149 MB. Those are point observations, not concurrent tree peaks. Early local probe reports are explicitly labeled launch-PID-only and must not be read as total Logic memory. Backend log database-completion timestamps match the long add-analyzer interval. Full-table long recordings can therefore consume substantial time and memory even when the portable decoder is fast.

### Native rate, fault and longer-recording matrix

Independent QA recordings at 250 MS/s supplied approximately 0.134 seconds of PCM at each physical audio rate. Both modes used the explicit sparse table and a 10 ms analysis window. Actual decoded stereo ramps, parity, rates, source intervals and mode-identical CSV hashes were checked; add-analyzer and export requests were timed separately.

| Source | Subframes | Samples setup | Fields setup | Samples / Fields CSV export |
|---|---:|---:|---:|---:|
| 48 kHz | 12,884 | 1.987 s | 1.843 s | 0.306 / 0.308 s |
| 96 kHz | 25,769 | 2.789 s | 3.090 s | 0.484 / 0.599 s |
| 192 kHz | 51,539 | 4.891 s | 4.894 s | 0.797 / 0.799 s |

The reused process tree was warm: sampled concurrent private usage across this matrix ranged roughly 1.5–2.0 GiB, including retained heaps and caches from previous cases. These values are not per-case allocation deltas or decoder memory. Input loading, table export, removal and capture close are separate fields in the local measurement reports. A 250 ms sampler can miss shorter peaks.

Four additional native fixture pairs exercised literal professional CRCC examples (82 passed records per mode), CRCC-only faults (34 passed, 16 failed, 16 CRC diagnostics and zero parity failures), gaps plus parity faults (7 synchronization losses and 7 parity failures), and 48↔96 kHz changes with reacquisition. All 14 cases passed, with exact paired CSV hashes. Content transitions additionally have real-source evidence in [HARDWARE.md](HARDWARE.md).

A requested five-second demo fixture contained an unexpected quiet tail: raw export spanned 5.066719232 s but its last transition was at 2.68435424 s. It was rejected as evidence of five seconds of continuous data. The cutoff cause remains unknown; no Saleae limit or decoder defect is inferred. The native long-recording measurement instead uses a fresh passive physical D6/D7 capture, after confirming 48 kHz PCM and preserving source settings and the user's original Logic session.

The physical recording contains 5.04692736 s of continuous transitions at 100 MS/s. Both waveform modes decoded **484,515 subframes** over 5.04692129 s with zero parity errors, mean transport rate near 48 kHz, and identical CSV SHA-256 `d77d67b538d16777aa4c223657233393a2f10bd47cfabc5975c5d69dba61f5ee`. Samples setup/export took 34.340/9.298 s; Fields took 32.167/8.299 s. Sampled concurrent process-tree private peaks were 1,425,911,808/1,571,319,808 bytes respectively; working-set sums were 1,133,539,328/1,322,991,616 bytes. Export, table output and cleanup are included in the separate 44.670/41.603 s measured intervals; signal generation/acquisition is excluded.

Those runs used a sampled 4 GiB tree-private limit, 2 GiB minimum available system RAM and a 120 s per-stage wall budget. No guard fired. The guard requests closure of only its owned capture; it is an advisory sampled stop condition, not an operating-system memory quota. Full dense tables were not extrapolated into this long run because their measured short-recording ingestion cost was already substantial. Full-table and sparse-table results are labelled separately.

### Native export interruption

The five-second physical recording also exercised an interrupted native CSV bundle export. Closing only its owned capture after export began left `INCOMPLETE`, `complete:false` and `state:cancelled`, with 7,693 of 484,515 sample rows written. The interrupted file is a byte-exact prefix of the complete export. The backend log records the native export thread ending 193 ms after it started, 69 ms after capture deletion. A fresh analyzer then decoded and exported the independent 48 kHz fixture successfully with its expected CSV hash, establishing continued plugin use.

The synchronous legacy-export API request remained pending for more than 60 seconds after that capture-close operation. The helper timed out; this is not a clean API-completion result. File-generation cancellation and usable subsequent analysis are established separately from that application/API limitation. Keep a capture open during ordinary synchronous automation exports; cancellation must be assessed from the explicit manifest/marker state rather than a client request disappearing. A cancelled gRPC future alone would not prove that native work stopped.

## Regression policy

Use `--compare previous/report.json` for an advisory comparison. Add `--enforce` only on a controlled host with the same build label, capture settings, filesystem device, and at least three repetitions. The executable hash may differ because a code change is being measured. Output checksum and subframe counts must match before speed is compared. A changed result is a correctness failure, not an optimization.

The default slowdown threshold is the old median plus the greatest of 10 ms, 30% of the old median, or six median absolute deviations from either run. The matrix's noisiest group had about 15% relative MAD on this shared host, which motivates a conservative threshold instead of an arbitrary tight CI timeout. Memory regressions require the same measurement method and growth above 25% plus 16 MiB. The runner does not enforce missing/new workload groups. Short smoke runs and shared CI hosts are useful for detecting gross regressions, not for establishing a release performance SLA.

## Bounded sanitizer fuzzing

The dedicated targets compile production sources with Clang libFuzzer, ASan, and UBSan; ordinary release libraries do not receive fuzzer instrumentation. See [LLVM's libFuzzer documentation](https://llvm.org/docs/LibFuzzer.html) for corpus mutation, resource flags, and crash minimization.

```sh
cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ \
  -DSPDIF_BUILD_PLUGIN=OFF -DSPDIF_BUILD_TESTS=OFF \
  -DSPDIF_BUILD_FUZZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-fuzz --parallel
mkdir -p /tmp/spdif-fuzz-artifacts
for target in edges input codec; do
  limit=4096
  if [ "$target" = codec ]; then limit=24569; fi
  cp -r "tests/fuzz/$target" "/tmp/spdif-corpus-$target-new"
  "build-fuzz/fuzz_$target" "/tmp/spdif-corpus-$target-new" \
    -seed=1 -max_len="$limit" -timeout=2 -rss_limit_mb=512 -max_total_time=10 \
    -artifact_prefix=/tmp/spdif-fuzz-artifacts/
done
```

The loop uses 4096 bytes for edges/input. For `fuzz_codec` and `tests/fuzz/codec`, it uses `-max_len=24569`: one control byte plus the public 24,568-byte payload bound. Always copy committed seeds to a new temporary mutation corpus. Do not let the fuzzer rewrite the tracked seed corpus.

| Target | Inputs and checked invariants |
|---|---|
| `fuzz_edges` | Bounded intervals, intentional duplicate timestamps, valid/faulty structured BMC; deterministic replay, exact raw parity/sign, monotonic frame fields, bounded callback count and burst lifecycle |
| `fuzz_input` | Strict decimal parser versus an independent digit/overflow oracle; whitespace tokenization, round trips, signs and 64-bit boundaries |
| `fuzz_codec` | All expected codec hints, malformed/truncated frames and partial valid bytes; deterministic complete metadata, input/consumed extents, public pointer/size rejection |

Initial local 10-second, seed-1 mutation runs completed without crashes: edges 155 executions (75 MiB peak), input 12,846 (90 MiB), codec 161,970 (173 MiB). All final committed seeds, including maximum-size boundary seeds added afterwards, replayed successfully. This is bounded smoke evidence, not exhaustive coverage. LeakSanitizer was disabled locally because the traced sandbox does not support its process inspection; it is not evidence of a passing leak check. Longer scheduled/manual runs should increase `-max_total_time` with the same input/RSS limits and preserve logs.

On failure, retain the emitted artifact and sanitizer log, replay it directly against the same instrumented executable, then minimize with `-minimize_crash=1 -exact_artifact_path=/tmp/minimized ...`. Add the minimized reproducer under `tests/fuzz/regressions/<target>-<description>` and replay it explicitly in that target's regression run; include a deterministic unit test when the expected protocol behavior is known. No invented crash seeds are recorded as regressions. Timeouts, allocation-limit failures, invariant failures, and sanitizer failures are test failures to investigate, not accepted parser rejections.

## Windows process-tree sampler

Use the read-only standard-library sampler while an isolated Logic test runs:

```powershell
python scripts/native_metrics.py --pid <isolated-Logic-PID> --seconds 30 --interval .25 --output artifacts/native-metrics-new.json
```

It identifies processes by PID and creation time, validates parent lifetime intervals, and reports sampled concurrent descendant working-set/private-usage sums and CPU deltas. Working-set sums include shared pages and are not unique physical memory. Pre-existing children start from their observed CPU baseline; newly born children can be counted from birth. An exited observed parent remains a lineage anchor. Root disappearance/reuse or query errors fail the report explicitly. The sampler never starts, stops or connects to an application.

Sampling can miss a short-lived process entirely. A live descendant also cannot be linked through an intermediate parent that exited before ever being observed. Sub-interval memory peaks are not measured. The 4,096-identity budget and bounded error list prevent indefinite tracking growth. These are disclosed measurement limits, not proof of constant application memory.
