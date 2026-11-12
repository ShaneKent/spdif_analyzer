"""Verify v0.2 exports from saved independent QA fixtures.

Requires logic2-automation only for acquisition. --verify-only reads collected
artifacts without opening Logic or acquiring hardware. --resume reuses complete
exports and refuses to overwrite failed or incomplete artifacts.
"""
import argparse
import csv
import decimal
import json
import math
import struct
import time
from pathlib import Path

FIXTURES = ['pcm', 'changes', 'ac3', 'dts', 'professional', 'partial']
DATASETS = {'subframes.csv', 'channel-status.csv', 'diagnostics.csv', 'content.csv'}
EXCLUSIONS = ['excluded_parity', 'excluded_invalid', 'excluded_unknown', 'excluded_non_pcm', 'excluded_layout']


def jsonl(path):
    assert path.is_file(), path
    return [json.loads(line) for line in path.read_text().splitlines() if line]


def csv_rows(path):
    assert path.is_file(), path
    with path.open(newline='') as stream:
        return list(csv.DictReader(stream))


def sample_at(seconds, manifest):
    rate = decimal.Decimal(str(manifest['capture_rate']))
    return int((decimal.Decimal(seconds)*rate).to_integral_value())+int(manifest['settings']['trigger_sample'])


def row_interval(row, manifest):
    return sample_at(row['start_seconds'], manifest), sample_at(row['end_seconds'], manifest)


def check_context(record, manifest):
    assert record['schema_version'] == 1
    assert record['analyzer_version'] == manifest['analyzer_version']
    assert record['capture_rate_hz'] == manifest['capture_rate']
    assert record['input_channel'] == manifest['input_channel']
    assert int(record['origin_sample']) == int(manifest['coverage_start_sample'])
    assert int(record['coverage_start_sample']) == int(manifest['coverage_start_sample'])
    assert int(record['coverage_end_sample_inclusive']) == int(manifest['coverage_end_sample'])
    assert record['settings']['analysis_window_ms'] == manifest['settings']['analysis_window_ms']
    assert record['live_snapshot'] is True


def check_bundle(directory):
    assert directory.is_dir(), directory
    manifest = json.loads((directory/'manifest.json').read_text())
    required = {'schema_version', 'analyzer_version', 'input_channel', 'capture_rate', 'settings',
                'coverage_start_sample', 'coverage_end_sample', 'complete', 'state', 'datasets'}
    assert required <= manifest.keys()
    assert manifest['schema_version'] == '1' and manifest['analyzer_version'] == '0.2.0'
    assert manifest['input_channel'] == 'D0' and manifest['capture_rate'] > 0
    assert manifest['complete'] is True and manifest['state'] == 'complete'
    assert not (directory/'INCOMPLETE').exists()
    assert len(manifest['datasets']) == 4
    assert {item['file'] for item in manifest['datasets']} == DATASETS
    settings = manifest['settings']
    assert {'device_id', 'trigger_sample', 'sample_display', 'csv_export', 'export_output',
            'nominal_rate', 'waveform_detail', 'analysis_window_ms', 'pcm_bits_override',
            'wav_rate_override', 'assume_stereo'} <= settings.keys()
    begin, end = int(manifest['coverage_start_sample']), int(manifest['coverage_end_sample'])
    assert begin <= end
    rows = {}
    for item in manifest['datasets']:
        data = csv_rows(directory/item['file'])
        assert len(data) == item['snapshot_rows'] == item['written_rows'], item
        for row in data:
            start, stop = row_interval(row, manifest)
            assert begin <= start <= stop <= end, (item['file'], row, manifest)
        rows[item['file']] = data
    previous = -1
    for row in rows['subframes.csv']:
        start, stop = row_interval(row, manifest)
        assert start > previous, 'Overlapping or repeated source subframes'
        previous = stop
    return manifest, rows


def directory_index(directory):
    assert directory.is_dir(), directory
    records = jsonl(directory/'index.jsonl')
    assert records and records[0]['type'] == 'configuration'
    assert records[-1]['type'] == 'finish' and records[-1]['complete'] is True
    assert not (directory/'INCOMPLETE').exists()
    assert not list(directory.glob('*.part'))
    return records


def precision(row, override):
    if row['layout_known'] == '1':
        return int(row['layout_bits'])
    return int(override)


def valid_pcm(row, bits_override=0):
    bits = precision(row, bits_override)
    return (row['content'] == 'PCM' and row['parity_ok'] == '1' and row['V_invalid'] == '0'
            and 16 <= bits <= 24)


def check_wav(directory, manifest, subframes, assume_stereo=False):
    records = directory_index(directory)
    config = records[0]
    assert config['container_bits'] == 24 and config['assume_stereo'] is assume_stereo
    assert config['capture_rate'] == manifest['capture_rate']
    source = {row_interval(row, manifest)[0]: i for i, row in enumerate(subframes)}
    assert len(source) == len(subframes)
    assigned = set()
    segments = [r for r in records if r['type'] == 'segment']
    frames = 0
    filenames = []
    for segment in segments:
        filenames.append(segment['file'])
        data = (directory/segment['file']).read_bytes()
        assert data[:4] == b'RIFF' and data[8:16] == b'WAVEfmt '
        assert struct.unpack_from('<I', data, 4)[0]+8 == len(data)
        assert struct.unpack_from('<HHI', data, 20) == (0xfffe, 2, 48000)
        assert struct.unpack_from('<HH', data, 34) == (24, 22)
        assert struct.unpack_from('<H', data, 38)[0] == segment['valid_bits']
        assert struct.unpack_from('<I', data, 40)[0] == 3
        assert data[44:60] == bytes.fromhex('0100000000001000800000aa00389b71')
        assert data[60:64] == b'data'
        assert struct.unpack_from('<I', data, 64)[0] == segment['frames']*6
        assert len(data) == 68+segment['frames']*6
        start = source[int(segment['start_sample'])]
        for n in range(segment['frames']):
            for channel in (0, 1):
                index = start+2*n+channel
                assert index not in assigned, 'WAV source interval exported twice'
                assigned.add(index)
                row = subframes[index]
                assert row['channel'] == ('A' if channel == 0 else 'B')
                assert valid_pcm(row, config['bits_override']), row
                assert row['layout_stereo'] == '1' or assume_stereo
                assert precision(row, config['bits_override']) == segment['valid_bits']
                if channel:
                    assert row_interval(subframes[index-1], manifest)[1]+1 == row_interval(row, manifest)[0]
                actual = int.from_bytes(data[68+6*n+3*channel:71+6*n+3*channel], 'little', signed=True)
                shift = 24-segment['valid_bits']
                expected = (int(row['signed_container_24']) >> shift) << shift
                assert actual == expected, (segment, n, channel, actual, expected, row)
        assert int(segment['end_sample']) == row_interval(subframes[start+2*segment['frames']-1], manifest)[1]
        frames += segment['frames']
    assert len(filenames) == len(set(filenames))
    assert {p.name for p in directory.glob('*.wav')} == set(filenames)
    for excluded in (r for r in records if r['type'] == 'excluded'):
        index = source[int(excluded['start_sample'])]
        assert index not in assigned, 'WAV excluded source interval duplicated or also written'
        assigned.add(index)
        row = subframes[index]
        assert row['channel'] == ('B' if excluded['channel'] else 'A')
        assert int(excluded['end_sample']) == row_interval(row, manifest)[1]
        assert excluded['reason']
    assert assigned == set(range(len(subframes))), 'WAV export silently omitted source subframes'
    # These fixtures have no faults, gaps, or asymmetric precision. Every
    # eligible adjacent A/B pair must be exported, rather than merely some pair.
    expected_pairs = 0
    for i in range(len(subframes)-1):
        a, b = subframes[i:i+2]
        if a['channel'] != 'A' or b['channel'] != 'B':
            continue
        if all(valid_pcm(row, config['bits_override']) and (row['layout_stereo'] == '1' or assume_stereo)
               for row in (a, b)) and precision(a, config['bits_override']) == precision(b, config['bits_override']):
            if row_interval(a, manifest)[1]+1 == row_interval(b, manifest)[0]:
                expected_pairs += 1
    assert frames == expected_pairs, (frames, expected_pairs)
    return segments, frames


def check_statistics(path, manifest, samples):
    records = jsonl(path)
    assert records[0]['event'] == 'metadata'
    check_context(records[0], manifest)
    windows = [r for r in records[1:] if r['event'] == 'pcm_window']
    assert len(windows) == len(records)-1
    assert len({(r['channel'], r['segment']) for r in windows}) == len(windows)
    starts = [row_interval(r, manifest)[0] for r in samples]
    assigned = set()
    total_accepted = 0
    override = records[0]['settings']['pcm_bits_override']
    for window in windows:
        selected = [i for i, (at, row) in enumerate(zip(starts, samples))
                    if int(window['start_sample']) <= at < int(window['end_sample_exclusive'])
                    and row['channel'] == ('B' if window['channel'] else 'A')]
        assert not assigned.intersection(selected), 'PCM history and current snapshots overlap'
        assigned.update(selected)
        assert len(selected) == window['accepted']+sum(window[k] for k in EXCLUSIONS)
        exclusions = dict.fromkeys(EXCLUSIONS, 0)
        for index in selected:
            row = samples[index]
            if row['parity_ok'] != '1':
                exclusions['excluded_parity'] += 1
            elif row['V_invalid'] != '0':
                exclusions['excluded_invalid'] += 1
            elif row['content'] == 'Unknown' or row['content'].startswith(('Provisional', 'Conflict')):
                exclusions['excluded_unknown'] += 1
            elif row['content'] != 'PCM':
                exclusions['excluded_non_pcm'] += 1
            elif not valid_pcm(row, override):
                exclusions['excluded_layout'] += 1
        assert all(window[key] == value for key, value in exclusions.items()), (window, exclusions)
        values = [int(samples[i]['signed_container_24']) >> (24-window['bits']) for i in selected
                  if valid_pcm(samples[i], override) and precision(samples[i], override) == window['bits']]
        assert len(values) == window['accepted'], (window, len(values))
        total_accepted += len(values)
        if not values:
            continue
        assert min(values) == window['minimum'] and max(values) == window['maximum']
        peak = max(abs(v) for v in values)
        assert peak == window['peak']
        assert math.isclose(sum(values)/len(values), window['mean'], rel_tol=1e-12, abs_tol=1e-9)
        rms = math.sqrt(sum(v*v for v in values)/len(values))
        assert math.isclose(rms, window['rms'], rel_tol=1e-12)
        full = 2**(window['bits']-1)
        assert math.isclose(peak/full, window['peak_normalized'], rel_tol=1e-12)
        assert math.isclose(rms/full, window['rms_normalized'], rel_tol=1e-12)
        assert sum(v == 0 for v in values) == window['silence_samples']
        assert sum(v in (-full, full-1) for v in values) == window['full_scale_samples']
    assert assigned == set(range(len(samples))), 'PCM statistics omit source samples'
    assert total_accepted == sum(valid_pcm(row, override) for row in samples)
    assert windows and total_accepted > 0
    return windows


def check_payloads(directory, name, manifest, samples):
    records = directory_index(directory)
    candidates = [r for r in records if r['type'] == 'burst']
    accepted = [r for r in candidates if r['accepted']]
    source = {row_interval(row, manifest)[0]: i for i, row in enumerate(samples)}
    if name == 'partial':
        assert candidates and not accepted and not list(directory.glob('*.bin'))
        assert any((r['Pc'] & 127) == 1 and not r['complete'] for r in candidates)
    else:
        assert accepted
        expected_type = 1 if name == 'ac3' else 11
        expected_format = 'AC-3 (Dolby Digital)' if name == 'ac3' else 'DTS type I'
        previous = None
        accepted_starts = []
        expected_words = [0xf872, 0x4e1f, expected_type, 64, 0x0b77, 0x1234, 0x5678, 0x9abc]
        source_words = [(int(row['raw_slots_4_31'], 0) >> 8) & 65535 for row in samples]
        complete_headers = [i for i in range(len(samples)-7) if samples[i]['channel'] == 'A'
                            and source_words[i:i+8] == expected_words]
        for record in accepted:
            assert record['format'] == expected_format and (record['Pc'] & 127) == expected_type
            assert record['Pd'] == 64 and record['stream_number'] == 0
            assert record['complete'] is True and record['integrity'] is True and record['evidence']
            assert record['declared_bits'] == record['observed_bits'] == 64
            assert record['payload_words'] == 4 and record['final_word_valid_bits'] == 16
            assert (directory/record['file']).read_bytes() == bytes.fromhex('0b77123456789abc')
            start = source[int(record['start_sample'])]
            accepted_starts.append(start)
            actual_words = [(int(row['raw_slots_4_31'], 0) >> 8) & 65535 for row in samples[start:start+8]]
            assert actual_words == [0xf872, 0x4e1f, expected_type, 64, 0x0b77, 0x1234, 0x5678, 0x9abc]
            assert int(record['end_sample']) == row_interval(samples[start+7], manifest)[1]
            if previous is not None:
                assert start-previous == 2*(1536 if name == 'ac3' else 512)
            previous = start
        assert accepted_starts == complete_headers, 'Complete source bursts omitted or duplicated'
        assert all(r['V_invalid'] == '1' for r in samples)
    files = [r['file'] for r in accepted]
    assert len(files) == len(set(files))
    assert {p.name for p in directory.glob('*.bin')} == set(files)
    for record in candidates:
        if not record['accepted']:
            assert record['file'] is None and record['reason']
    return accepted, candidates


def verify_fixture(name, output):
    assert not list(output.glob('*.error.json')), 'Fixture has an explicit export error report'
    manifest, datasets = check_bundle(output/'analysis.bundle')
    samples = datasets['subframes.csv']
    assert samples and all(r['parity_ok'] == '1' for r in samples)
    result = {'subframes': len(samples), 'status_records': len(datasets['channel-status.csv'])}
    if name in ('pcm', 'changes'):
        health = json.loads((output/'health.json').read_text())
        check_context(health, manifest)
        assert sum(c['decoded_subframes'] for c in health['channels']) == len(samples)
        assert all(c['parity_failures'] == c['V_invalid'] == 0 for c in health['channels'])
        assert health['health_windows']
        windows = check_statistics(output/'pcm.jsonl', manifest, samples)
        segments, count = check_wav(output/'audio.wav-export', manifest, samples)
        assert count > 0
        result.update(wav_frames=count, wav_segments=len(segments), pcm_windows=len(windows))
        if name == 'changes':
            changes = jsonl(output/'changes.jsonl')
            assert changes[0]['event'] == 'metadata'
            check_context(changes[0], manifest)
            events = [r for r in changes[1:] if r['event'] == 'status_change']
            assert len(events) == len(changes)-1 == 4
            assert sorted(r['channel'] for r in events) == [0, 0, 1, 1]
            assert all(bytes.fromhex(r['changed_mask_hex']) == bytes([12, 0, 0, 0, 1])+bytes(19) for r in events)
            assert {r['valid_bits'] for r in segments} == {20, 24}
            result['status_changes'] = len(events)
    elif name in ('ac3', 'dts', 'partial'):
        accepted, candidates = check_payloads(output/'payload.bursts', name, manifest, samples)
        segments, count = check_wav(output/'audio.wav-export', manifest, samples)
        assert not segments and count == 0
        result.update(accepted_payloads=len(accepted), rejected_payloads=len(candidates)-len(accepted))
    else:
        assert datasets['channel-status.csv'] and all('professional' in r['interpretation'] for r in datasets['channel-status.csv'])
        segments, count = check_wav(output/'audio.wav-export', manifest, samples)
        assert not segments and count == 0
        segments, count = check_wav(output/'stereo.wav-export', manifest, samples, assume_stereo=True)
        assert segments and count > 0
        result['explicit_stereo_frames'] = count
    return result


def reusable_export(kind, path):
    assert not Path(str(path)+'.error.json').exists(), 'Previous export failed: '+str(path)
    suffix = {'Analysis CSV bundle': '.bundle', 'PCM WAV': '.wav-export', 'IEC61937 payloads': '.bursts'}.get(kind)
    target = Path(str(path)+suffix) if suffix else path
    if not target.exists():
        assert not suffix or not path.exists(), 'Existing incomplete export base file: '+str(path)
        return False
    if suffix == '.bundle':
        check_bundle(target)
    elif suffix:
        directory_index(target)
    elif kind == 'Health summary JSON':
        obj = json.loads(path.read_text())
        assert obj['event'] == 'summary' and 'health_windows' in obj
    else:
        records = jsonl(path)
        assert records and records[0]['event'] == 'metadata'
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixtures', type=Path, nargs='?', default=Path('.'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--port', type=int, default=10431)
    parser.add_argument('--only', nargs='+', choices=FIXTURES)
    parser.add_argument('--verify-only', action='store_true')
    parser.add_argument('--resume', action='store_true')
    args = parser.parse_args()
    args.output = args.output.resolve()
    names = args.only or FIXTURES
    if args.verify_only:
        report = {'fixtures': {}, 'verified_fixtures': names, 'result': 'INCOMPLETE', 'verification': 'offline artifacts; no Logic connection'}
        for name in names:
            report['fixtures'][name] = verify_fixture(name, args.output/name)
            print(name, report['fixtures'][name], flush=True)
        report['result'] = 'PASS'
        (args.output/'verification-report.json').write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(report, indent=2))
        return
    from saleae import automation  # No dependency or app calls in --verify-only.
    args.output.mkdir(parents=True, exist_ok=args.resume)
    report = {'fixtures': {}, 'result': 'INCOMPLETE'}
    with automation.Manager.connect(port=args.port) as manager:
        info = manager.get_app_info()
        report['logic_version'] = info.app_version
        report['app_pid'] = info.app_pid
        for name in names:
            with manager.load_capture(str((args.fixtures/(name+'.sal')).resolve())) as capture:
                output = args.output/name
                output.mkdir(exist_ok=args.resume)

                def export(kind, filename, **settings):
                    path = output/filename
                    if args.resume and reusable_export(kind, path):
                        print('Reusing', name, kind, flush=True)
                        return path
                    options = {'S/PDIF': 0, 'Waveform detail': 'Samples',
                               'Analysis window (ms)': 10, 'Export output': kind}
                    options.update(settings)
                    setup_started = time.monotonic()
                    print('Configuring', name, kind, flush=True)
                    analyzer = capture.add_analyzer('S/PDIF Analyzer', settings=options)
                    print('Configured', name, kind, round(time.monotonic()-setup_started, 2), 's', flush=True)
                    started = time.monotonic()
                    print('Exporting', name, kind, flush=True)
                    capture.legacy_export_analyzer(str(path), analyzer, automation.RadixType.HEXADECIMAL)
                    capture.remove_analyzer(analyzer)
                    assert not Path(str(path)+'.error.json').exists(), Path(str(path)+'.error.json').read_text()
                    print('Finished', name, kind, round(time.monotonic()-started, 2), 's', flush=True)
                    return path

                export('Analysis CSV bundle', 'analysis')
                if name in ('pcm', 'changes'):
                    export('Health summary JSON', 'health.json')
                    export('PCM statistics JSONL', 'pcm.jsonl')
                    export('PCM WAV', 'audio')
                    if name == 'changes':
                        export('Status changes JSONL', 'changes.jsonl')
                elif name in ('ac3', 'dts', 'partial'):
                    export('IEC61937 payloads', 'payload')
                    export('PCM WAV', 'audio')
                else:
                    export('PCM WAV', 'audio')
                    export('PCM WAV', 'stereo', **{'Assume A/B stereo': True})
                report['fixtures'][name] = verify_fixture(name, output)
                (args.output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
                print(name, report['fixtures'][name], flush=True)
    report['result'] = 'PASS'
    (args.output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
