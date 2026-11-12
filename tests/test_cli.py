"""Black-box JSONL/policy tests using independent integer-timestamp waveforms."""
import json
import subprocess
import sys

binary = sys.argv[1]
checks = 0

def require(condition, message):
    global checks
    checks += 1
    if not condition:
        raise AssertionError(message)

def signal(frames=385, offset=0, bad_parity=False, gap=False, conflict=False):
    # 61.44 MS/s, 48 kHz: exactly ten capture samples per half-cell.
    edges = []
    base = offset + 100
    status = [4, 0, 0, 2, 11] + [0] * 19
    special = [0xf872, 0x4e1f, 1, 32, 0x0b77, 0x1234]
    for frame in range(frames):
        if gap and frame == 20:
            base += 10000
        for channel in range(2):
            runs = (0, 3, 5, 6) if channel else ((0, 3, 4, 5) if frame % 192 == 0 else (0, 3, 6, 7))
            edges.extend(base + n * 10 for n in runs)
            signed = (frame + 1) * (1 if channel == 0 else -1)
            payload = signed & 0xffffff
            index = frame * 2 + channel - 400
            if conflict and 0 <= index < len(special):
                payload = special[index] << 8
            position = frame % 192
            c = (status[position // 8] >> (position % 8)) & 1
            raw = payload | (c << 26)
            raw |= (bin(raw).count('1') % 2) << 27
            if bad_parity and frame == 30 and channel == 0:
                raw ^= 1 << 27
            for bit in range(28):
                edges.append(base + (8 + bit * 2) * 10)
                if (raw >> bit) & 1:
                    edges.append(base + (9 + bit * 2) * 10)
            base += 640
    edges.append(base)
    return ''.join(str(x) + '\n' for x in edges)

def run(data, *args):
    result = subprocess.run([binary, '61440000', '--jsonl', *args], input=data, text=True, capture_output=True, timeout=40)
    records = [json.loads(line) for line in result.stdout.splitlines()]
    require(all(r['schema_version'] == 1 for r in records), 'Every machine record must be versioned')
    return result, records

r, records = run(signal(), '--expect-content', 'pcm')
require(r.returncode == 0, r.stderr)
require(records[0]['event'] == 'metadata', 'Missing metadata')
require(records[-1]['event'] == 'summary', 'Missing final summary')
frames = [x for x in records if x['event'] == 'subframe']
blocks = [x for x in records if x['event'] == 'channel_status']
require(len(frames) == 770, 'Clean fixture lost subframes')
require(len(blocks) == 4, 'Wrong complete status count')
require(records[-1]['totals']['subframes'] == len(frames), 'Summary must match detailed events')
require(sum(c['decoded_subframes'] for c in records[-1]['channels']) == len(frames), 'Health and report use different counts')
require(records[-1]['expected_content_observed'], 'Confirmed PCM was not observed')
require(all(isinstance(x['start_sample'], str) for x in frames), 'Sample indices must be JSON strings')
require(frames[0]['start_sample'] == '100' and frames[1]['start_sample'] == '740', 'Integer timestamp mapping changed')

large = (1 << 54) + 17
r, records = run(signal(offset=large), '--origin-sample', str(large))
require(r.returncode == 0, r.stderr)
first = next(x for x in records if x['event'] == 'subframe')
require(first['start_sample'] == str(large + 100), 'Lost timestamp bits above 2^53')
require(first['field_boundaries'][0] == str(large + 100), 'Field boundary lost timestamp bits')

for bad, policy, code in [(dict(bad_parity=True), 'parity', 'parity'), (dict(gap=True), 'sync_loss', 'sync_loss'), (dict(conflict=True), 'content_conflict', None)]:
    r, records = run(signal(**bad), '--fail-on', policy)
    require(r.returncode == 1, 'Failure policy did not fail: ' + policy)
    require(policy in records[-1]['failed_policies'], 'Missing failed policy name')
    if code:
        require(any(x['event'] == 'diagnostic' and x['code'] == code for x in records), 'Missing diagnostic event')

r, records = run(signal(bad_parity=True))
require(r.returncode == 0, 'Opt-in parity failure policy changed default behavior')
r, records = run(signal(), '--expect-content', 'dts')
require(r.returncode == 1 and 'expected_content_absent' in records[-1]['failed_policies'], 'Absent expected content was accepted')
r, records = run('')
require(r.returncode == 1 and records[-1]['event'] == 'summary', 'Empty input must return a machine summary and failure')
r, records = run('-1\n')
require(r.returncode == 2 and records[-1]['event'] == 'fatal_error', 'Negative timestamps must be rejected')
r, records = run('not-an-edge\n')
require(r.returncode == 2, 'Malformed input was accepted')
r = subprocess.run([binary, '61440000'], input=signal(), text=True, capture_output=True, timeout=40)
require(r.returncode == 0 and 'subframes=770 status_records=4' in r.stdout, 'Legacy text summary changed')
require(not r.stdout.startswith('{'), 'Text output became JSON without request')
r = subprocess.run([binary, '1e30', '--jsonl'], input='', text=True, capture_output=True, timeout=40)
require(r.returncode == 2, 'Unrepresentable capture rate was accepted')
print('PASS CLI:', checks, 'checks')
