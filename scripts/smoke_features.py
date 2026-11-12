"""Validate v0.2 exports in a running Logic instance, using saved QA fixtures.

Requires logic2-automation and six .sal files generated with SpdifTestSignal:
pcm, ac3, dts, changes, professional, partial. No hardware is acquired. Use a
separate Logic profile; this creates and closes new tabs in the selected app.
The test signal module is a QA build target and is excluded from release ZIPs.
"""
import argparse
import csv
import decimal
import json
import math
import struct
import time
from pathlib import Path
from saleae import automation


def jsonl(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line]


def csv_rows(path):
    with path.open(newline='') as stream:
        return list(csv.DictReader(stream))


def check_bundle(directory):
    manifest = json.loads((directory/'manifest.json').read_text())
    assert manifest['complete'] and not (directory/'INCOMPLETE').exists()
    rows = {}
    for item in manifest['datasets']:
        data = csv_rows(directory/item['file'])
        assert len(data) == item['snapshot_rows'] == item['written_rows'], item
        rows[item['file']] = data
    return manifest, rows


def check_wav(directory, manifest, subframes):
    records = jsonl(directory/'index.jsonl')
    assert records[-1]['complete'] and not (directory/'INCOMPLETE').exists()
    rate = decimal.Decimal(str(manifest['capture_rate']))
    trigger = int(manifest['settings']['trigger_sample'])
    source = {int((decimal.Decimal(row['start_seconds'])*rate).to_integral_value())+trigger: i
              for i, row in enumerate(subframes)}
    segments = [r for r in records if r['type'] == 'segment']
    count = 0
    for segment in segments:
        data = (directory/segment['file']).read_bytes()
        assert data[:4] == b'RIFF' and data[8:16] == b'WAVEfmt '
        assert struct.unpack_from('<I', data, 4)[0]+8 == len(data)
        assert struct.unpack_from('<HHI', data, 20) == (0xfffe, 2, 48000)
        assert struct.unpack_from('<HH', data, 34) == (24, 22)
        assert struct.unpack_from('<H', data, 38)[0] == segment['valid_bits']
        assert data[60:64] == b'data'
        assert struct.unpack_from('<I', data, 64)[0] == segment['frames']*6
        start = source[int(segment['start_sample'])]
        for n in range(segment['frames']):
            for channel in (0, 1):
                row = subframes[start+2*n+channel]
                assert row['channel'] == ('A' if channel == 0 else 'B')
                assert row['parity_ok'] == '1' and row['V_invalid'] == '0'
                actual = int.from_bytes(data[68+6*n+3*channel:71+6*n+3*channel], 'little', signed=True)
                assert actual == int(row['signed_container_24']), (segment, n, channel, actual, row)
        count += segment['frames']
    return segments, count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixtures', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--port', type=int, default=10431)
    parser.add_argument('--only', nargs='+', choices=['pcm', 'changes', 'ac3', 'dts', 'professional', 'partial'])
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    report = {'fixtures': {}, 'result': 'INCOMPLETE'}
    with automation.Manager.connect(port=args.port) as manager:
        info = manager.get_app_info()
        report['logic_version'] = info.app_version
        report['app_pid'] = info.app_pid
        for name in args.only or ['pcm', 'changes', 'ac3', 'dts', 'professional', 'partial']:
            with manager.load_capture(str((args.fixtures/(name+'.sal')).resolve())) as capture:
                output = args.output/name
                output.mkdir()

                def export(kind, filename, **settings):
                    options = {'S/PDIF': 0, 'Waveform detail': 'Samples',
                               'Analysis window (ms)': 10, 'Export output': kind}
                    options.update(settings)
                    setup_started = time.monotonic()
                    print('Configuring', name, kind, flush=True)
                    analyzer = capture.add_analyzer('S/PDIF Analyzer', settings=options)
                    print('Configured', name, kind, round(time.monotonic()-setup_started, 2), 's', flush=True)
                    path = output/filename
                    started = time.monotonic()
                    print('Exporting', name, kind, flush=True)
                    capture.legacy_export_analyzer(str(path), analyzer, automation.RadixType.HEXADECIMAL)
                    capture.remove_analyzer(analyzer)
                    assert not Path(str(path)+'.error.json').exists(), Path(str(path)+'.error.json').read_text()
                    print('Finished', name, kind, round(time.monotonic()-started, 2), 's', flush=True)
                    return path

                bundle = Path(str(export('Analysis CSV bundle', 'analysis'))+'.bundle')
                manifest, datasets = check_bundle(bundle)
                samples = datasets['subframes.csv']
                assert samples and all(r['parity_ok'] == '1' for r in samples)
                result = {'subframes': len(samples), 'status_records': len(datasets['channel-status.csv'])}
                if name in ('pcm', 'changes'):
                    health = json.loads(export('Health summary JSON', 'health.json').read_text())
                    assert health['capture_rate_hz'] == manifest['capture_rate']
                    assert sum(c['decoded_subframes'] for c in health['channels']) == len(samples)
                    assert all(c['parity_failures'] == c['V_invalid'] == 0 for c in health['channels'])
                    assert health['health_windows'], 'Completed windows must be exported'
                    statistics = jsonl(export('PCM statistics JSONL', 'pcm.jsonl'))
                    assert statistics[0]['event'] == 'metadata'
                    windows = [r for r in statistics if r['event'] == 'pcm_window' and r['accepted']]
                    assert windows and all(math.isfinite(r['rms']) and r['rms'] >= 0 for r in windows)
                    capture_rate = decimal.Decimal(str(manifest['capture_rate']))
                    trigger = int(manifest['settings']['trigger_sample'])
                    starts = [int((decimal.Decimal(r['start_seconds'])*capture_rate).to_integral_value())+trigger
                              for r in samples]
                    for window in windows:
                        values = [int(row['signed_container_24']) >> (24-window['bits'])
                                  for at, row in zip(starts, samples)
                                  if int(window['start_sample']) <= at < int(window['end_sample_exclusive'])
                                  and row['channel'] == ('B' if window['channel'] else 'A')
                                  and row['content'] == 'PCM' and row['layout_known'] == '1'
                                  and int(row['layout_bits']) == window['bits']]
                        assert len(values) == window['accepted'], (window, len(values))
                        assert min(values) == window['minimum'] and max(values) == window['maximum']
                        assert math.isclose(sum(values)/len(values), window['mean'], rel_tol=1e-12, abs_tol=1e-9)
                        assert math.isclose(math.sqrt(sum(v*v for v in values)/len(values)), window['rms'], rel_tol=1e-12)
                    wav = Path(str(export('PCM WAV', 'audio'))+'.wav-export')
                    segments, count = check_wav(wav, manifest, samples)
                    assert count > 0
                    result.update(wav_frames=count, wav_segments=len(segments), pcm_windows=len(windows))
                    if name == 'changes':
                        changes = jsonl(export('Status changes JSONL', 'changes.jsonl'))
                        assert changes[0]['event'] == 'metadata'
                        events = [r for r in changes if r['event'] == 'status_change']
                        assert len(events) == 4 and sorted(r['channel'] for r in events) == [0, 0, 1, 1]
                        assert all(bytes.fromhex(r['changed_mask_hex']) == bytes([12, 0, 0, 0, 1])+bytes(19) for r in events)
                        assert {r['valid_bits'] for r in segments} == {20, 24}
                        result['status_changes'] = len(events)
                elif name in ('ac3', 'dts', 'partial'):
                    bursts = Path(str(export('IEC61937 payloads', 'payload'))+'.bursts')
                    records = jsonl(bursts/'index.jsonl')
                    assert records[-1]['complete'] and not (bursts/'INCOMPLETE').exists()
                    candidates = [r for r in records if r['type'] == 'burst']
                    accepted = [r for r in candidates if r['accepted']]
                    if name == 'partial':
                        assert candidates and not accepted and not list(bursts.glob('*.bin'))
                    else:
                        assert accepted
                        for record in accepted:
                            assert (bursts/record['file']).read_bytes() == bytes.fromhex('0b77123456789abc')
                            assert record['declared_bits'] == record['observed_bits'] == 64
                        assert all(r['V_invalid'] == '1' for r in samples)
                    wav = Path(str(export('PCM WAV', 'audio'))+'.wav-export')
                    assert not list(wav.glob('*.wav'))
                    result.update(accepted_payloads=len(accepted), rejected_payloads=len(candidates)-len(accepted))
                else:
                    blocks = datasets['channel-status.csv']
                    assert blocks and all('professional' in r['interpretation'] for r in blocks)
                    wav = Path(str(export('PCM WAV', 'audio'))+'.wav-export')
                    assert not list(wav.glob('*.wav')), 'Independent channels require explicit stereo assumption'
                    wav = Path(str(export('PCM WAV', 'stereo', **{'Assume A/B stereo': True}))+'.wav-export')
                    segments, count = check_wav(wav, manifest, samples)
                    assert segments and count > 0
                    result['explicit_stereo_frames'] = count
                report['fixtures'][name] = result
                (args.output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
                print(name, result, flush=True)
    report['result'] = 'PASS'
    (args.output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
