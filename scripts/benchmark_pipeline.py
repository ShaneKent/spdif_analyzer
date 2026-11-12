"""Run reproducible fresh-process portable pipeline benchmarks (never opens Logic).

Generation/file input, shared decode+analysis, retained exports, bundle writing,
and online generator+pipeline costs are reported separately. Outputs are local.
"""
import argparse
import hashlib
import json
import os
import platform
import statistics
import subprocess
import time
from pathlib import Path


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024*1024), b''):
            h.update(block)
    return h.hexdigest()


def execute(binary, arguments, timeout):
    start = time.monotonic()
    result = subprocess.run([str(binary), *map(str, arguments)], capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f'benchmark failed ({result.returncode}): {arguments}\n{result.stderr}')
    rows = [json.loads(line) for line in result.stdout.splitlines() if line]
    if len(rows) != 1 or rows[0]['event'] != 'pipeline_benchmark':
        raise RuntimeError('Unexpected benchmark machine output')
    row = rows[0]
    row['process_wall_seconds'] = time.monotonic()-start
    return row


def summarize(runs):
    groups = {}
    for row in runs:
        key = (row['scenario'], row['nominal_rate_hz'], row['mode'], row.get('source', 'generated'), row['cancel_after_rows'])
        groups.setdefault(key, []).append(row)
    result = []
    for key, rows in groups.items():
        times = [r.get('decode_analysis_export_seconds', r.get('generation_and_pipeline_seconds', 0))+r['bundle_seconds'] for r in rows]
        median = statistics.median(times)
        mad = statistics.median(abs(t-median) for t in times)
        checksums = {r['result_checksum'] for r in rows}
        counts = {tuple(r[n] for n in ('edges', 'subframes', 'status_records', 'diagnostics', 'content_records', 'codec_records', 'codec_structure_passed', 'codec_crc_passed')) for r in rows}
        if len(checksums) != 1 or len(counts) != 1:
            raise AssertionError('Non-deterministic results: '+str(key))
        result.append(dict(scenario=key[0], nominal_rate_hz=key[1], mode=key[2], source=key[3], cancel_after_rows=key[4],
                           repetitions=len(rows), capture_rate_hz=rows[0]['capture_rate_hz'], capture_seconds=rows[0]['capture_seconds'],
                           median_pipeline_seconds=median, median_absolute_deviation_seconds=mad,
                           maximum_pipeline_seconds=max(times), minimum_pipeline_seconds=min(times),
                           median_peak_rss_bytes=statistics.median(r['peak_rss_bytes'] for r in rows),
                           result_checksum=rows[0]['result_checksum'], subframes=rows[0]['subframes'], edges=rows[0]['edges'],
                           retained_subframe_capacity_bytes=max(r['retained_subframe_capacity_bytes'] for r in rows),
                           peak_memory_method=rows[0].get('peak_memory_method', 'getrusage ru_maxrss (legacy)')))
    return result


def compare(report, baseline, factor, enforce):
    if baseline['host'] != report['host'] or baseline['build_label'] != report['build_label']:
        raise ValueError('Regression comparisons require the same host description and build label')
    old = {(r['scenario'], r['nominal_rate_hz'], r['mode'], r['source'], r['cancel_after_rows']): r for r in baseline['summary']}
    changes = []
    for row in report['summary']:
        key = tuple(row[n] for n in ('scenario', 'nominal_rate_hz', 'mode', 'source', 'cancel_after_rows'))
        if key not in old:
            continue
        reference = old[key]
        if row['capture_seconds'] != reference['capture_seconds'] or row['capture_rate_hz'] != reference['capture_rate_hz']:
            raise ValueError('Capture settings differ for '+str(key))
        if row['result_checksum'] != reference['result_checksum'] or row['subframes'] != reference['subframes']:
            raise AssertionError('Result fidelity differs; performance cannot mask dropped/changed output: '+str(key))
        if row['repetitions'] < 3 or reference['repetitions'] < 3:
            changes.append(dict(key=key, verdict='insufficient repetitions; advisory only'))
            continue
        # A 30% default effect floor plus six MADs is deliberately conservative.
        # Short/noisy shared-host measurements remain advisory unless requested.
        floor = max(0.010, reference['median_pipeline_seconds']*(factor-1),
                    6*reference['median_absolute_deviation_seconds'], 6*row['median_absolute_deviation_seconds'])
        slow = row['median_pipeline_seconds'] > reference['median_pipeline_seconds']+floor
        same_memory_method = row['peak_memory_method'] == reference['peak_memory_method']
        growth = same_memory_method and row['median_peak_rss_bytes'] > reference['median_peak_rss_bytes']*1.25+16*1024*1024
        changes.append(dict(key=key, time_regression=slow, memory_regression=growth, time_threshold_seconds=reference['median_pipeline_seconds']+floor))
        if enforce and (slow or growth):
            raise AssertionError('Measured performance regression: '+str(changes[-1]))
    return changes


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('binary', type=Path)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--rates', type=int, nargs='+', default=[48000, 96000, 192000])
    p.add_argument('--seconds', type=float, default=.1)
    p.add_argument('--repeats', type=int, default=3)
    p.add_argument('--scenarios', nargs='+', choices=['clean', 'faults', 'transitions', 'burst'], default=['clean', 'faults', 'transitions', 'burst'])
    p.add_argument('--modes', nargs='+', choices=['analysis', 'exports'], default=['analysis', 'exports'])
    p.add_argument('--max-fixture-bytes', type=int, default=512*1024*1024)
    p.add_argument('--long-seconds', type=float, default=0)
    p.add_argument('--timeout', type=float, default=300)
    p.add_argument('--keep-fixtures', action='store_true')
    p.add_argument('--keep-outputs', action='store_true')
    p.add_argument('--build-label', default='Release')
    p.add_argument('--compare', type=Path)
    p.add_argument('--regression-factor', type=float, default=1.30)
    p.add_argument('--enforce', action='store_true')
    a = p.parse_args()
    if a.repeats < 1 or a.seconds <= 0 or a.regression_factor <= 1:
        p.error('Invalid repetitions, duration, or regression threshold')
    a.binary = a.binary.resolve()
    a.output.mkdir(parents=True, exist_ok=False)
    report = dict(schema_version=1, binary=str(a.binary), binary_sha256=digest(a.binary), build_label=a.build_label,
                  host=dict(system=platform.system(), release=platform.release(), machine=platform.machine(), cpu_count=os.cpu_count(), output_filesystem_device=os.stat(a.output).st_dev),
                  settings=vars(a).copy(), generation=[], runs=[], summary=[], result='INCOMPLETE')
    report['settings'] = {k: str(v) if isinstance(v, Path) else v for k, v in report['settings'].items()}
    path = a.output/'report.json'
    path.write_text(json.dumps(report, indent=2)+'\n')
    import shutil
    for rate in a.rates:
        for scenario in a.scenarios:
            fixture = a.output/f'{rate}-{scenario}.edges'
            generation = execute(a.binary, ['--generate', fixture, '--rate', rate, '--seconds', a.seconds,
                                           '--scenario', scenario, '--max-fixture-bytes', a.max_fixture_bytes], a.timeout)
            generation['fixture_sha256'] = digest(fixture)
            report['generation'].append(generation)
            for repetition in range(a.repeats):
                for mode in a.modes:
                    output = a.output/f'{rate}-{scenario}-{mode}-{repetition}'
                    args = ['--input', fixture, '--mode', mode, '--max-fixture-bytes', a.max_fixture_bytes]
                    if mode == 'exports':
                        args += ['--output', output]
                    row = execute(a.binary, args, a.timeout)
                    row['repetition'] = repetition
                    report['runs'].append(row)
                    if mode == 'exports' and not a.keep_outputs:
                        shutil.rmtree(output)
                    print(rate, scenario, mode, repetition, round(row['process_wall_seconds'], 3), 's', flush=True)
            if 'exports' in a.modes:
                output = a.output/f'{rate}-{scenario}-cancelled'
                row = execute(a.binary, ['--input', fixture, '--mode', 'exports', '--output', output,
                                         '--max-fixture-bytes', a.max_fixture_bytes, '--cancel-after', 100], a.timeout)
                assert not row['bundle_complete']
                manifest = json.loads((output/'bundle'/'manifest.json').read_text())
                assert not manifest['complete'] and (output/'bundle'/'INCOMPLETE').exists()
                report['runs'].append(row)
                if not a.keep_outputs:
                    shutil.rmtree(output)
            if not a.keep_fixtures:
                fixture.unlink()
            path.write_text(json.dumps(report, indent=2)+'\n')
    if a.long_seconds:
        # No edge file or retained history: this measures generation+pipeline,
        # alongside a separate generator-only cost, not decoder-only throughput.
        for mode in ('generator', 'analysis'):
            print('Starting online', mode, a.long_seconds, 'capture seconds', flush=True)
            row = execute(a.binary, ['--online', mode, '--rate', a.rates[0], '--scenario', 'transitions', '--seconds', a.long_seconds], max(a.timeout, a.long_seconds*20))
            report['runs'].append(row)
            path.write_text(json.dumps(report, indent=2)+'\n')
            print('Finished online', mode, round(row['process_wall_seconds'], 3), 's', flush=True)
    report['summary'] = summarize(report['runs'])
    # Verify exports never change the shared decoder result compared with analysis.
    equal = {}
    for row in report['runs']:
        if row['mode'] == 'generator':
            continue
        key = (row['scenario'], row['nominal_rate_hz'], row['capture_seconds'])
        value = (row['result_checksum'], row['subframes'], row['edges'], row['codec_records'], row['codec_structure_passed'], row['codec_crc_passed'])
        if key in equal:
            assert equal[key] == value, 'Analysis/export fidelity mismatch'
        equal[key] = value
    report['regressions'] = compare(report, json.loads(a.compare.read_text()), a.regression_factor, a.enforce) if a.compare else []
    report['result'] = 'PASS'
    path.write_text(json.dumps(report, indent=2)+'\n')
    print('Report:', path)


if __name__ == '__main__':
    main()
