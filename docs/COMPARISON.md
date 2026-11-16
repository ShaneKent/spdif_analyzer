# Compare compressed transport between two paths

`scripts/compare_compressed.py` compares received IEC 61937 payload bits and raw Pc/Pd headers. It helps answer whether D6 and D7 retained the same compressed words, where a payload changed, and which observations cannot be aligned safely. It does not decode audio, synchronize clocks, resample data, override channel-status declarations, or identify which device caused a difference.

Python 3.8 or later and the standard library are sufficient. The existing PCM comparator remains separate.

## Inputs and commands

Use the full version-1 JSONL emitted by `decode_edges --jsonl`, including metadata, raw subframes, content events, status blocks and diagnostics. Produce one file per observed path, retaining its actual capture rate and exact source indices:

```sh
build-core/decode_edges 100000000 --jsonl < D6-edges.txt > D6.jsonl
build-core/decode_edges 100000000 --jsonl < D7-edges.txt > D7.jsonl
python scripts/compare_compressed.py D6.jsonl D7.jsonl --output comparison.json
```

Raw burst files, codec-only JSONL, CSV and audio files are not inputs. In particular, rejected/conflicting D7 candidates may have no accepted burst file. Comparing only extracted files would omit those observations. A missing closing summary is reported as incomplete input evidence; it is not an invented EOF. An upstream `fatal_error` is an input error. A completed producer's failed analysis policy is retained separately and does not prevent byte comparison.

The comparator consumes the producer's existing burst candidate events. It does **not** scan payloads for new Pa/Pb sequences. Header records are checked against literal `F872 / 4E1F / Pc / Pd`, received in A/B/A/B order with contiguous source intervals. Payload words come from the upper 16 raw payload bits. Parity, gaps and alternating channels are checked independently; V=1 is retained and does not automatically mean corrupt compressed transport.

Supported extents are AC3 and DTS types I/II/III with Pd in bits, plus EAC3 with Pd in bytes. Zero/oversized lengths, unknown/null/pause types, partial headers, incomplete payloads, contradictory metadata and overlapping candidate intervals remain excluded from complete-payload matching. No unsupported length or missing word is guessed. Partial final words compare only their declared high bits; unused low bits are zeroed. Received big-endian transport-word order is preserved, including the final two-byte word.

## Conservative correspondence

The default finds payload fingerprints occurring exactly once in each selected input, confirms their actual bytes, and uses them only when their order agrees. Type, stream number and valid bit count participate in identity. Fingerprints are indexes, not substitutes for byte equality.

Unique ordered anchors establish individual exact matches. Between two such anchors, a single compatible candidate on each side can be compared byte for byte. A one-sided interior run can be reported as observed only on A or B. Those observations do **not** establish transmission loss or device fault. Missing/extra candidate counts and source coordinates remain visible.

Repeated payloads, crossing anchors, multi-candidate unmatched runs, capture edges, incomplete candidates and discontinuities can leave correspondence unresolved. The tool never silently zips repeated runs or uses a gap as justification to bridge missing evidence. It can therefore report uncertainty even when two files appear similar.

When correspondence is independently known, an explicit burst-index offset is available:

```sh
python scripts/compare_compressed.py D6.jsonl D7.jsonl --offset-bursts 0 --output mapped.json
```

Positive N asserts `B[i + N]` corresponds to `A[i]`. This is a user assertion, not a measured delay. Partial and unsupported candidates keep their ordinal positions. Unpaired edges, partials and source gaps remain reported and prevent an exact overall result. Explicit mapping does not establish clock synchronization or validate payloads.

## Evidence and results

The JSON report has `schema_version: 1` and `event: compressed_comparison`. Source sample indices and origins are exact decimal strings, including values above 2^53; capture rates are preserved as decimal strings. Source intervals remain inclusive. Numeric byte offsets are zero based within the big-endian word payload; they are not invented exact capture-sample positions inside a word.

Each candidate retains producer classification, completion/integrity flags, raw Pc/Pd, declared/observed bits, exclusions, source epoch and latest prior completed status blocks. Observed C-block differences are distinct from payload equality. The compared blocks are not assumed synchronized, and a C-byte difference does not establish causality or prove that an old declaration remains current. Blocks spanning a source discontinuity cannot supply the attached status baseline.

D7 can therefore have identical received payload bits while retaining **PCM status / IEC 61937 header conflict**. Equality never promotes that candidate to trusted audio or a validated codec body. DTS header/extent checking and unsupported codec CRC remain separate from this tool's byte comparison.

Pairs report payload/header equality, exact differing-byte totals, first byte offset and a bounded list of differing values. Complete payload equality is `null` when either candidate is incomplete, unsupported or damaged. Available partial words can be compared separately as `observed_prefix_only`; equal observed prefixes do not establish equal complete payloads. Hashes of complete payloads and hashes of observed words are labeled separately.

- Exit **0**: all candidates are covered by justified correspondence; complete, integrity-good received payload bits and Pc/Pd match; both producer summaries exist; no source discontinuity remains. Status differences alone do not fail this payload comparison and remain explicit.
- Exit **1**: differences, unmatched observations, ambiguity, partial/unsupported/damaged candidates, source gaps, missing producer summaries or no comparable payloads.
- Exit **2**: malformed/incompatible input, exceeded resource budget, or I/O failure.

Reports written with `--output` must have a new filename. Existing files are never overwritten. Without this option the report goes to stdout. Output-budget failure occurs before a report is written.

## Bounds and verification

Defaults are 512 MiB per input, 1 MiB per line, two million lines, one million subframes, 10,000 candidates, 20,000 status blocks, 64 MiB observed payload per input, 20 million shared work units, 16 MiB output and 64 detailed differing bytes per pair. All are finite CLI options shown by `--help`; reaching a limit fails explicitly rather than omitting observations. Work counts parsing, source continuity, candidate resolution and payload/matching operations; it is not a wall-clock deadline. Input records are retained under these limits so content callbacks arriving before their associated subframe can be resolved correctly.

```sh
python tests/test_compare_compressed.py
```

Tests use hand-checked decoded-word vectors independent of the C++ encoder/decoder. They cover native callback ordering, D6/D7 status conflict, supported units, partial high bits, changed bytes, missing observations, repeated/crossing anchors, source gaps, partial slots, hostile input, exact large timestamps and explicit budgets. Codec decompression and physical unplug behavior are outside this comparison's scope.
