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

The native lifecycle harness is `build-win/Release/smoke_windows.exe`; it expects the plugin path as its argument and the SDK runtime in its executable directory or DLL search path. It verifies loading and settings, while field annotations require Logic's host initialization.

If automatic acquisition produces no samples, inspect the selected channel, digitized input levels, sample rate and manual-rate override. A capture beginning inside a block can produce valid subframes before complete channel status is available. Format labels remain Unknown until supported evidence is established.

Logic automation exports took roughly 55–70 seconds per request in this environment, even for short recordings. Keep the same request running while it progresses; repeated exports create more work. This observed application/API latency is separate from the decoder-core throughput reported in the validation record.
