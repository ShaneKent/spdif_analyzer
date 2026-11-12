"""Collect/verify native codec exports from saved captures in an isolated Logic.

Use --manifest JSON --output NEWDIR --expected-pid PID for collection. A manifest
explicitly supplies physical/synthetic provenance, capture and CLI reference
paths, channels and independent carrier/metadata evidence. --verify-only never
imports Saleae automation or connects to an app. No acquisition is performed.
--sparse-table explicitly omits per-subframe FrameV2 rows, while preserving all
legacy samples/waveform data and codec/status notifications.

Manifest schema (all paths relative to the manifest):
  {"schema_version":1,"analyzer_version":"0.3.0","cases":[
    {"name":"ac3","provenance":"physical","capture":"capture.sal",
     "capture_rate_hz":100000000,
     "channels":[{"channel":6,"cli_jsonl":"D6.jsonl",
                  "expected_accepted_payloads":8}],
     "independent":{"carrier":"source.spdif","source_receipt":"receipt.json",
       "validation":"independent-validation.json","Pc":1,"period_frames":1536,
       "expected_codec_fields":{"codec":"AC-3","packing":"16-bit big endian",
         "header_state":"ok","structure":"passed","sample_rate_hz":48000,
         "channels":2,"channel_count_known":true,"frame_bytes":768,
         "samples_per_frame":1536,"crc1":"passed","header_crc":"not_applicable",
         "frame_crc":"passed","bit_rate":192000}}}]}
Expected metadata must come from independent encoder/probe evidence, not the
native export under test. DTS-I uses Pc11/period512. A synthetic_partial case
has no independent carrier and may omit cli_jsonl; it must produce an explicit
rejected header and truncated snapshot-prefix codec record, never valid audio.
Source receipts bind carrier SHA, finite playback and capture clock. Independent
validation records bind the same SHA, source_payload_frames, exact source-match
boolean, FFmpeg return code and decode/CRC errors. Physical cases require at
least one positive expected accepted-payload channel; negative channels use 0.
"""
import argparse
import csv
from decimal import Decimal, ROUND_HALF_UP, localcontext
import hashlib
import json
from pathlib import Path
import re
import struct
import time

DETAILS = ('Samples', 'Fields')
MAX_BYTES = 128 * 1024 * 1024
MAX_RECORDS = 250000


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024*1024), b''):
            h.update(block)
    return h.hexdigest()


def bounded(path):
    path = Path(path)
    require(path.is_file() and path.stat().st_size <= MAX_BYTES, 'Missing/oversized artifact: '+str(path))
    return path


def jsonl(path):
    result = []
    with bounded(path).open(encoding='utf-8-sig') as stream:
        for line in stream:
            if line.strip():
                require(len(result) < MAX_RECORDS, 'Record budget exceeded: '+str(path))
                result.append(json.loads(line))
    return result


def rows(path):
    with bounded(path).open(newline='', encoding='utf-8-sig') as stream:
        result = []
        for row in csv.DictReader(stream):
            require(len(result) < MAX_RECORDS, 'CSV record budget exceeded')
            result.append(row)
    return result


def resolve(base, value):
    return (base/Path(value)).resolve()


def load_manifest(path):
    path = bounded(path).resolve()
    data = json.loads(path.read_text())
    require(data['schema_version'] == 1 and data['cases'], 'Unsupported/empty fixture manifest')
    seen = set()
    for case in data['cases']:
        name = case['name']
        require(re.fullmatch(r'[A-Za-z0-9_-]{1,40}', name) and name not in seen, 'Invalid/duplicate case name')
        seen.add(name)
        require(case['provenance'] in ('physical', 'synthetic_partial'), 'Declare physical/synthetic provenance')
        case['_capture'] = resolve(path.parent, case['capture'])
        channels = case['channels']
        require(channels and len({c['channel'] for c in channels}) == len(channels), 'Invalid/duplicate channels')
        for channel in channels:
            require(type(channel['channel']) is int and 0 <= channel['channel'] < 32, 'Invalid input channel')
            channel['_cli'] = resolve(path.parent, channel['cli_jsonl']) if channel.get('cli_jsonl') else None
            if case['provenance'] == 'physical':
                require(channel['_cli'] is not None, 'Physical cases require per-channel CLI evidence')
                require(type(channel.get('expected_accepted_payloads')) is int and channel['expected_accepted_payloads'] >= 0,
                        'Declare per-channel expected accepted payload counts')
        if case['provenance'] == 'physical':
            require(any(c['expected_accepted_payloads'] > 0 for c in channels), 'Physical case needs a positive confirmed-payload expectation')
            evidence = case['independent']
            required = {'codec', 'packing', 'header_state', 'structure', 'sample_rate_hz', 'channels', 'channel_count_known',
                        'samples_per_frame', 'frame_bytes', 'crc1', 'header_crc', 'frame_crc'}
            require(required <= evidence['expected_codec_fields'].keys(), 'Independent rate/channel/CRC metadata is incomplete')
            for key in ('carrier', 'source_receipt', 'validation'):
                evidence['_'+key] = resolve(path.parent, evidence[key])
            require(evidence['expected_codec_fields'] and evidence['period_frames'] > 0, 'Independent metadata/period required')
    return path, data


def canonical(record):
    result = dict(record)
    result.pop('snapshot_prefix', None)
    result.pop('live_snapshot', None)
    # Earlier development CLI artifacts used a generic CRC-present key. Its
    # meaning applies only to DTS, so the public v0.3 spelling is explicit.
    if 'crc_present' in result:
        value = result.pop('crc_present')
        value = value if result.get('codec') == 'DTS core' else None
        require('dts_crc_present' not in result or result['dts_crc_present'] == value, 'Contradictory CRC-present aliases')
        result['dts_crc_present'] = value
    return result


def key(record):
    return int(record['transport_start_sample']), int(record['frame_index']), int(record['byte_offset'])


def complete(record):
    return (record.get('transport_complete') is True and record.get('transport_integrity') is True
            and record.get('source_interval_known') is True and not record.get('snapshot_prefix', False))


def confirmed(record):
    return complete(record) and record['transport_format'] in ('AC-3 (Dolby Digital)', 'E-AC-3', 'DTS type I', 'DTS type II', 'DTS type III')


def signature(record):
    return json.dumps(canonical(record), sort_keys=True, separators=(',', ':'))


def independent(case):
    if case['provenance'] != 'physical':
        return None
    evidence = case['independent']
    receipt = json.loads(bounded(evidence['_source_receipt']).read_text())
    validation = json.loads(bounded(evidence['_validation']).read_text())
    source = bounded(evidence['_carrier']).read_bytes()
    source_hash = hashlib.sha256(source).hexdigest()
    require(source_hash == receipt['source_sha256'] == validation['source_sha256'], 'Independent carrier SHA mismatch')
    require(receipt['finite_playback_complete'] is True, 'Source playback not established complete')
    require(receipt['capture_rate'] == case['capture_rate_hz'], 'Receipt/capture clock mismatch')
    require(validation['every_payload_byte_exact_source_match'] is True and validation['ffmpeg_returncode'] == 0
            and not validation['independent_ffmpeg_decode_crc_errors'], 'Independent source/decode validation failed')
    period = int(evidence['period_frames'])*4
    require(evidence['period_frames'] == {1: 1536, 11: 512}.get(evidence['Pc'] & 127), 'Unsupported independent AC3/DTS-I carrier period')
    require(period >= 8 and len(source) % period == 0, 'Source is not complete fixed-period carrier frames')
    payloads = set()
    for offset in range(0, len(source), period):
        pa, pb, pc, pd = struct.unpack_from('<4H', source, offset)
        require((pa, pb) == (0xf872, 0x4e1f) and pc == evidence['Pc'], 'Independent source IEC header mismatch')
        require(pd > 0 and pd % 16 == 0 and 8+pd//8 <= period, 'Unsupported independent source bit extent')
        data = source[offset+8:offset+8+pd//8]
        # FFmpeg's S/PDIF carrier contains little-endian transport words; native
        # extraction uses canonical big-endian words. No codec parser is reused.
        payloads.add(bytes(value for n in range(0, len(data), 2) for value in (data[n+1], data[n])))
    require(len(source)//period == validation['source_payload_frames'], 'Source frame count mismatch')
    return dict(payloads=payloads, fields=evidence['expected_codec_fields'], source_sha256=source_hash,
                carrier_frames=len(source)//period, Pc=evidence['Pc'])


def interval(row, metadata):
    rate = Decimal(str(metadata['capture_rate_hz']))
    trigger = int(metadata['settings']['trigger_sample'])
    with localcontext() as context:
        context.prec = 60
        return tuple(int((Decimal(row[field])*rate).to_integral_value(rounding=ROUND_HALF_UP))+trigger
                     for field in ('start_seconds', 'end_seconds'))


def check_record(record, coverage):
    require(record.get('event') == 'codec_frame' and record.get('schema_version') == 1, 'Invalid codec record schema')
    start, end = int(record['start_sample']), int(record['end_sample_inclusive'])
    transport_start, transport_end = int(record['transport_start_sample']), int(record['transport_end_sample_inclusive'])
    require(coverage[0] <= transport_start <= transport_end <= coverage[1], 'Codec transport outside snapshot coverage')
    if record['source_interval_known']:
        require(transport_start <= start <= end <= transport_end, 'Codec source outside transport interval')
    if record.get('snapshot_prefix'):
        require(record.get('live_snapshot') is True and (not record['transport_complete'] or not record['transport_integrity']), 'Prefix falsely presented as completed codec')


def check_one(case, channel, detail, directory, expected_version, reference, sparse_table=False):
    require(directory.is_dir() and not list(directory.glob('*.error.json')), 'Missing/failed export: '+str(directory))
    records = jsonl(directory/'codecs.jsonl')
    require(records and records[0].get('event') == 'metadata', 'Missing native codec export context')
    metadata, codecs = records[0], records[1:]
    require(metadata['analyzer_version'] == expected_version and metadata['schema_version'] == 1, 'Analyzer version/schema mismatch')
    require(metadata['input_channel'] == 'D'+str(channel['channel']) and metadata['live_snapshot'] is True, 'Wrong input/snapshot context')
    require(metadata['capture_rate_hz'] == case['capture_rate_hz'], 'Wrong native capture clock')
    require(metadata['settings']['waveform_detail'] == {'Fields': 0, 'Samples': 1}[detail], 'Wrong waveform detail context')
    require(metadata['settings'].get('structured_sample_rows') is (not sparse_table), 'Wrong structured sample rows context')
    coverage = int(metadata['coverage_start_sample']), int(metadata['coverage_end_sample_inclusive'])
    require(coverage[0] <= coverage[1], 'Invalid snapshot coverage')
    identities = [key(c) for c in codecs]
    require(len(identities) == len(set(identities)), 'Duplicate codec identity in JSONL')
    for codec in codecs:
        check_record(codec, coverage)
    cli = jsonl(channel['_cli']) if channel['_cli'] else []
    cli_codecs = [c for c in cli if c.get('event') == 'codec_frame']
    cli_map = {key(c): c for c in cli_codecs}
    require(len(cli_map) == len(cli_codecs), 'Duplicate CLI codec identity')
    if cli:
        wanted = sorted(signature(c) for c in cli_codecs if complete(c))
        actual = sorted(signature(c) for c in codecs if complete(c))
        require(actual == wanted, 'Completed native codec metadata differs from CLI')
        for codec in codecs:
            if not codec.get('snapshot_prefix'):
                require(key(codec) in cli_map and signature(codec) == signature(cli_map[key(codec)]),
                        'Closed native codec event differs from CLI')
    samples = rows(directory/'subframes.csv')
    cli_samples = [r for r in cli if r.get('event') == 'subframe']
    if cli:
        require(len(samples) == len(cli_samples), 'Native/CLI subframe count differs')
    last = -1
    source_samples = {}
    for n, sample in enumerate(samples):
        start, end = interval(sample, metadata)
        require(last < start <= end and coverage[0] <= start <= end <= coverage[1], 'Legacy source intervals overlap/outside coverage')
        last = end
        source_samples[start] = (end, int(sample['raw_slots_4_31'], 0), int(sample['signed_container_24']))
        if cli:
            expected = cli_samples[n]
            require((start, end) == (int(expected['start_sample']), int(expected['end_sample_inclusive'])), 'Legacy/CLI source interval differs')
            require(sample['channel'] == ('B' if expected['channel'] else 'A') and
                    int(sample['raw_slots_4_31'], 0) == expected['raw_slots_4_31'] and
                    int(sample['signed_container_24']) == expected['signed_container_24'] and
                    sample['parity_ok'] == ('1' if expected['parity_ok'] else '0'), 'Legacy/CLI payload or channel differs')
            for field in ('V_invalid', 'U', 'C', 'P'):
                require(sample[field] == ('1' if expected[field] else '0'), 'Legacy/CLI flags differ: '+field)
    table = rows(directory/'table.csv')
    last = None
    table_codecs = []
    codec_signatures = {signature(c) for c in codecs}
    for row in table:
        start, duration = Decimal(row['start_time']), Decimal(row['duration'])
        require(duration >= 0 and (last is None or start+Decimal('1e-12') >= last), 'Structured table displayed intervals overlap')
        last = start+duration
        if row['type'] == 'subframe':
            original = int(row['coverage_start_sample'])
            require(original in source_samples and source_samples[original] ==
                    (int(row['coverage_end_sample']), int(row['raw_slots_4_31'], 0), int(row['signed_container_24'])),
                    'Structured sample original source/payload differs from legacy CSV')
        if row['type'] == 'codec_frame':
            codec = json.loads(row['json'])
            require(signature(codec) in codec_signatures, 'Structured codec JSON missing from retained export')
            for field in ('codec', 'packing', 'header_state', 'structure', 'crc1', 'header_crc', 'frame_crc', 'channel_configuration'):
                require(row[field] == codec[field], 'Structured flat codec field differs: '+field)
            for field in ('channel_count_known', 'transport_complete', 'transport_integrity', 'transport_conflict'):
                require(row[field] == ('true' if codec[field] else 'false'), 'Structured boolean codec field differs: '+field)
            for field in ('sample_rate_hz', 'channels', 'frame_bytes', 'samples_per_frame'):
                require(int(row[field]) == codec[field], 'Structured numeric codec field differs: '+field)
            require(int(row['coverage_start_sample']) == int(codec['start_sample']) and
                    int(row['coverage_end_sample']) == int(codec['end_sample_inclusive']), 'Structured codec source coverage differs')
            table_codecs.append(codec)
    require(sorted(signature(c) for c in table_codecs if complete(c)) == sorted(signature(c) for c in codecs if complete(c)),
            'Completed codec table notifications missing or duplicated')
    require(sum(row['type'] == 'subframe' for row in table) == (0 if sparse_table else len(samples)), 'Structured/legacy sample count differs from explicit table mode')
    payload_directory = directory/'payload.bursts'
    index = jsonl(payload_directory/'index.jsonl')
    require(index and index[0]['type'] == 'configuration' and index[-1] == {'type': 'finish', 'complete': True}, 'Incomplete payload export index')
    require(not (payload_directory/'INCOMPLETE').exists() and not list(payload_directory.glob('*.part')), 'Uncommitted payload export')
    index_codecs = [r['record'] for r in index if r['type'] == 'codec_frame']
    require(sorted(signature(c) for c in index_codecs) == sorted(signature(c) for c in codecs), 'Payload index and Codec frames JSONL differ')
    bursts = [r for r in index if r['type'] == 'burst']
    accepted = [r for r in bursts if r['accepted']]
    files = set()
    for burst in bursts:
        require(coverage[0] <= int(burst['start_sample']) <= int(burst['end_sample']) <= coverage[1], 'Burst outside native source coverage')
        if burst['accepted']:
            filename = burst['file'];require(Path(filename).name == filename and filename not in files, 'Invalid/duplicate payload filename');files.add(filename)
            data = bounded(payload_directory/filename).read_bytes()
            require(burst['complete'] is True and burst['integrity'] is True and len(data)*8 == burst['declared_bits'] == burst['observed_bits'], 'Incomplete payload accepted')
            require(reference is not None and data in reference['payloads'] and burst['Pc'] == reference['Pc'], 'Payload differs from independent carrier bytes')
        else:
            require(burst['file'] is None and burst['reason'], 'Rejected payload omitted its reason')
    require({p.name for p in payload_directory.glob('*.bin')} == files, 'Unindexed/missing payload files')
    if reference:
        require(len(accepted) == channel['expected_accepted_payloads'], 'Accepted payload count differs from explicit fixture expectation')
        for codec in codecs:
            if complete(codec):
                for field, expected in reference['fields'].items():
                    require(codec[field] == expected, 'Independent codec metadata differs: '+field)
        require(len(accepted) == sum(confirmed(c) for c in codecs), 'Expected one complete independent codec frame per accepted burst')
    else:
        require(not accepted and not any(complete(c) for c in codecs), 'Synthetic partial header incorrectly establishes a valid codec')
        require(bursts and all(not b['accepted'] for b in bursts), 'Synthetic pending header rejection missing')
        require(codecs and all(c.get('snapshot_prefix') is True and c['header_state'] == 'truncated' and
                              c['structure'] != 'passed' for c in codecs), 'Missing explicit truncated snapshot-prefix codec evidence')
    return dict(subframes=len(samples), structured_sample_rows=not sparse_table, table_subframe_rows=sum(row['type'] == 'subframe' for row in table), table_rows=len(table), table_codec_rows=len(table_codecs), codec_rows=len(codecs),
                completed_codec_rows=sum(complete(c) for c in codecs), confirmed_codec_rows=sum(confirmed(c) for c in codecs), snapshot_prefix_rows=sum(bool(c.get('snapshot_prefix')) for c in codecs),
                accepted_payloads=len(accepted), rejected_payloads=len(bursts)-len(accepted),
                source_coverage=[str(v) for v in coverage], codecs_sha256=digest(directory/'codecs.jsonl'))


def verify_all(manifest, output, names, sparse_table=False):
    result = {}
    for case in manifest['cases']:
        if case['name'] not in names:
            continue
        reference = independent(case)
        cases = {}
        for channel in case['channels']:
            modes = {}
            for detail in DETAILS:
                directory = output/case['name']/('D'+str(channel['channel']))/detail.lower()
                modes[detail] = check_one(case, channel, detail, directory, manifest['analyzer_version'], reference, sparse_table)
            # Display detail is presentation only; original codec/source events
            # and extracted payload identities must be identical in both modes.
            a = jsonl(output/case['name']/('D'+str(channel['channel']))/'samples/codecs.jsonl')[1:]
            b = jsonl(output/case['name']/('D'+str(channel['channel']))/'fields/codecs.jsonl')[1:]
            require([signature(c) for c in a] == [signature(c) for c in b], 'Fields/Samples codec events differ')
            cases['D'+str(channel['channel'])] = modes
        result[case['name']] = dict(provenance=case['provenance'], capture_sha256=digest(case['_capture']), channels=cases,
                                    independent_source_sha256=reference['source_sha256'] if reference else None)
    return result


def reusable(directory, paths):
    require(not list(directory.glob('*.error.json')), 'Previous export explicitly failed')
    exists = [path.exists() for path in paths]
    if any(exists):
        require(all(exists), 'Existing export is incomplete; refusing overwrite')
        if paths[0].is_dir():
            index = jsonl(paths[0]/'index.jsonl')
            require(index[-1] == {'type': 'finish', 'complete': True} and not (paths[0]/'INCOMPLETE').exists(), 'Existing payload directory incomplete')
        return True
    return False


def collect(manifest, output, names, args):
    from saleae import automation  # Deliberately absent from offline verification.
    require(args.expected_pid is not None, 'Collection requires --expected-pid for the isolated app')
    progress = {'result': 'INCOMPLETE', 'saved_capture_only': True, 'captures': {}}
    with automation.Manager.connect(port=args.port) as manager:
        info = manager.get_app_info()
        require(info.app_pid == args.expected_pid, 'Connected app PID differs; no captures were opened')
        progress.update(logic_version=info.app_version, app_pid=info.app_pid, port=args.port)
        for case in manifest['cases']:
            if case['name'] not in names:
                continue
            capture_hash = digest(case['_capture'])
            with manager.load_capture(str(case['_capture'])) as capture:
                for channel in case['channels']:
                    for detail in DETAILS:
                        directory = output/case['name']/('D'+str(channel['channel']))/detail.lower()
                        directory.mkdir(parents=True, exist_ok=args.resume)
                        options = {'S/PDIF': channel['channel'], 'Waveform detail': detail, 'Analysis window (ms)': 1000,
                                   'Structured sample rows': not args.sparse_table}
                        requests = [('Selected CSV', [directory/'table.csv', directory/'subframes.csv']),
                                    ('Codec frames JSONL', [directory/'codecs.jsonl']),
                                    ('IEC61937 payloads', [directory/'payload.bursts'])]
                        for kind, paths in requests:
                            if args.resume and reusable(directory, paths):
                                print('Reusing', case['name'], channel['channel'], detail, kind, flush=True)
                                continue
                            require(not any(path.exists() for path in paths), 'Refusing to overwrite an existing export')
                            settings = {**options, 'Export output': kind}
                            print('Configuring', case['name'], channel['channel'], detail, kind, flush=True)
                            started = time.monotonic()
                            analyzer = capture.add_analyzer('S/PDIF Analyzer', settings=settings)
                            try:
                                if kind == 'Selected CSV':
                                    capture.export_data_table(str(directory/'table.csv'), [analyzer])
                                    capture.legacy_export_analyzer(str(directory/'subframes.csv'), analyzer, automation.RadixType.HEXADECIMAL)
                                else:
                                    target = directory/('payload' if kind == 'IEC61937 payloads' else 'codecs.jsonl')
                                    capture.legacy_export_analyzer(str(target), analyzer, automation.RadixType.HEXADECIMAL)
                                    require(not Path(str(target)+'.error.json').exists(), 'Explicit native export failure')
                            finally:
                                capture.remove_analyzer(analyzer)
                            print('Exported', kind, round(time.monotonic()-started, 3), 's', flush=True)
            require(digest(case['_capture']) == capture_hash, 'Saved input capture changed during collection')
            progress['captures'][case['name']] = {'sha256': capture_hash, 'provenance': case['provenance']}
            (output/'collection.json').write_text(json.dumps(progress, indent=2)+'\n')
    progress['result'] = 'COLLECTED'
    (output/'collection.json').write_text(json.dumps(progress, indent=2)+'\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cases', nargs='+')
    parser.add_argument('--port', type=int, default=10431)
    parser.add_argument('--expected-pid', type=int)
    parser.add_argument('--verify-only', action='store_true')
    parser.add_argument('--resume', action='store_true')
    parser.add_argument('--sparse-table', action='store_true', help='Explicitly omit only per-subframe structured rows; legacy CSV and codec/status notifications remain required')
    args = parser.parse_args()
    path, manifest = load_manifest(args.manifest)
    names = args.cases or [case['name'] for case in manifest['cases']]
    require(set(names) <= {case['name'] for case in manifest['cases']}, 'Unknown requested case')
    output = args.output.resolve()
    if args.verify_only:
        require(output.is_dir(), 'Offline artifact directory does not exist')
    else:
        output.mkdir(parents=True, exist_ok=args.resume)
    report = dict(result='INCOMPLETE', manifest_sha256=digest(path), cases={},
                  verification='offline artifacts; source receipts distinguish physical/synthetic provenance')
    report_path = output/'verification-report.json'
    report_path.write_text(json.dumps(report, indent=2)+'\n')
    try:
        if not args.verify_only:
            collect(manifest, output, names, args)
        report['cases'] = verify_all(manifest, output, names, args.sparse_table)
        report['result'] = 'PASS'
    except Exception as error:
        report.update(result='FAIL', error=str(error))
        report_path.write_text(json.dumps(report, indent=2)+'\n')
        raise
    report_path.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
