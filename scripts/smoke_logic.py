"""Verify a saved capture of this plugin's default 48 kHz demo ramp.

Requires logic2-automation. Configure the plugin BEFORE starting a demo capture,
save it as .sal, then invoke this script. Does not acquire physical hardware.
"""
import argparse
import csv
import json
from pathlib import Path
from saleae import automation

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('capture', type=Path)
p.add_argument('--port', type=int, default=10430)
p.add_argument('--output', type=Path, default=Path('artifacts/smoke'))
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
a.output = a.output.resolve()
with automation.Manager.connect(port=a.port) as manager:
    info = manager.get_app_info()
    with manager.load_capture(str(a.capture.resolve())) as capture:
        analyzer = capture.add_analyzer('S/PDIF Analyzer', settings={'S/PDIF': 0})
        capture.export_data_table(str(a.output/'table.csv'), [analyzer])
        capture.legacy_export_analyzer(str(a.output/'samples.csv'), analyzer, automation.RadixType.HEXADECIMAL)
rows = list(csv.DictReader((a.output/'samples.csv').open()))
assert len(rows) > 768, 'Capture at least two complete blocks'
for i, r in enumerate(rows):
    expected = ((i//2)*257) & 0x7fffff
    assert r['channel'] == ('B' if i % 2 else 'A'), (i, r)
    assert int(r['signed_container_24']) == (-expected if i % 2 else expected), (i, r)
    assert r['parity_ok'] == '1' and r['V_invalid'] == '0', (i, r)
table = list(csv.DictReader((a.output/'table.csv').open()))
blocks = [r for r in table if r['type'] == 'channel_status']
assert sum(r['type'] == 'subframe' for r in table) == len(rows)
assert blocks and len(blocks) % 2 == 0
for b in blocks:
    assert b['complete'] == b['integrity'] == 'true'
    assert 'nominal_rate=48000' in b['interpretation'] and 'word_length=24' in b['interpretation']
end = -1.0
for r in table:
    start = float(r['start_time'])
    assert start >= end-1e-14, 'Overlapping structured intervals'
    end = start+float(r['duration'])
report = {'logic_version': info.app_version, 'subframes_verified': len(rows), 'status_records_verified': len(blocks), 'result': 'PASS'}
(a.output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report))
