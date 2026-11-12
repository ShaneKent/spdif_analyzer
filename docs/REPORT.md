# Standalone capture reports

Run the report generator with Python 3.8 or newer. It uses only the standard library and creates one HTML file containing its data, styles and interaction code. Open that file in a modern desktop browser; it does not upload captures, load a CDN, or require a web server.

```sh
python scripts/capture_report.py decoded.jsonl -o capture.html
python scripts/capture_report.py analysis.bundle -o capture.html \
  --health health.json --pcm pcm.jsonl --status-changes changes.jsonl \
  --comparison comparison.json
```

The primary input is either the offline decoder's version-1 JSONL (`decode_edges ... --jsonl`) or the analyzer's four-CSV bundle directory. Optional inputs are the existing capture-health JSON object, PCM-statistics and status-change JSONL exports, and a version-1 PCM comparison report. These files should describe the same capture represented by the primary input; comparison source A supplies the reference axis. No extension settings or original exports are modified.

The view combines format/status evidence, faults, health windows, PCM statistics, IEC bursts, codec-frame evidence and relative-delay windows on one timeline. Drag to pan, scroll to zoom, or use the range controls. Event-type switches filter the plot and table. Search examines all retained sparse event fields. The table has 100 rows per page; filtering and pagination do not remove records from the report. Enable **Table follows visible range** to limit the table to the current viewport.

Select a row or a plotted event to inspect its full record. Exact source capture indices remain decimal strings, including values above JavaScript's 53-bit integer limit. Use those source timestamps to locate the corresponding position manually in Logic. The report does not assume automatic control of Logic navigation.

## Time and evidence

All plotted offsets share an explicitly displayed base. The generator subtracts that base using Decimal arithmetic before converting small offsets to plotting floats, even when a capture has large absolute sample indices and metadata origin zero. The table retains original source indices and exact exported decimal times. A plotted bar covers the full source interval; its position does not imply a more precise codec-bit or midword timestamp.

Source `coverage_start_sample`/`coverage_end_sample` fields take precedence over a notification's publication interval. Both remain in the selected raw record. Codec frames with unknown source coverage use their transport interval and are labeled accordingly. Unknown intervals are retained in the table rather than placed at a fabricated time. Inclusive source ends and exclusive analysis ends are distinguished in field names; the renderer uses an exclusive plotted end internally.

CSV times remain the original trigger-relative decimal strings. The report does **not** reconstruct exact sample indices from rounded CSV times. A bundle's explicit `settings.trigger_sample` lets optional sample-index analysis share the CSV axis. Without it, the bundle can be viewed alone, but merging sample-index analysis fails explicitly. Input sample-rate mismatches are rejected.

Comparison windows use source A's declared origin. Numeric source origins are translated to the primary origin when capture-rate metadata agrees. CSV comparison origins use the same trigger-relative seconds convention. When provenance is insufficient to map an origin, comparison records remain available in the table and are not plotted on an invented shared axis. Aligning declared origins does not synchronize independent capture clocks. Ambiguous windows, holes, delay-change candidates and uncalibrated drift remain explicit. A global comparison without interval information appears in the event table and comparison details.

Unknown, provisional, non-PCM, bad-parity, invalid-V and unknown-precision samples are excluded from the PCM amplitude plot and counted by reason. The 24-bit sample container is reduced using the declared effective width; an explicit producer metadata precision override is honored only for unknown widths. This is a numeric sample view, not audio decompression or an independent authentication of the producer's classification. DTS or Dolby transport identification does not imply that codec CRCs passed. The full codec and transport checks remain separate in each record.

Unobserved capture gaps and declared uncovered boundaries are visible as unknown coverage. A declared capture interval containing no decoded subframes appears as one unknown interval; an empty timeline does not imply valid audio. Live snapshots and incomplete bundles carry warnings. Exported cumulative health/PCM totals remain authoritative records in **Counts, input provenance and authoritative exported totals**; this page does not silently recompute them from reduced plot bins.

## Bounds and totals

Default limits are:

| Limit | Default |
|---|---:|
| Total input bytes | 128 MiB |
| Input analysis records | 2,000,000 |
| Retained sparse events | 100,000 |
| Dense plot bins per channel | 2,048 |
| Output HTML bytes | 64 MiB |

Use `--max-input-bytes`, `--max-records`, `--max-events`, `--max-plot-bins` and `--max-output-bytes` to set explicit budgets. `--help` describes the allowed ranges. Bounds are finite; an exceeded budget exits with an error instead of discarding sparse records. Large sparse tables are paginated, not downsampled.

The input-record budget is shared by the primary input and every companion. Each comparison container, window, delay-change candidate and drift segment consumes a record, as do nested exported health windows. Integer fields used for interpretation (payloads, precision, channel and sample/window indices) reject fractional values and booleans instead of silently rounding them. Decimal integer strings and hexadecimal raw payload strings remain supported.

Dense subframes use streaming adjacent min/max bins. Every bin keeps its original observation count, accepted count, excluded-state counts and extrema. Reduction preserves peaks and totals, but individual dense samples and transition timing within a bin cannot be recovered from this HTML. A bin can contain gaps or mixed eligibility, so its span is not proof of continuous valid audio. Keep the original export for detailed sample comparisons.

Primary summary subframe/status totals must match the loaded detailed records. Bundle row counts must match their versioned manifest, including snapshot/written counts. Additional optional analysis files are counted separately as supplied; supplying the same analysis twice produces duplicate evidence rather than deduplicated totals. Derived coverage-gap events are counted in the retained event table and are distinguishable from input records.

Input and output errors exit **2**; successful generation exits **0**. A damaged capture can produce a successful report: this command reports evidence and does not impose a capture pass/fail policy. Existing decoder/comparator policies remain available for that purpose. Output replacement is atomic, and errors preserve an existing destination file. The destination may not overwrite an input or a bundle file.

## Validation and limitations

Run `python tests/test_report.py`. Independent fixtures cover PCM extrema, mixed/non-PCM/unknown/damaged evidence, row/summary consistency, actual bundle and optional export shapes, both comparison modes, origin translation, uint64-sized sample indices, truncated/malformed inputs, HTML/script injection and explicit byte/record/event/output budgets. Python/JSON and generated JavaScript syntax checks do not substitute for browser interaction tests; release validation should also exercise zoom, pan, filters, pagination and exact event inspection in a real browser.

Text enters the page through `textContent`. Embedded JSON escapes HTML parser delimiters, including `</script>`, and a restrictive content-security policy disables external connections. Source data remains untrusted evidence; this report does not decode audio, calibrate jitter, infer missing codec data, recover lost capture precision or automatically navigate Logic.
