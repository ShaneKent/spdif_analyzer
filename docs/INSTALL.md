# Installation and signal requirements

Build Windows x64 Release using the README commands. Logic must be 2.3.43 or newer; the primary validation target is 2.4.44.

In Logic, open **Edit → Settings**, find **Custom Low Level Analyzers**, and select the folder containing `SpdifAnalyzer.dll` (normally `build-win/Analyzers/Release`). Restart Logic, add **S/PDIF Analyzer**, and select the digital input channel. Keep existing custom plugins accessible: either use an additional custom path if supported, or copy the release DLL alongside existing plugins after closing Logic. Do not overwrite unrelated plugins.

The Saleae SDK runtime comes with Logic. Do not replace Logic's own Analyzer.dll. A standalone lifecycle test needs the SDK's matching Analyzer.dll next to its executable or in its DLL search path.

## Capturing

Use the digital output of an optical receiver or a suitably conditioned coaxial receiver. A digital logic input cannot directly receive optical light. Check your receiver's logic voltage against your instrument's input specifications.

S/PDIF has 128 half-cells per stereo sample. Capture at least four samples per half-cell; eight or more is recommended. At 48 kHz this means at least 24.576 MS/s (recommended 49.152 MS/s); at 192 kHz at least 98.304 MS/s (recommended 196.608 MS/s). Choose a supported instrument rate above these figures.

Automatic timing recovery is polarity independent. The manual rate setting is the stereo sample rate, not the BMC clock rate. A wrong manual rate or insufficient capture resolution can prevent acquisition.

## Interpretation limits

The signed sample value represents the 24-bit numeric container. It does not establish effective resolution or prove PCM. Validity, parity, supported channel status and structural integrity must be considered together. Non-audio payloads remain raw data; this release identifies supported IEC 61937 transport formats but does not decompress their audio payloads.

Blocks begin at B and contain 192 A/B frame pairs. Status and user bits are collected separately for both channels. A capture starting mid-block cannot produce that block's complete status. The final block needs a confirming next B; an incomplete ending is not a complete block.

Saleae's live edge API provides no reliable capture-end callback. The offline core supports explicit end-of-input and reports partial endings. The plugin may leave a final incomplete structure unreported until more transitions arrive.

Digital timing analysis describes the sampled waveform. It does not measure analog signal quality or precision source jitter.

The default Fields waveform detail splits each subframe into seven annotations. Select Samples for lower result-storage overhead. Both choices produce one row per subframe in the sample CSV export.

## Exports and troubleshooting

Choose the desired **CSV export** format in the analyzer settings, then use its generic **Export to TXT/CSV** action. The automation API's legacy export also uses this setting. Changing the export selection may rerun the analyzer; the capture's digital waveform remains intact.

Keep **Export output → Selected CSV** for that workflow. Other choices export health JSON (including completed health windows), status-change JSONL, PCM-statistics JSONL, codec-frame JSONL, PCM WAV, IEC61937 payloads, or an analysis CSV bundle. Standalone reports include input, capture rate, frozen settings and coverage. JSONL reports start with metadata. Health reports and current PCM windows explicitly identify live snapshots; they are not claims of a known capture end.

**Structured sample rows** is enabled by default. Disable it for a sparse data table containing status, diagnostics, codec and analysis notifications. Both choices retain all decoded waveform annotations and legacy subframe CSV rows; this setting affects only the per-subframe structured table records. Frozen export settings record the choice. Older saved analyzer settings migrate with full sample rows enabled.

**Analysis window (ms)** controls health and PCM windows. **PCM bits override** defaults to zero (use the declared effective precision); choose 16–24 only to resolve unspecified precision explicitly. It never overrides known precision or bypasses PCM, validity or parity gates. WAV additionally offers **WAV rate override (Hz)** and **Assume A/B stereo**. The latter is needed when interpreting professional independent channels as a stereo pair. Neither option resamples or decodes compressed audio.

Directory exports append `.wav-export`, `.bursts`, or `.bundle` to the requested filename. Choose a new destination for each request: existing directories are rejected. Read the index/manifest and check that `INCOMPLETE` is absent. A failure also creates a separate `<requested-filename>.error.json`; the SDK has no dedicated export-error setter. See [feature and format definitions](FEATURES.md) before treating exported files as audio.

The native lifecycle harness is `build-win/Release/smoke_windows.exe`; it expects the plugin path as its argument and the SDK runtime in its executable directory or DLL search path. It verifies loading and settings, while field annotations require Logic's host initialization.

If automatic acquisition produces no samples, inspect the selected channel, digitized input levels, sample rate and manual-rate override. A capture beginning inside a block can produce valid subframes before complete channel status is available. Format labels remain Unknown until supported evidence is established.

Logic automation exports took roughly 55–70 seconds per request in this environment, even for short recordings. Keep the same request running while it progresses; repeated exports create more work. This observed application/API latency is separate from the decoder-core throughput reported in the validation record.

Separately timed v0.2 fixture checks located most of that delay in `add_analyzer` (36–68 seconds in these observations); completed bundle/report/file exports then took approximately 0.1–0.6 seconds for those short captures. This does not promise the same timings for other captures or eliminate host setup latency.

Version 0.3 measurements confirmed substantial structured-result ingestion costs in Logic's backend. Disabling **Structured sample rows** reduced setup for the same 25,668-subframe physical recording from approximately 86–92 seconds to 2.3–2.5 seconds, with byte-identical sample CSV. This is an explicit table-output tradeoff, not a change to the default or a decoder throughput guarantee. See [performance methods and limits](PERFORMANCE.md).

## Reproducing the feature integration checks

The default test-enabled SDK build also produces `SpdifTestSignal` in `build-win/TestAnalyzers/Release`. This independent QA module is never installed or included in the production ZIP. In a separate Logic profile, add both this directory and the production analyzer directory to custom analyzer paths, restart, and enable automation on a separate port (the harness defaults to 10431).

On a simulated device, enable D0 at 50 MS/s. Add **S/PDIF Test Signal** on D0, choose a pattern, then use the UI capture button to retake a demo recording with that analyzer configured. Save recordings of at least 100 ms as `pcm.sal`, `ac3.sal`, `dts.sal`, `changes.sal`, `professional.sal`, and `partial.sal`, matching the six pattern names in order. The partial-header pattern intentionally holds the line after Pc. Adding a source analyzer after a demo capture does not replace its existing waveform; verify the retaken data rather than assuming a keyboard shortcut succeeded.

The QA source also supports 48/96/192 kHz physical audio rates, literal professional CRCC examples, a CRCC-only fault transition, gaps/parity faults, and rate changes. Use a sufficiently high simulated capture rate (250 MS/s covers all three rates). These extra fixtures are test infrastructure and remain excluded from production packages.

With `logic2-automation` installed, run:

```powershell
python scripts/smoke_features.py <fixture-directory> --port 10431 --output artifacts/native-feature-check
```

The output directory must not already exist. The script opens and closes new capture tabs, acquires no hardware, and verifies the exports against the known fixture behavior. `--only pcm changes` selects a subset for diagnosis. Synthetic AC3/DTS payloads exercise transport identification and extraction; they are not codec-valid compressed audio.

`--resume` reuses complete exports and refuses to overwrite incomplete or failed ones. `--verify-only` rechecks existing artifacts without importing the automation package or opening Logic; its separate `verification-report.json` must say `PASS`. The checks account for every source subframe as either exported PCM or an explicit exclusion and compare declared audio precision rather than treating auxiliary bits as audio.
