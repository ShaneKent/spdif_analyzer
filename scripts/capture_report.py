#!/usr/bin/env python3
"""Bounded, standalone offline report for S/PDIF analyzer version-1 exports."""
from __future__ import annotations
import argparse
from collections import Counter
import csv
from decimal import Decimal, InvalidOperation, localcontext
import html
import json
import math
import os
from pathlib import Path
import sys
import tempfile

UINT64_MAX = (1 << 64) - 1
DATASETS = {'subframes.csv': 'subframe', 'channel-status.csv': 'channel_status',
            'diagnostics.csv': 'diagnostic', 'content.csv': 'content'}


class ReportError(ValueError):
    pass


def decimal(value):
    try:
        result = Decimal(str(value))
    except InvalidOperation:
        raise ReportError('Invalid decimal timestamp or rate')
    if not result.is_finite():
        raise ReportError('Nonfinite decimal timestamp or rate')
    return result


def index(value):
    if isinstance(value, bool) or not isinstance(value, (str, int)):
        raise ReportError('Sample indices must be unsigned decimal strings or integers')
    text = str(value)
    if not text.isascii() or not text.isdigit() or not 0 <= int(text) <= UINT64_MAX:
        raise ReportError('Sample index outside uint64 range')
    return int(text)


def integer(value, name, hexadecimal=False):
    """Interpret integer syntax without silently rounding JSON numeric values."""
    if isinstance(value, int) and not isinstance(value, bool):
        return value
    if isinstance(value, str):
        text = value.strip()
        if hexadecimal and text.lower().startswith('0x') and len(text) > 2 and all(c in '0123456789abcdefABCDEF' for c in text[2:]):
            return int(text, 16)
        digits = text[1:] if text.startswith(('-', '+')) else text
        if text.isascii() and digits.isdigit():
            return int(text)
    raise ReportError(name + ' must be an integer, not a boolean or fractional value')


def boolean(value):
    if isinstance(value, bool):
        return value
    if value in (0, '0', 'false', 'False', ''):
        return False
    if value in (1, '1', 'true', 'True'):
        return True
    raise ReportError('Invalid boolean field')


def safe_json(value):
    # An HTML script element's raw-text parser recognizes </script> even inside
    # application/json. Escape HTML metacharacters, not merely JSON quotes.
    return json.dumps(value, ensure_ascii=True, allow_nan=False, separators=(',', ':')).replace('<', '\\u003c').replace('>', '\\u003e').replace('&', '\\u0026')


def strict_json(text):
    def reject(value):
        raise ReportError('Nonfinite JSON constant: ' + value)
    try:
        return json.loads(text, parse_constant=reject)
    except (ValueError, RecursionError) as error:
        raise ReportError('Invalid JSON: ' + str(error))


class Bins:
    """Streaming adjacent min/max reduction. Counts survive every reduction."""
    def __init__(self, limit):
        self.limit = limit
        self.items = []
        self.level = 1

    @staticmethod
    def merge(a, b):
        states = dict(a['states'])
        for key, count in b['states'].items():
            states[key] = states.get(key, 0) + count
        low = [n for n in (a['minimum'], b['minimum']) if n is not None]
        high = [n for n in (a['maximum'], b['maximum']) if n is not None]
        return {'t0': a['t0'], 't1': b['t1'], 'count': a['count'] + b['count'],
                'accepted': a['accepted'] + b['accepted'], 'states': states,
                'minimum': min(low) if low else None, 'maximum': max(high) if high else None}

    def add(self, t0, t1, state, value=None):
        item = {'t0': str(t0), 't1': str(t1), 'count': 1, 'accepted': int(value is not None),
                'states': {state: 1}, 'minimum': value, 'maximum': value}
        if self.items and self.items[-1]['count'] < self.level:
            self.items[-1] = self.merge(self.items[-1], item)
        else:
            self.items.append(item)
        if len(self.items) > self.limit:
            self.items = [self.merge(self.items[i], self.items[i + 1]) if i + 1 < len(self.items) else self.items[i]
                          for i in range(0, len(self.items), 2)]
            self.level *= 2


class Builder:
    def __init__(self, args):
        self.args = args
        self.rate = None
        self.origin = 0
        self.basis = 'source samples relative to metadata origin'
        self.counts = Counter()
        self.main_counts = Counter()
        self.events = []
        self.sources = []
        self.notes = []
        self.summaries = []
        self.records = self.input_bytes = 0
        self.stats = {'subframes': 0, 'pcm_eligible': 0, 'parity_failures': 0, 'V_invalid': 0,
                      'excluded': {}, 'content': {}, 'channels': [0, 0]}
        self.series = [Bins(args.max_plot_bins), Bins(args.max_plot_bins)]
        self.comparisons = []
        self.low = self.high = None
        self.last = [None, None]
        self.declared_low = self.declared_high = None
        self.metadata_bits = 0
        self.csv_mode = False
        self.csv_trigger_known = True
        self.primary_metadata_seen = False
        self.active_source = ''
        self.first_subframe = self.last_subframe = None

    def read_text(self, path):
        try:
            size = path.stat().st_size
        except OSError as error:
            raise ReportError(str(error))
        self.input_bytes += size
        if self.input_bytes > self.args.max_input_bytes:
            raise ReportError('Input byte budget exceeded (--max-input-bytes)')
        return path.read_text(encoding='utf-8-sig')

    def account(self, kind, primary=False):
        self.records += 1
        if self.records > self.args.max_records:
            raise ReportError('Input record budget exceeded (--max-records)')
        self.counts[kind] += 1
        if primary:
            self.main_counts[kind] += 1

    def precision_override(self, value):
        bits = integer(value, 'pcm_bits_override')
        if bits != 0 and not 16 <= bits <= 24:
            raise ReportError('pcm_bits_override must be zero or 16..24 bits')
        return bits

    def context(self, record, primary=False):
        if 'capture_rate_hz' not in record:
            return
        rate = decimal(record['capture_rate_hz'])
        if rate <= 0 or rate > UINT64_MAX:
            raise ReportError('capture_rate_hz must be positive and bounded')
        if self.rate is not None and rate != self.rate:
            raise ReportError('Optional analysis capture rate conflicts with primary input')
        self.rate = rate
        if primary:
            self.origin = index(record.get('origin_sample', '0'))
            self.metadata_bits = self.precision_override(record.get('pcm_bits_override', 0))
        if primary and 'coverage_start_sample' in record:
            self.declared_low = self.sample_time(record['coverage_start_sample'])
        if primary and 'coverage_end_sample_inclusive' in record:
            self.declared_high = self.sample_time(record['coverage_end_sample_inclusive']) + 1 / rate

    def sample_time(self, value):
        if self.rate is None:
            raise ReportError('Source intervals require capture_rate_hz metadata')
        if self.csv_mode and not self.csv_trigger_known:
            raise ReportError('Cannot align sample-index analysis to CSV bundle without trigger_sample')
        return Decimal(index(value) - self.origin) / self.rate

    def interval(self, record):
        # Actual source coverage takes precedence over the Logic notification
        # interval, which remains retained verbatim in the details table.
        if 'coverage_start_sample' in record:
            first = record['coverage_start_sample']
            last = record.get('coverage_end_sample', record.get('coverage_end_sample_inclusive', first))
            return self.sample_time(first), self.sample_time(last) + 1 / self.rate, 'source coverage samples'
        if record.get('event') == 'codec_frame' and not boolean(record.get('source_interval_known', False)):
            if 'transport_start_sample' in record:
                return self.sample_time(record['transport_start_sample']), self.sample_time(record['transport_end_sample_inclusive']) + 1 / self.rate, 'transport interval; codec source interval unknown'
            return None, None, 'source interval unknown'
        if 'start_sample' in record:
            first = self.sample_time(record['start_sample'])
            if 'end_sample_exclusive' in record:
                last = self.sample_time(record['end_sample_exclusive'])
            else:
                last = self.sample_time(record.get('end_sample_inclusive', record.get('end_sample', record['start_sample']))) + 1 / self.rate
            return first, last, 'source samples'
        if 'start_seconds' in record:
            first = decimal(record['start_seconds'])
            last = decimal(record.get('end_seconds_exclusive', record.get('end_seconds', record['start_seconds'])))
            if 'end_seconds_exclusive' not in record and self.rate:
                last += 1 / self.rate
            return first, last, 'exported trigger-relative seconds (CSV precision)'
        return None, None, 'interval unavailable'

    def extent(self, low, high):
        if low is None:
            return
        if high < low:
            raise ReportError('Interval end precedes start')
        if not math.isfinite(float(low)) or not math.isfinite(float(high)):
            raise ReportError('Relative interval exceeds plotting range')
        self.low = low if self.low is None else min(self.low, low)
        self.high = high if self.high is None else max(self.high, high)

    def sparse(self, kind, record, low, high, basis):
        if len(self.events) >= self.args.max_events:
            raise ReportError('Sparse event budget exceeded (--max-events); no events were discarded')
        label = str(record.get('format', record.get('codec', record.get('code', record.get('interpretation', record.get('status', kind))))))
        if kind == 'health_window':
            channels = record.get('channels', [])
            label = 'decoded={} parity={} unknown_samples={}'.format(sum(integer(c.get('decoded_subframes', 0), 'decoded_subframes') for c in channels), sum(integer(c.get('parity_failures', 0), 'parity_failures') for c in channels), record.get('unknown_stream_samples', 'unknown'))
        elif kind == 'pcm_window':
            label = 'channel={} accepted={} RMS={} peak={}'.format(record.get('channel', '?'), record.get('accepted', '?'), record.get('rms_normalized', record.get('rms', '?')), record.get('peak_normalized', record.get('peak', '?')))
        elif kind == 'codec_frame':
            label += ' header={} structure={} frameCRC={}'.format(record.get('header_state', '?'), record.get('structure', '?'), record.get('frame_crc', '?'))
        elif kind == 'status_change':
            label = 'channel={} status fields changed={}'.format(record.get('channel', '?'), len(record.get('fields', [])))
        self.events.append({'kind': kind, 'label': label, 't0': str(low) if low is not None else None,
                            't1': str(high) if high is not None else None, 'time_basis': basis,
                            'source': self.active_source, 'record': record})

    def subframe(self, record, low, high):
        if low is None:
            raise ReportError('Subframe has no usable source interval')
        channel = record.get('channel', 0)
        channel = {'A': 0, 'B': 1}.get(channel, channel)
        channel = integer(channel, 'Subframe channel')
        if channel not in (0, 1):
            raise ReportError('Invalid A/B subframe channel')
        if self.last[channel] is not None and low < self.last[channel] - (2 / self.rate if self.rate else Decimal(0)):
            raise ReportError('Subframes for each channel must be monotonic and nonoverlapping')
        self.last[channel] = high
        content = str(record.get('content', 'Unknown'))
        parity = boolean(record.get('parity_ok', False))
        invalid = boolean(record.get('V_invalid', False))
        if self.first_subframe is None:
            self.first_subframe = low
        if self.last_subframe is not None and low > self.last_subframe + (2 / self.rate if self.rate else 0):
            gap = {'event': 'capture_gap', 'schema_version': 1, 'start_seconds': str(self.last_subframe),
                   'end_seconds_exclusive': str(low), 'evidence': 'No decoded subframe coverage in this source interval; audio is unknown'}
            self.sparse('capture_gap', gap, self.last_subframe, low, 'derived source coverage gap')
        self.last_subframe = high if self.last_subframe is None else max(self.last_subframe, high)
        self.stats['subframes'] += 1
        self.stats['channels'][channel] += 1
        self.stats['parity_failures'] += not parity
        self.stats['V_invalid'] += invalid
        self.stats['content'][content] = self.stats['content'].get(content, 0) + 1
        layout = record.get('layout', {})
        if not isinstance(layout, dict):
            raise ReportError('PCM layout must be an object')
        known = boolean(layout.get('known', record.get('layout_known', False)))
        bits = integer(layout.get('bits', record.get('layout_bits', 0)), 'PCM layout bits')
        raw = record.get('raw_slots_4_31')
        if 'raw_slots_4_31' in record:
            raw = integer(raw, 'raw_slots_4_31', hexadecimal=True)
            if not 0 <= raw < (1 << 28):
                raise ReportError('raw_slots_4_31 exceeds 28 bits')
        if not known and self.metadata_bits:
            bits = self.metadata_bits
            known = True
        if not parity:
            reason = 'excluded: parity'
        elif invalid:
            reason = 'excluded: validity'
        elif content.upper() != 'PCM':
            reason = 'excluded: ' + content
        elif not known or not 1 <= bits <= 24:
            reason = 'excluded: PCM word length unknown'
        else:
            reason = 'PCM'
        value = None
        if reason == 'PCM':
            if raw is None:
                raise ReportError('PCM raw_slots_4_31 is missing')
            signed = (raw >> (24 - bits)) & ((1 << bits) - 1)
            if signed & (1 << (bits - 1)):
                signed -= 1 << bits
            value = signed / (1 << (bits - 1))
            self.stats['pcm_eligible'] += 1
        else:
            self.stats['excluded'][reason] = self.stats['excluded'].get(reason, 0) + 1
        self.series[channel].add(low, high, reason, value)

    def add(self, record, primary=False):
        if not isinstance(record, dict) or str(record.get('schema_version')) != '1':
            raise ReportError('Expected version-1 JSON export objects')
        kind = str(record.get('event', record.get('type', 'unknown')))
        self.account(kind, primary)
        if boolean(record.get('live_snapshot', False)):
            note = 'Input contains a live snapshot; pending/tail coverage is not final.'
            if note not in self.notes:
                self.notes.append(note)
        if kind == 'metadata':
            if primary and self.primary_metadata_seen:
                raise ReportError('Primary input must contain one initial metadata record')
            if primary:
                self.primary_metadata_seen = True
            self.context(record, primary)
            return
        if 'capture_rate_hz' in record:
            self.context(record, False)
        low, high, basis = self.interval(record)
        self.extent(low, high)
        if kind == 'subframe':
            self.subframe(record, low, high)
            return
        if kind == 'summary':
            self.summaries.append({'source': self.active_source, 'record': record, 'primary': primary})
            for window in record.get('health_windows', []):
                self.add(window, False)
            return
        self.sparse(kind, record, low, high, basis)

    def json_input(self, path, primary=False):
        self.active_source = path.name
        text = self.read_text(path)
        # The producer writes one JSON object per line; health.json is one object
        # containing an array. Pretty-printed single JSON is accepted as well.
        try:
            whole = strict_json(text)
        except ReportError:
            whole = None
        if isinstance(whole, dict):
            records = [whole]
        elif isinstance(whole, list):
            records = whole
        else:
            records = (strict_json(line) for line in text.splitlines() if line.strip())
        first = True
        for record in records:
            if not isinstance(record, dict):
                raise ReportError('JSON export records must be objects')
            if primary and first and record.get('event') != 'metadata':
                raise ReportError('Primary JSONL requires initial metadata record')
            self.add(record, primary)
            first = False
        if first:
            raise ReportError('Empty JSON input')
        if primary and self.rate is None:
            raise ReportError('Primary metadata requires capture_rate_hz')

    def bundle(self, directory):
        self.csv_mode = True
        self.active_source = 'manifest.json'
        manifest = strict_json(self.read_text(directory / 'manifest.json'))
        if not isinstance(manifest, dict) or str(manifest.get('schema_version')) != '1':
            raise ReportError('Unsupported bundle manifest version')
        self.rate = decimal(manifest['capture_rate'])
        if self.rate <= 0:
            raise ReportError('Bundle capture rate must be positive')
        settings = manifest.get('settings', {})
        if not isinstance(settings, dict):
            raise ReportError('Bundle settings must be an object')
        trigger = settings.get('trigger_sample')
        self.origin = index(trigger) if trigger is not None else 0
        self.csv_trigger_known = trigger is not None
        self.basis = 'exported trigger-relative CSV seconds; source indices not reconstructed'
        self.metadata_bits = self.precision_override(settings.get('pcm_bits_override', 0))
        self.sources.append({'path': str(directory), 'kind': 'four-CSV bundle', 'manifest': manifest})
        if trigger is not None:
            self.declared_low = self.sample_time(manifest['coverage_start_sample'])
            self.declared_high = self.sample_time(manifest['coverage_end_sample']) + 1 / self.rate
        else:
            self.notes.append('Bundle has no trigger_sample: CSV seconds remain exact as exported; absolute sample indices and coverage cannot be reconstructed.')
        if not boolean(manifest.get('complete', False)) or (directory / 'INCOMPLETE').exists():
            self.notes.append('INCOMPLETE bundle: missing/unwritten data remains unknown; this report does not certify a completed export.')
        expected = {}
        for entry in manifest.get('datasets', []):
            name = entry['file']
            if name in expected or name not in DATASETS:
                raise ReportError('Unexpected or duplicate version-1 bundle dataset')
            written, snapshot = integer(entry['written_rows'], 'written_rows'), integer(entry['snapshot_rows'], 'snapshot_rows')
            if not 0 <= written <= snapshot or boolean(manifest.get('complete', False)) and written != snapshot:
                raise ReportError('Bundle manifest snapshot/written-row count mismatch')
            expected[name] = written
        if set(expected) != set(DATASETS):
            raise ReportError('Bundle manifest must describe all four CSV datasets')
        for name, kind in DATASETS.items():
            path = directory / name
            if not path.exists():
                if boolean(manifest.get('complete', False)):
                    raise ReportError('Complete bundle is missing ' + name)
                self.notes.append('Incomplete bundle missing dataset: ' + name)
                continue
            self.active_source = name
            import io
            rows = csv.DictReader(io.StringIO(self.read_text(path), newline=''))
            if not rows.fieldnames or len(set(rows.fieldnames)) != len(rows.fieldnames) or not {'start_seconds', 'end_seconds'} <= set(rows.fieldnames):
                raise ReportError('Invalid bundle CSV header: ' + name)
            count = 0
            for row in rows:
                if None in row or any(value is None for value in row.values()):
                    raise ReportError('Malformed bundle CSV row: ' + name)
                count += 1
                for key in ('burst', 'complete', 'integrity'):
                    if key in row:
                        row[key] = boolean(row[key])
                self.add({'schema_version': 1, 'event': kind, **row}, True)
            if name in expected and count != expected[name]:
                raise ReportError('Bundle written-row count mismatch: ' + name)

    def comparison(self, path):
        self.active_source = path.name
        record = strict_json(self.read_text(path))
        if not isinstance(record, dict) or record.get('schema_version') != 1:
            raise ReportError('Unsupported comparison report version')
        self.account('comparison_report')
        self.comparisons.append(record)
        comparison_shift = None
        provenance = record.get('sources', [])
        if provenance and isinstance(provenance[0], dict):
            source = provenance[0]
            capture = source.get('capture_rate_hz')
            if capture is not None and self.rate is not None:
                if decimal(capture) != self.rate:
                    raise ReportError('Comparison source A capture rate conflicts with primary input')
                if not self.csv_mode or self.csv_trigger_known:
                    comparison_shift = Decimal(index(source['origin']) - self.origin) / self.rate
            elif self.csv_mode and source.get('origin') == 'trigger-relative CSV seconds':
                comparison_shift = Decimal(0)
        if comparison_shift is None:
            self.notes.append('Comparison source A origin cannot be mapped to this capture. Comparison intervals remain in the table and are not plotted on an invented shared axis.')
        elif comparison_shift:
            self.notes.append('Comparison source A origin translated by {} seconds; this aligns declared sample origins, not independent capture clocks.'.format(comparison_shift))
        windows = record.get('windows', [])
        if not isinstance(windows, list):
            raise ReportError('Comparison windows must be an array')
        if not windows:
            self.sparse('relative_delay', record, None, None, 'comparison source origins; full selected interval')
        for window in windows:
            self.account('relative_delay')
            if not isinstance(window, dict):
                raise ReportError('Invalid comparison window')
            if 'index' in window:
                integer(window['index'], 'Comparison window index')
            low = decimal(window['start_seconds'])
            high = decimal(window['end_seconds_exclusive'])
            if comparison_shift is not None:
                low += comparison_shift
                high += comparison_shift
            else:
                low = high = None
            self.extent(low, high)
            self.sparse('relative_delay', window, low, high, 'comparison source A seconds; independent captures are not clock-calibrated')
        candidates = record.get('delay_change_candidates', [])
        drifts = record.get('drift_segments', [])
        if not isinstance(candidates, list) or not isinstance(drifts, list):
            raise ReportError('Comparison candidates and drift segments must be arrays')
        for candidate in candidates:
            self.account('delay_change_candidate')
            if not isinstance(candidate, dict):
                raise ReportError('Comparison candidate must be an object')
            for key in ('support_windows_before', 'support_windows_after', 'transition_windows'):
                for window_index in candidate.get(key, []):
                    integer(window_index, 'Candidate ' + key)
            low = decimal(candidate['interval_start_seconds'])
            high = decimal(candidate['interval_end_seconds_exclusive'])
            if comparison_shift is not None:
                low += comparison_shift
                high += comparison_shift
            else:
                low = high = None
            self.extent(low, high)
            self.sparse('delay_change_candidate', candidate, low, high, 'comparison candidate interval; not proof of lost audio')
        for drift in drifts:
            self.account('drift_segment')
            if not isinstance(drift, dict):
                raise ReportError('Comparison drift segment must be an object')
            for key in ('first_window', 'last_window'):
                if key in drift:
                    integer(drift[key], 'Drift ' + key)
            for window_index in drift.get('support_windows', []):
                integer(window_index, 'Drift support_windows')
            # Different comparator versions retain different fit interval keys;
            # preserve every record even if its plotting interval is unavailable.
            low = drift.get('start_seconds')
            high = drift.get('end_seconds_exclusive')
            if low is None and high is None and 'first_window' in drift and 'last_window' in drift:
                supporting = [w for w in windows if integer(drift['first_window'], 'Drift first_window') <= integer(w.get('index', -1), 'Comparison window index') <= integer(drift['last_window'], 'Drift last_window')]
                if supporting:
                    low = min(decimal(w['start_seconds']) for w in supporting)
                    high = max(decimal(w['end_seconds_exclusive']) for w in supporting)
            low, high = (decimal(low), decimal(high)) if low is not None and high is not None else (None, None)
            if comparison_shift is not None and low is not None:
                low += comparison_shift
                high += comparison_shift
            else:
                low = high = None
            self.extent(low, high)
            self.sparse('drift_segment', drift, low, high, 'uncalibrated relative captured timestamp fit')

    def result(self):
        for summary in self.summaries:
            totals = summary['record'].get('totals', {})
            if summary['primary']:
                for key, event in [('subframes', 'subframe'), ('status_records', 'channel_status')]:
                    if key in totals and integer(totals[key], 'Summary ' + key) != self.main_counts[event]:
                        raise ReportError('Primary summary/detail count mismatch: ' + key)
        if self.declared_low is not None and self.declared_high is not None and self.declared_high < self.declared_low:
            raise ReportError('Declared capture coverage end precedes start')
        boundaries = [(self.declared_low, self.first_subframe), (self.last_subframe, self.declared_high)]
        if self.first_subframe is None:
            boundaries = [(self.declared_low, self.declared_high)]
        for low, high in boundaries:
            if low is not None and high is not None and high > low:
                self.sparse('capture_gap', {'schema_version': 1, 'event': 'capture_gap', 'start_seconds': str(low),
                            'end_seconds_exclusive': str(high), 'evidence': 'Unknown/unobserved capture boundary coverage'},
                            low, high, 'declared source coverage boundary')
        for bound in (self.declared_low, self.declared_high):
            if bound is not None:
                self.extent(bound, bound)
        low = self.low if self.low is not None else Decimal(0)
        high = self.high if self.high is not None else low
        # Subtract the view's exact Decimal base before converting to plotting
        # floats, even when metadata origin0 accompanies uint64-sized stamps.
        for item in self.events:
            item['plot_t0'] = float(decimal(item['t0']) - low) if item['t0'] is not None else None
            item['plot_t1'] = float(decimal(item['t1']) - low) if item['t1'] is not None else None
        for series in self.series:
            for item in series.items:
                item['plot_t0'] = float(decimal(item['t0']) - low)
                item['plot_t1'] = float(decimal(item['t1']) - low)
        return {'schema_version': 1, 'plot_origin_seconds': str(low), 'plot_range': [0, float(high - low)], 'title': self.args.title, 'capture_rate_hz': str(self.rate) if self.rate else None,
                'origin_sample': str(self.origin), 'time_basis': self.basis, 'range': [str(low), str(high)],
                'counts': dict(self.counts), 'main_counts': dict(self.main_counts), 'stats': self.stats,
                'sparse_event_count': len(self.events), 'input_records': self.records, 'input_bytes': self.input_bytes,
                'events': self.events, 'pcm_bins': [s.items for s in self.series], 'summaries': self.summaries,
                'sources': self.sources, 'notes': self.notes, 'comparisons': self.comparisons,
                'limits': {'sparse_events': self.args.max_events, 'plot_bins_per_channel': self.args.max_plot_bins,
                           'input_records': self.args.max_records},
                'dense_policy': 'Subframes reduced to per-channel adjacent min/max bins; original counts and excluded-state counts preserved. All sparse records retained in paginated table.'}


HTML = r'''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'none'; img-src data:; base-uri 'none'; form-action 'none'">
<title>__TITLE__</title><style>
:root{color-scheme:dark;--bg:#101820;--panel:#192631;--fg:#e5eef4;--muted:#aabcc9;--line:#375063}*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px system-ui,sans-serif}main{max-width:1550px;margin:auto;padding:24px}h1{font-size:25px;margin:0 0 8px}h2{font-size:18px}p{line-height:1.5;color:var(--muted)}.panel{background:var(--panel);border:1px solid var(--line);border-radius:9px;padding:16px;margin:16px 0}.controls{display:flex;flex-wrap:wrap;gap:9px;align-items:center}button,input,select{font:inherit;color:var(--fg);background:#203848;border:1px solid #547185;border-radius:4px;padding:6px}button{cursor:pointer}button:hover{background:#36556a}input[type=number]{width:135px}input[type=search]{min-width:200px;flex:1}label{color:var(--muted)}#stats{display:flex;flex-wrap:wrap;gap:12px}.stat{border-left:3px solid #4db8b2;padding:3px 12px}.stat strong{display:block;font-size:22px}.warn{border-left:3px solid #e9b55d;padding:8px;color:#f1ce94}canvas{width:100%;height:490px;display:block;touch-action:none;cursor:crosshair}table{border-collapse:collapse;width:100%;font-size:13px;table-layout:fixed}th,td{text-align:left;border-bottom:1px solid var(--line);padding:8px;overflow-wrap:anywhere}th{color:var(--muted)}tbody tr{cursor:pointer}tbody tr:hover{background:#294457}th:nth-child(1){width:14%}th:nth-child(2){width:14%}th:nth-child(3){width:20%}th:nth-child(4){width:38%}th:nth-child(5){width:14%}pre{white-space:pre-wrap;overflow-wrap:anywhere;max-height:500px;overflow:auto;font-size:12px}summary{cursor:pointer;padding:6px}.legend{color:var(--muted);font-size:13px}.pill{display:inline-block;padding:3px 6px;border:1px solid var(--line);border-radius:4px;margin:3px}.empty{padding:20px;color:var(--muted)}#hover{min-height:36px;font-family:ui-monospace,monospace;font-size:12px}@media(max-width:700px){main{padding:12px}canvas{height:430px}table{font-size:11px}.panel{padding:10px}}
</style></head><body><main><h1 id="title"></h1><p>Offline S/PDIF evidence on a shared timeline. Select an event for exact source timestamps and raw fields, then locate it manually in Logic. All times are relative to the stated origin; compressed audio is not decoded.</p>
<div class="panel"><div id="stats"></div><div id="notes"></div><p id="basis"></p><details><summary>Counts, input provenance and authoritative exported totals</summary><pre id="totals"></pre></details></div>
<div class="panel"><div class="controls"><button id="back">← Pan</button><button id="out">Zoom out</button><button id="in">Zoom in</button><button id="forward">Pan →</button><button id="reset">Full capture</button><label>Start offset s <input id="low" type="number" step="any"></label><label>End offset s <input id="high" type="number" step="any"></label><button id="apply">Apply range</button></div>
<p class="legend">Drag to pan · scroll to zoom · click plotted evidence to inspect. Teal = PCM eligible; orange = unknown/excluded/provisional; red = fault; violet = bursts/codec evidence. Min/max bins retain peaks and exact observation counts; a bin may contain gaps or mixed states. The table retains every sparse event.</p><canvas id="plot" aria-label="Shared timeline of format, status, faults, coverage, PCM and delay" role="img"></canvas><div id="hover"></div><div id="filters" class="controls"></div></div>
<div class="panel"><div class="controls"><h2>Events</h2><input id="search" type="search" placeholder="Search all sparse event fields" aria-label="Search all sparse event fields"><label><input type="checkbox" id="onlyrange"> Table follows visible range</label></div><p id="eventcount"></p><table><thead><tr><th>Time (s)</th><th>Kind</th><th>Source interval</th><th>Evidence</th><th>Input</th></tr></thead><tbody id="rows"></tbody></table><div class="controls"><button id="previous">Previous page</button><span id="page"></span><button id="next">Next page</button></div></div>
<div class="panel"><h2>Selected evidence</h2><p id="selectednote">Select a row or plotted event. Details preserve source intervals separately from any notification/publication position; no automatic Logic navigation is attempted.</p><pre id="details"></pre></div>
<details class="panel"><summary>Relative-delay reports and uncertainty</summary><p>Alignment is relative captured data, with independent origins and clocks. Ambiguous windows, holes, transition candidates and uncalibrated drift remain explicit.</p><pre id="comparison"></pre></details>
</main><script type="application/json" id="report-data">__DATA__</script><script>
'use strict';
const D=JSON.parse(document.getElementById('report-data').textContent),$=id=>document.getElementById(id);
const colors={diagnostic:'#ee807b',capture_gap:'#e7ad56',channel_status:'#66b9da',status_change:'#66b9da',content:'#4db8b2',codec_frame:'#ba9ce7',relative_delay:'#f3ce7b',delay_change_candidate:'#e7ad56',drift_segment:'#f3ce7b',health_window:'#8eb5cc',pcm_window:'#4db8b2'};
$('title').textContent=D.title;document.title=D.title;
for(const [label,n] of [['Subframes',D.stats.subframes],['PCM eligible',D.stats.pcm_eligible],['Excluded',D.stats.subframes-D.stats.pcm_eligible],['Parity failures',D.stats.parity_failures],['Sparse events',D.sparse_event_count]]){const e=document.createElement('div');e.className='stat';const strong=document.createElement('strong');strong.textContent=n;e.append(strong,document.createTextNode(label));$('stats').append(e);}
for(const note of D.notes){const p=document.createElement('p');p.className='warn';p.textContent=note;$('notes').append(p);}
$('basis').textContent='Time basis: '+D.time_basis+' · capture rate '+(D.capture_rate_hz||'unknown')+' Hz · origin sample '+D.origin_sample+' · plot offsets start at exact relative time '+D.plot_origin_seconds+' seconds. Exact sample indices remain decimal strings. CSV times retain the precision of their export.';
$('totals').textContent=JSON.stringify({counts:D.counts,primary_counts:D.main_counts,subframe_statistics:D.stats,input_records:D.input_records,input_bytes:D.input_bytes,dense_policy:D.dense_policy,limits:D.limits,sources:D.sources,exported_summaries:D.summaries},null,2);
$('comparison').textContent=JSON.stringify(D.comparisons,null,2);
const events=D.events.map((e,i)=>({...e,id:i,a:e.plot_t0,b:e.plot_t1}));
const kinds=[...new Set(events.map(e=>e.kind))].sort(),enabled=new Set(kinds);let low=D.plot_range[0],high=D.plot_range[1];if(!(high>low))high=low+1;
const initial=[low,high];let page=0,filtered=[],selected=null,points=[];
function active(e){return enabled.has(e.kind)}
function inrange(e){return e.a===null||e.b>=low&&e.a<=high}
function select(e){selected=e.id;$('details').textContent=JSON.stringify(e.record,null,2);$('selectednote').textContent=e.kind+' · '+e.time_basis+' · source interval '+(e.t0===null?'unavailable':e.t0+' to '+e.t1+' seconds (exclusive plotted end)')+' · '+e.source;draw();}
function exact(e){const r=e.record;if(r.coverage_start_sample!==undefined)return r.coverage_start_sample+'…'+(r.coverage_end_sample||r.coverage_end_sample_inclusive)+' samples';if(r.source_interval_known===false)return 'codec source unknown; transport '+(r.transport_start_sample||'?')+'…'+(r.transport_end_sample_inclusive||'?');if(r.start_sample!==undefined)return r.start_sample+'…'+(r.end_sample_exclusive!==undefined?r.end_sample_exclusive+' exclusive':r.end_sample_inclusive||r.end_sample||r.start_sample)+' samples';return e.t0===null?'unavailable':e.t0+'…'+e.t1+' seconds';}
function renderTable(){const q=$('search').value.toLowerCase(),only=$('onlyrange').checked;filtered=events.filter(e=>active(e)&&(!only||inrange(e))&&(!q||JSON.stringify(e.record).toLowerCase().includes(q)||e.kind.toLowerCase().includes(q)));const pages=Math.max(1,Math.ceil(filtered.length/100));page=Math.min(page,pages-1);$('rows').replaceChildren();for(const e of filtered.slice(page*100,page*100+100)){const tr=document.createElement('tr');for(const v of [e.t0===null?'unknown':e.t0,e.kind,exact(e),e.label,e.source]){const td=document.createElement('td');td.textContent=v;tr.append(td)}tr.onclick=()=>select(e);$('rows').append(tr)}$('eventcount').textContent=filtered.length+' matching events / '+events.length+' retained. '+D.stats.subframes+' dense subframes counted separately.';$('page').textContent='Page '+(page+1)+' / '+pages;$('previous').disabled=page===0;$('next').disabled=page+1>=pages;}
for(const kind of kinds){const label=document.createElement('label');label.className='pill';const box=document.createElement('input');box.type='checkbox';box.checked=true;box.onchange=()=>{if(box.checked)enabled.add(kind);else enabled.delete(kind);page=0;renderTable();draw()};label.append(box,document.createTextNode(' '+kind));$('filters').append(label)}
function lane(e){if(e.kind==='diagnostic'||e.kind==='capture_gap')return 2;if(e.kind==='channel_status'||e.kind==='status_change')return 1;if(e.kind==='health_window')return 3;if(e.kind==='pcm_window')return 4;if(e.kind==='relative_delay'||e.kind==='delay_change_candidate'||e.kind==='drift_segment')return 8;if(e.kind==='codec_frame'||e.record.burst===true||e.record.burst==='1')return 5;return 0;}
const canvas=$('plot'),ctx=canvas.getContext('2d');
function draw(){const width=canvas.clientWidth,height=canvas.clientHeight,ratio=window.devicePixelRatio||1;canvas.width=Math.round(width*ratio);canvas.height=Math.round(height*ratio);ctx.setTransform(ratio,0,0,ratio,0,0);ctx.clearRect(0,0,width,height);const left=110,right=width-16,top=34,step=(height-top-25)/9;const x=t=>left+(t-low)/(high-low)*(right-left);points=[];ctx.font='11px system-ui';ctx.strokeStyle='#375063';ctx.fillStyle='#aabcc9';for(let i=0;i<=5;i++){const xx=left+i*(right-left)/5;ctx.beginPath();ctx.moveTo(xx,top-10);ctx.lineTo(xx,height-20);ctx.stroke();ctx.fillText((low+i*(high-low)/5).toPrecision(6),Math.max(left,xx-22),16)}const names=['Format / other','Status / changes','Fault / unknown','Capture health','PCM statistics','Burst / codec','PCM A min/max','PCM B min/max','Relative delay'];for(let i=0;i<9;i++){const y=top+i*step;ctx.fillStyle='#aabcc9';ctx.fillText(names[i],4,y+12);ctx.strokeStyle='#293e4d';ctx.beginPath();ctx.moveTo(left,y+step-4);ctx.lineTo(right,y+step-4);ctx.stroke()}
ctx.save();ctx.beginPath();ctx.rect(left,top-10,right-left,height-top-10);ctx.clip();for(const e of events){if(!active(e)||e.a===null||!inrange(e))continue;const y=top+lane(e)*step+6;let color=colors[e.kind]||'#9bb2c6';const r=e.record;if(e.kind==='codec_frame'&&(r.frame_crc==='failed'||r.structure==='failed'||r.transport_conflict))color='#ee807b';else if((e.kind==='pcm_window'&&['excluded_parity','excluded_invalid','excluded_unknown','excluded_non_pcm','excluded_layout'].some(k=>Number(r[k]||0)>0))||r.integrity===false||r.complete===false||/unknown|provisional|conflict|excluded|ambig|insufficient|incomplete_coverage/i.test(e.label+' '+(r.status||'')))color='#e7ad56';const xa=x(e.a),xb=x(e.b);ctx.globalAlpha=e.id===selected?1:.65;ctx.fillStyle=color;ctx.fillRect(xa,y,Math.max(2,xb-xa),Math.max(4,step-18));points.push({x:Math.max(left,Math.min(right,(xa+xb)/2)),y:y+(step-18)/2,e});if(e.kind==='relative_delay'&&r.lag_frames!==undefined){ctx.globalAlpha=1;ctx.fillStyle='#f3ce7b';ctx.fillText(String(r.lag_frames)+'fr',xa+2,y+12)}}ctx.globalAlpha=1;
for(let ch=0;ch<2;ch++){const y=top+(6+ch)*step+step/2,scale=step*.38;ctx.strokeStyle='#4db8b2';for(const bin of D.pcm_bins[ch]){const a=bin.plot_t0,b=bin.plot_t1;if(b<low||a>high)continue;const xa=x(a),xb=x(b);if(bin.accepted<bin.count){ctx.fillStyle='#e7ad56';ctx.globalAlpha=.25;ctx.fillRect(xa,y-scale,Math.max(2,xb-xa),scale*2);ctx.globalAlpha=1}if(bin.minimum!==null){ctx.beginPath();const center=(xa+xb)/2;ctx.moveTo(center,y-bin.maximum*scale);ctx.lineTo(center,y-bin.minimum*scale);ctx.stroke();ctx.fillStyle='#4db8b2';ctx.fillRect(center-1,y-bin.maximum*scale,2,2)}}}ctx.restore();$('low').value=low;$('high').value=high;}
function range(a,b){if(!Number.isFinite(a)||!Number.isFinite(b)||b<=a)return;low=a;high=b;draw();if($('onlyrange').checked){page=0;renderTable()}}
function zoom(factor,at=.5){const span=(high-low)*factor,center=low+(high-low)*at;range(center-span*at,center+span*(1-at))}
$('in').onclick=()=>zoom(.5);$('out').onclick=()=>zoom(2);$('back').onclick=()=>range(low-(high-low)*.25,high-(high-low)*.25);$('forward').onclick=()=>range(low+(high-low)*.25,high+(high-low)*.25);$('reset').onclick=()=>range(...initial);$('apply').onclick=()=>range(Number($('low').value),Number($('high').value));$('search').oninput=()=>{page=0;renderTable()};$('onlyrange').onchange=()=>{page=0;renderTable()};$('previous').onclick=()=>{page--;renderTable()};$('next').onclick=()=>{page++;renderTable()};
let drag=null,moved=false;canvas.onpointerdown=e=>{drag={x:e.offsetX,low,high};moved=false;canvas.setPointerCapture(e.pointerId)};canvas.onpointermove=e=>{if(drag){const delta=(e.offsetX-drag.x)/(canvas.clientWidth-126)*(drag.high-drag.low);if(Math.abs(e.offsetX-drag.x)>3)moved=true;range(drag.low-delta,drag.high-delta)}const nearest=points.reduce((best,p)=>Math.hypot(p.x-e.offsetX,p.y-e.offsetY)<best.distance?{distance:Math.hypot(p.x-e.offsetX,p.y-e.offsetY),p}:best,{distance:24,p:null});$('hover').textContent=nearest.p?nearest.p.e.kind+' · '+exact(nearest.p.e)+' · '+nearest.p.e.label:'Plot offset seconds '+(low+(e.offsetX-110)/(canvas.clientWidth-126)*(high-low)).toPrecision(12)+'; exact event samples are in the table.'};canvas.onpointerup=e=>{drag=null;if(moved)return;const nearest=points.reduce((best,p)=>Math.hypot(p.x-e.offsetX,p.y-e.offsetY)<best.distance?{distance:Math.hypot(p.x-e.offsetX,p.y-e.offsetY),p}:best,{distance:24,p:null});if(nearest.p)select(nearest.p.e)};canvas.onpointercancel=()=>{drag=null};canvas.addEventListener('wheel',e=>{e.preventDefault();zoom(e.deltaY>0?1.25:.8,Math.max(0,Math.min(1,(e.offsetX-110)/(canvas.clientWidth-126))))},{passive:false});window.addEventListener('resize',draw);renderTable();draw();
</script></body></html>'''


def exact_data(value):
    """Keep all large integers exact in browser JSON, including unknown fields."""
    if isinstance(value, dict):
        return {str(key): exact_data(item) for key, item in value.items()}
    if isinstance(value, list):
        return [exact_data(item) for item in value]
    if isinstance(value, int) and not isinstance(value, bool) and abs(value) > (1 << 53) - 1:
        return str(value)
    return value


def build(args):
    builder = Builder(args)
    source = Path(args.input)
    if source.is_dir():
        builder.bundle(source)
    else:
        builder.sources.append({'path': str(source), 'kind': 'JSONL v1'})
        builder.json_input(source, True)
    for option in ('health', 'pcm', 'status_changes'):
        path = getattr(args, option)
        if path:
            builder.sources.append({'path': str(path), 'kind': option})
            builder.json_input(Path(path))
    if args.comparison:
        builder.sources.append({'path': str(args.comparison), 'kind': 'relative delay'})
        builder.comparison(Path(args.comparison))
    return exact_data(builder.result())


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('input', help='Version-1 decoded JSONL or four-CSV bundle directory')
    p.add_argument('-o', '--output', required=True, help='Standalone HTML file (atomic replacement)')
    p.add_argument('--title', default='S/PDIF capture report')
    for name in ('health', 'pcm', 'status-changes', 'comparison'):
        p.add_argument('--' + name, help='Optional version-1 analysis export')
    p.add_argument('--max-input-bytes', type=int, default=128 * 1024 * 1024)
    p.add_argument('--max-records', type=int, default=2_000_000)
    p.add_argument('--max-events', type=int, default=100_000)
    p.add_argument('--max-plot-bins', type=int, default=2048, help='Per-channel dense min/max bin budget')
    p.add_argument('--max-output-bytes', type=int, default=64 * 1024 * 1024)
    return p


def main(argv=None):
    p = parser()
    args = p.parse_args(argv)
    if not (1 <= args.max_input_bytes <= 2 ** 31 and 1 <= args.max_records <= 10_000_000 and
            1 <= args.max_events <= 1_000_000 and 2 <= args.max_plot_bins <= 100_000 and
            1 <= args.max_output_bytes <= 2 ** 31):
        p.error('Invalid input/record/event/plot/output budgets (see --help)')
    try:
        output = Path(args.output).resolve()
        inputs = [Path(args.input)] + [Path(getattr(args, n)) for n in ('health', 'pcm', 'status_changes', 'comparison') if getattr(args, n)]
        if any(output == value.resolve() or value.is_dir() and output.parent == value.resolve() for value in inputs):
            raise ReportError('Output must not overwrite an input or its bundle directory')
        with localcontext() as context:
            context.prec = 80
            data = build(args)
        prefix, suffix = HTML.split('__DATA__', 1)
        encoded = (prefix.replace('__TITLE__', html.escape(args.title, quote=True)) + safe_json(data) + suffix).encode('utf-8')
        if len(encoded) > args.max_output_bytes:
            raise ReportError('HTML output byte budget exceeded (--max-output-bytes)')
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode='wb', prefix=output.name + '.', suffix='.tmp', dir=output.parent, delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(encoded)
            os.replace(str(temporary), str(output))
        finally:
            if temporary is not None and temporary.exists():
                temporary.unlink()
        print('Wrote {}: {} subframes, {} sparse events, {} bytes'.format(output, data['stats']['subframes'], data['sparse_event_count'], len(encoded)))
        return 0
    except (OSError, ValueError, TypeError, KeyError, csv.Error, OverflowError, RecursionError) as error:
        print('capture_report: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
