# Offline analysis and automated checks

Build `decode_edges` with the normal CMake build. Its input is one strictly increasing unsigned capture-sample edge index per token. For a Saleae digital CSV, preserve the actual capture rate:

```sh
python scripts/csv_to_edges.py digital.csv --channel 6 --sample-rate 100000000 > edges.txt
build-core/decode_edges 100000000 < edges.txt
build-core/decode_edges 100000000 --jsonl --fail-on parity,sync_loss,content_conflict --expect-content pcm < edges.txt > report.jsonl
```

The original text mode remains available. `--jsonl` produces one JSON object per line, beginning with metadata and ending with a summary on completed analysis. Each record has `schema_version: 1`. Types include `subframe`, `channel_status`, `content`, `diagnostic`, `health_window`, `status_change`, and `pcm_window`. Fatal input/I/O errors are reported separately as `fatal_error`; stderr remains a diagnostic stream. Sample indices are decimal **strings**, preserving values above the JSON/JavaScript exact-integer limit.

Subframe/block ends are named `_inclusive`; analysis/report interval ends are named `_exclusive`. `field_boundaries` includes the exclusive final boundary. Window boundaries identify analysis buckets; a PCM segment's actual `start_sample`/`end_sample_exclusive` describe its observed coverage, which may occupy only part of that bucket.

Important options:

- `--window-seconds 1`: health and PCM analysis window.
- `--origin-sample N`: capture/window origin, including recordings with large absolute sample indices.
- `--end-exclusive N`: known capture end; otherwise the offline tool uses one sample after the last observed edge and does not claim knowledge of a longer tail.
- `--nominal-rate Hz`: constrain timing acquisition; zero means automatic.
- `--pcm-bits 16..24`: resolve unknown declared PCM precision explicitly.
- `--wav-output NEW_DIRECTORY`, `--wav-rate Hz`, `--assume-stereo`: segmented WAV export and explicit interpretation overrides.
- `--burst-output NEW_DIRECTORY`: supported IEC 61937 payload files and index.
- `--fail-on parity,sync_loss,content_conflict,no_frames`: configurable failure policies.
- `--expect-content pcm|nonpcm|ac3|eac3|dts|dts1|dts2|dts3`: fail if the expected classified content is absent.

Exit codes are **0** for success, **1** for no decoded frames or a failed analysis policy, and **2** for invalid input/configuration or I/O failure. Format presence is established from decoded classification, not from a substring in a diagnostic message. Audio and payload outputs follow the integrity/segmentation rules in [feature definitions](FEATURES.md).

## Compare two PCM exports

```sh
python scripts/compare_streams.py stream-a.jsonl stream-b.jsonl --max-lag 128
python scripts/compare_streams.py D6.csv D7.csv --rate 48000 --bits 24 --max-lag 128
```

The comparator accepts version-1 JSONL or subframe CSV exports. Supply explicit nominal rate/precision where old CSV metadata is missing. Known conflicting declarations are rejected rather than overridden. Use `--start-seconds`/`--end-seconds` to select a compatible interval when a capture changes rate or layout. `--swap-b` explicitly swaps B's A/B sample channels.

Positive lag means `B[k + lag_frames] == A[k]`. Reports retain source origins and sample locations; separate captures are not automatically clock-synchronized. Invalid, unknown and missing frame positions remain holes. Integer-frame alignment uses local timestamp steps so small clock-estimation errors do not accumulate into invented missing frames. It never silently resamples.

Silence, repetition, inadequate overlap and tied candidates produce an ambiguous/insufficient-evidence result rather than a fabricated delay. Search work, input frame count, and reported mismatch locations are bounded by options shown in `--help`. Exit **0** means a unique exact compared overlap, **1** means mismatches/holes/ambiguity/insufficient evidence, and **2** means malformed or incompatible input. These results describe relative captured-data alignment, not calibrated clock jitter.
