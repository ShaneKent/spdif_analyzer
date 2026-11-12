"""Independent black-box fixtures for standalone report fidelity and boundaries."""
import csv
from decimal import Decimal
from html.parser import HTMLParser
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts' / 'capture_report.py'
CAPTURE = 1000000


def event(kind, **fields):
    return {'schema_version': 1, 'event': kind, **fields}


def frames(values, first=0):
    records = []
    for number, pair in enumerate(values):
        for channel, value in enumerate(pair):
            start = first + number * 20 + channel * 10
            records.append(event('subframe', start_sample=str(start), end_sample_inclusive=str(start + 9),
                                 channel=channel, raw_slots_4_31=(value & 0xffff) << 8,
                                 content='PCM', parity_ok=True, V_invalid=False,
                                 layout={'known': True, 'bits': 16, 'stereo': True, 'professional': False}))
    return records


class Parser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.scripts = []
        self.external = []

    def handle_starttag(self, tag, attrs):
        values = dict(attrs)
        if tag == 'script':
            self.scripts.append(values)
        if ('src' in values and not values['src'].startswith('data:')) or tag == 'link':
            self.external.append((tag, values))


class ReportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.dir = Path(self.temp.name)
        self.number = 0

    def tearDown(self):
        self.temp.cleanup()

    def source(self, rows, *, origin=0, rate=CAPTURE, metadata=None):
        self.number += 1
        path = self.dir / ('source%d.jsonl' % self.number)
        meta = event('metadata', capture_rate_hz=rate, origin_sample=str(origin))
        if metadata:
            meta.update(metadata)
        path.write_text('\n'.join(json.dumps(item) for item in [meta] + rows) + '\n', encoding='utf8')
        return path

    def run_report(self, source, *options, expected=0, output=None):
        output = output or self.dir / 'report.html'
        run = subprocess.run([sys.executable, str(SCRIPT), str(source), '-o', str(output), *map(str, options)],
                             text=True, capture_output=True, timeout=30)
        self.assertEqual(run.returncode, expected, run.stdout + run.stderr)
        if expected:
            return run
        text = output.read_text(encoding='utf8')
        match = re.search(r'<script type="application/json" id="report-data">(.*?)</script>', text, re.S)
        self.assertIsNotNone(match)
        return json.loads(match.group(1)), text

    def test_pcm_counts_and_independent_extrema(self):
        rows = frames([(-32768, 32767), (0, -1), (123, -456)])
        rows += [event('summary', totals={'subframes': 6, 'status_records': 0})]
        data, text = self.run_report(self.source(rows))
        self.assertEqual(data['stats']['subframes'], 6)
        self.assertEqual(data['stats']['pcm_eligible'], 6)
        bins = data['pcm_bins']
        self.assertEqual(min(b['minimum'] for b in bins[0]), -1)
        self.assertEqual(max(b['maximum'] for b in bins[1]), 32767 / 32768)
        self.assertEqual(sum(b['count'] for group in bins for b in group), 6)
        self.assertIn('manual', text)
        self.assertNotIn('https://', text)
        parsed = Parser();parsed.feed(text)
        self.assertEqual(len(parsed.scripts), 2)
        self.assertFalse(parsed.external)

    def test_mixed_unknown_nonpcm_and_invalid_are_excluded(self):
        rows = frames([(100, 200), (300, 400), (500, 600), (700, 800)])
        rows[0]['content'] = 'Unknown'
        rows[1]['content'] = 'Provisional IEC 61937 (status unconfirmed)'
        rows[2]['content'] = 'AC-3 (Dolby Digital)'
        rows[3]['parity_ok'] = False
        rows[4]['V_invalid'] = True
        rows[5]['layout']['known'] = False
        data, _ = self.run_report(self.source(rows))
        self.assertEqual(data['stats']['pcm_eligible'], 2)
        self.assertEqual(sum(data['stats']['excluded'].values()), 6)
        self.assertEqual(data['stats']['parity_failures'], 1)
        self.assertEqual(data['stats']['V_invalid'], 1)
        self.assertEqual(sum(b['accepted'] for group in data['pcm_bins'] for b in group), 2)

    def test_sparse_categories_and_exact_source_vs_notification(self):
        rows = frames([(0, 0)])
        for kind in ['content', 'channel_status', 'status_change', 'diagnostic', 'health_window', 'pcm_window']:
            rows.append(event(kind, start_sample='0', end_sample_inclusive='19', evidence='original'))
        rows += [event('codec_frame', start_sample='0', end_sample_inclusive='0', source_interval_known=False,
                       transport_start_sample='5', transport_end_sample_inclusive='18', frame_crc='failed'),
                 event('diagnostic', start_sample='1000', end_sample_inclusive='1001', code='parity',
                       coverage_start_sample='7', coverage_end_sample='9', publication_sample='1000')]
        data, _ = self.run_report(self.source(rows))
        self.assertEqual(data['sparse_event_count'], 8)
        item = data['events'][-1]
        self.assertEqual(Decimal(item['t0']), Decimal(7) / CAPTURE)
        self.assertEqual(item['record']['publication_sample'], '1000')
        self.assertEqual(data['events'][-2]['time_basis'], 'transport interval; codec source interval unknown')

    def test_large_sample_indices_keep_exact_origin_subtraction(self):
        origin = (1 << 63) + 913
        rows = frames([(1, -1)], origin + 100)
        rows.append(event('diagnostic', start_sample=str(origin + 123), end_sample_inclusive=str(origin + 125), code='test'))
        data, _ = self.run_report(self.source(rows, origin=origin))
        self.assertEqual(data['origin_sample'], str(origin))
        self.assertEqual(data['events'][0]['record']['start_sample'], str(origin + 123))
        self.assertEqual(Decimal(data['events'][0]['t0']), Decimal('0.000123'))
        self.assertEqual(Decimal(data['pcm_bins'][0][0]['t0']), Decimal('0.0001'))

    def test_plot_subtraction_precedes_float_even_with_origin_zero(self):
        first=(1<<63)+913
        rows=frames([(1,2)],first)
        rows += [event('diagnostic',start_sample=str(first+3),end_sample_inclusive=str(first+3),code='nearby')]
        data,_=self.run_report(self.source(rows))
        self.assertEqual(data['events'][0]['plot_t0'],0.000003)
        self.assertEqual(data['pcm_bins'][0][0]['plot_t0'],0)
        self.assertEqual(data['plot_range'][1],0.00002)

    def test_numeric_large_unknown_fields_stay_exact(self):
        value = (1 << 63) + 3
        rows = [event('unknown_extension', arbitrary_count=value, start_sample=value, end_sample_inclusive=value)]
        data, _ = self.run_report(self.source(rows, origin=value))
        self.assertEqual(data['events'][0]['record']['arbitrary_count'], str(value))
        self.assertEqual(data['events'][0]['record']['start_sample'], str(value))

    def test_injection_and_template_tokens_remain_text(self):
        attack = '</script><script>alert("x")</script><img src=x onerror=alert(1)>&\u2028__TITLE____DATA__'
        data, text = self.run_report(self.source([event('diagnostic', start_sample='0', end_sample_inclusive='1',
                                                      message=attack, code=attack)]), '--title', attack)
        self.assertEqual(data['title'], attack)
        self.assertEqual(data['events'][0]['record']['message'], attack)
        parsed = Parser();parsed.feed(text)
        self.assertEqual(len(parsed.scripts), 2)
        self.assertFalse(parsed.external)
        self.assertIn('\\u003c/script\\u003e', text)
        self.assertNotIn('innerHTML', text)

    def test_downsampling_preserves_counts_and_extrema(self):
        values = [(i % 100, -(i % 100)) for i in range(1001)]
        values[117] = (-32768, 32767)
        values[901] = (32767, -32768)
        rows = frames(values)
        rows[100]['V_invalid'] = True
        data, _ = self.run_report(self.source(rows), '--max-plot-bins', '2')
        self.assertTrue(all(len(group) <= 2 for group in data['pcm_bins']))
        self.assertEqual(data['stats']['subframes'], 2002)
        self.assertEqual(sum(b['count'] for group in data['pcm_bins'] for b in group), 2002)
        self.assertEqual(sum(b['accepted'] for group in data['pcm_bins'] for b in group), 2001)
        for group in data['pcm_bins']:
            self.assertEqual(min(b['minimum'] for b in group), -1)
            self.assertEqual(max(b['maximum'] for b in group), 32767 / 32768)

    def test_all_sparse_events_survive_pagination(self):
        rows = [event('diagnostic', start_sample=str(i), end_sample_inclusive=str(i), code='fault-%d' % i) for i in range(253)]
        data, text = self.run_report(self.source(rows))
        self.assertEqual(len(data['events']), 253)
        self.assertEqual(data['events'][-1]['label'], 'fault-252')
        self.assertIn('slice(page*100,page*100+100)', text)

    def test_explicit_budgets_do_not_replace_existing_output(self):
        source = self.source(frames([(0, 0)]) + [event('diagnostic', start_sample='0', code='x') for _ in range(3)])
        for option, value in [('--max-input-bytes', 1), ('--max-records', 1), ('--max-events', 2), ('--max-output-bytes', 100)]:
            output = self.dir / 'keep.html';output.write_text('original', encoding='utf8')
            result = self.run_report(source, option, value, expected=2, output=output)
            self.assertIn('budget', result.stderr)
            self.assertEqual(output.read_text(), 'original')
            self.assertFalse(list(self.dir.glob('*.tmp')))

    def test_summary_consistency_rejects_mismatched_totals(self):
        source = self.source(frames([(0, 0)]) + [event('summary', totals={'subframes': 3})])
        self.assertIn('count mismatch', self.run_report(source, expected=2).stderr)

    def test_unknown_capture_gaps_are_visible(self):
        rows = frames([(1, 2)]) + frames([(3, 4)], 100)
        data, _ = self.run_report(self.source(rows, metadata={'coverage_start_sample': '0', 'coverage_end_sample_inclusive': '199'}))
        gaps = [e for e in data['events'] if e['kind'] == 'capture_gap']
        self.assertEqual([(Decimal(g['t0']), Decimal(g['t1'])) for g in gaps],
                         [(Decimal('0.00002'), Decimal('0.0001')), (Decimal('0.00012'), Decimal('0.0002'))])

    def test_completely_undecoded_declared_capture_is_unknown(self):
        data, _ = self.run_report(self.source([], metadata={'coverage_start_sample': '0', 'coverage_end_sample_inclusive': '99'}))
        gaps = [e for e in data['events'] if e['kind'] == 'capture_gap']
        self.assertEqual(data['stats']['subframes'], 0)
        self.assertEqual([(Decimal(g['t0']), Decimal(g['t1'])) for g in gaps], [(Decimal(0), Decimal('0.0001'))])
        self.assertIn('Unknown', gaps[0]['record']['evidence'])
        self.assertEqual(data['sparse_event_count'], 1)

    def test_fractional_boolean_and_noninteger_fields_are_rejected(self):
        for key, value in [('raw_slots_4_31', 256.75), ('raw_slots_4_31', True), ('raw_slots_4_31', None),
                           ('channel', 0.5), ('channel', True), ('start_sample', 0.5), ('start_sample', False)]:
            with self.subTest(key=key, value=value):
                rows = frames([(1, 2)]);rows[0][key] = value
                self.run_report(self.source(rows), expected=2)
        for value in [16.5, True, '16.0']:
            rows = frames([(1, 2)]);rows[0]['layout']['bits'] = value
            self.run_report(self.source(rows), expected=2)
            self.run_report(self.source(frames([(1, 2)]), metadata={'pcm_bits_override': value}), expected=2)
        rows = frames([(1, 2)]);rows[0]['content'] = 'Unknown';rows[0]['raw_slots_4_31'] = 256.75
        self.run_report(self.source(rows), expected=2)
        rows = frames([(1, 2)]);rows[0]['layout'] = []
        self.run_report(self.source(rows), expected=2)
        # Exported decimal integer strings and hexadecimal payload strings stay
        # supported. Their values must not be reinterpreted as decimal floats.
        rows = frames([(1, 2)]);rows[0]['raw_slots_4_31'] = '000256';rows[0]['channel'] = '0';rows[0]['layout']['bits'] = '16'
        rows[1]['raw_slots_4_31'] = '0x000200'
        data, _ = self.run_report(self.source(rows))
        self.assertEqual(data['stats']['pcm_eligible'], 2)
        self.assertEqual(data['pcm_bins'][0][0]['minimum'], 1 / 32768)

    def bundle(self, *, complete=True, trigger='1000'):
        directory = self.dir / 'bundle';directory.mkdir()
        sources = {
            'subframes.csv': (['start_seconds','end_seconds','channel','raw_slots_4_31','content','parity_ok','V_invalid','layout_known','layout_bits'],
                              [['-0.000020','-0.000011','A','0x800000','PCM','1','0','1','16'],
                               ['-0.000010','-0.000001','B','0x7FFF00','PCM','1','0','1','16']]),
            'channel-status.csv': (['start_seconds','end_seconds','channel','integrity','interpretation'], [['-0.000020','-0.000001','A','1','consumer; PCM']]),
            'diagnostics.csv': (['start_seconds','end_seconds','code','message'], [['-0.000015','-0.000014','parity','test']]),
            'content.csv': (['start_seconds','end_seconds','format','burst','complete','integrity','Pc','Pd','evidence'], [['-0.000020','-0.000001','PCM','0','1','1','0x0000','0','status']])}
        manifest = {'schema_version':'1','capture_rate':CAPTURE,'coverage_start_sample':'980','coverage_end_sample':'999',
                    'complete':complete,'settings':{},'datasets':[]}
        if trigger is not None:
            manifest['settings']['trigger_sample'] = trigger
        for name,(header,rows) in sources.items():
            with (directory/name).open('w',newline='') as stream:
                writer=csv.writer(stream);writer.writerow(header);writer.writerows(rows)
            manifest['datasets'].append({'file':name,'snapshot_rows':len(rows),'written_rows':len(rows)})
        (directory/'manifest.json').write_text(json.dumps(manifest),encoding='utf8')
        return directory

    def test_real_four_csv_bundle_and_negative_trigger_seconds(self):
        data, _ = self.run_report(self.bundle())
        self.assertEqual(data['stats']['subframes'], 2)
        self.assertEqual(data['stats']['pcm_eligible'], 2)
        self.assertEqual(data['origin_sample'], '1000')
        self.assertEqual(data['events'][0]['record']['start_seconds'], '-0.000020')
        self.assertNotIn('start_sample', data['events'][0]['record'])
        self.assertIn('CSV precision', data['events'][0]['time_basis'])

    def test_bundle_count_mismatch_and_missing_complete_dataset(self):
        directory=self.bundle();manifest=json.loads((directory/'manifest.json').read_text())
        manifest['datasets'][0]['written_rows']=3;(directory/'manifest.json').write_text(json.dumps(manifest))
        self.assertIn('count mismatch',self.run_report(directory,expected=2).stderr)
        manifest['datasets'][0]['written_rows']=2;(directory/'manifest.json').write_text(json.dumps(manifest));(directory/'content.csv').unlink()
        self.assertIn('missing content.csv',self.run_report(directory,expected=2).stderr)

    def test_partial_bundle_unknowns_and_no_trigger_are_explicit(self):
        directory=self.bundle(complete=False,trigger=None);(directory/'content.csv').unlink();(directory/'INCOMPLETE').touch()
        data,_=self.run_report(directory)
        self.assertTrue(any('INCOMPLETE' in note for note in data['notes']))
        self.assertTrue(any('no trigger_sample' in note for note in data['notes']))
        self.assertTrue(any('content.csv' in note for note in data['notes']))

    def test_optional_analysis_shapes_and_live_snapshot(self):
        primary=self.source(frames([(0,0)]))
        health=self.dir/'health.json';health.write_text(json.dumps(event('summary', capture_rate_hz=CAPTURE,
           origin_sample='0', live_snapshot=True, channels=[{'decoded_subframes':1,'unknown_coverage_samples':100}],
           health_windows=[event('health_window',start_sample='0',end_sample_exclusive='20',channels=[])])))
        pcm=self.dir/'pcm.jsonl';pcm.write_text(json.dumps(event('metadata',capture_rate_hz=CAPTURE,origin_sample='0'))+'\n'+json.dumps(event('pcm_window',start_sample='0',end_sample_exclusive='20',accepted=2,live_snapshot=True))+'\n')
        changes=self.dir/'changes.jsonl';changes.write_text(json.dumps(event('metadata',capture_rate_hz=CAPTURE,origin_sample='0'))+'\n'+json.dumps(event('status_change',start_sample='0',end_sample_exclusive='20',changed_mask_hex='80'))+'\n')
        data,_=self.run_report(primary,'--health',health,'--pcm',pcm,'--status-changes',changes)
        self.assertEqual({e['kind'] for e in data['events']},{'health_window','pcm_window','status_change'})
        self.assertEqual(len(data['summaries']),1)
        self.assertEqual(data['summaries'][0]['record']['channels'][0]['unknown_coverage_samples'],100)

    def test_optional_capture_rate_conflict(self):
        source=self.source(frames([(0,0)]));other=self.source([],rate=48000)
        self.assertIn('conflicts',self.run_report(source,'--pcm',other,expected=2).stderr)

    def test_windowed_comparison_and_fit_support(self):
        source=self.source(frames([(0,0)]));comparison=self.dir/'comparison.json'
        comparison.write_text(json.dumps({'schema_version':1,'mode':'windowed','status':'changes_or_uncertainty',
          'sources':[{'origin':'0','capture_rate_hz':str(CAPTURE)}],
          'windows':[{'index':0,'start_seconds':'0','end_seconds_exclusive':'1','status':'aligned','lag_frames':3},
                     {'index':1,'start_seconds':'0.5','end_seconds_exclusive':'1.5','status':'ambiguous','candidate_lags':[-1,3]}],
          'delay_change_candidates':[{'interval_start_seconds':'0.2','interval_end_seconds_exclusive':'0.8','kind':'sample_insertion_or_drop_candidate'}],
          'drift_segments':[{'first_window':0,'last_window':1,'relative_drift_ppm':'3.14'}]}))
        data,_=self.run_report(source,'--comparison',comparison)
        self.assertEqual(len(data['events']),4)
        self.assertEqual(data['events'][-1]['t0'],'0')
        self.assertEqual(data['events'][-1]['t1'],'1.5')
        self.assertEqual(data['comparisons'][0]['windows'][1]['status'],'ambiguous')

    def test_global_comparison_still_preserves_uncertainty(self):
        source=self.source(frames([(0,0)]));comparison=self.dir/'comparison.json'
        comparison.write_text(json.dumps({'schema_version':1,'status':'ambiguous','candidates':[-2,2]}))
        data,_=self.run_report(source,'--comparison',comparison)
        self.assertEqual(data['events'][0]['t0'],None)
        self.assertEqual(data['events'][0]['record']['candidates'],[-2,2])

    def test_comparison_entries_share_input_record_budget(self):
        source = self.source(frames([(1, 2)]));comparison = self.dir / 'bounded-comparison.json'
        report = {'schema_version': 1, 'mode': 'windowed', 'sources': [{'origin': '0', 'capture_rate_hz': str(CAPTURE)}],
                  'windows': [{'index': i, 'start_seconds': str(i), 'end_seconds_exclusive': str(i + 1), 'status': 'aligned', 'lag_frames': 0} for i in range(20)],
                  'delay_change_candidates': [{'interval_start_seconds': '0', 'interval_end_seconds_exclusive': '1'}],
                  'drift_segments': [{'first_window': 0, 'last_window': 1}]}
        comparison.write_text(json.dumps(report))
        output = self.dir / 'keep-budget.html';output.write_text('original')
        error = self.run_report(source, '--comparison', comparison, '--max-records', '3', output=output, expected=2)
        self.assertIn('record budget', error.stderr)
        self.assertEqual(output.read_text(), 'original')
        # Primary metadata+2 subframes, comparison container+20 windows,
        # candidate+drift are26 input analysis objects in the shared budget.
        data, _ = self.run_report(source, '--comparison', comparison, '--max-records', '26')
        self.assertEqual(data['input_records'], 26)
        self.assertEqual(data['counts']['relative_delay'], 20)
        self.assertEqual(data['counts']['delay_change_candidate'], 1)
        self.assertEqual(data['counts']['drift_segment'], 1)
        self.run_report(source, '--comparison', comparison, '--max-records', '25', expected=2)

    def test_comparison_integer_indices_never_truncate(self):
        source = self.source(frames([(1, 2)]));comparison = self.dir / 'index-comparison.json'
        for location, value in [('window', 0.5), ('window', True), ('drift', 0.5), ('drift', False)]:
            report = {'schema_version': 1, 'windows': [{'index': value if location == 'window' else 0, 'start_seconds': '0', 'end_seconds_exclusive': '1'}],
                      'drift_segments': [{'first_window': value if location == 'drift' else 0, 'last_window': 0}]}
            comparison.write_text(json.dumps(report))
            self.run_report(source, '--comparison', comparison, expected=2)
        for key in ('delay_change_candidates', 'drift_segments'):
            for value in ({'unexpected': 'object'}, [None]):
                comparison.write_text(json.dumps({'schema_version': 1, key: value}))
                self.run_report(source, '--comparison', comparison, expected=2)

    def test_comparison_origin_translation_and_unmapped_evidence(self):
        source=self.source(frames([(0,0)],first=500),origin=500);comparison=self.dir/'origin-comparison.json'
        report={'schema_version':1,'mode':'windowed','sources':[{'origin':'0','capture_rate_hz':str(CAPTURE)}],
                'windows':[{'index':0,'start_seconds':'0.0005','end_seconds_exclusive':'0.001','status':'aligned','lag_frames':2}]}
        comparison.write_text(json.dumps(report));data,_=self.run_report(source,'--comparison',comparison)
        self.assertEqual(Decimal(data['events'][0]['t0']),0)
        self.assertEqual(Decimal(data['events'][0]['t1']),Decimal('0.0005'))
        del report['sources'];comparison.write_text(json.dumps(report));data,_=self.run_report(source,'--comparison',comparison)
        self.assertIsNone(data['events'][0]['t0'])
        self.assertTrue(any('cannot be mapped' in note for note in data['notes']))

    def test_sample_analysis_cannot_guess_missing_csv_trigger(self):
        bundle=self.bundle(trigger=None);other=self.source([event('pcm_window',start_sample='0',end_sample_exclusive='10')])
        self.assertIn('without trigger_sample',self.run_report(bundle,'--pcm',other,expected=2).stderr)

    def test_invalid_inputs_and_versions_fail_explicitly(self):
        for row in [event('diagnostic',start_sample='-1'), event('diagnostic',start_sample=1.5),
                    event('diagnostic',start_sample=str(1<<64)), {'schema_version':2,'event':'new'},
                    event('diagnostic',start_sample='10',end_sample_inclusive='1'),
                    event('metadata',capture_rate_hz=CAPTURE,origin_sample='1')]:
            self.run_report(self.source([row]),expected=2)
        empty=self.dir/'empty.jsonl';empty.write_text('');self.run_report(empty,expected=2)
        empty.write_text(json.dumps(event('metadata')));self.run_report(empty,expected=2)
        empty.write_text(json.dumps(event('metadata',capture_rate_hz=float('nan'))));self.run_report(empty,expected=2)
        empty.write_text('[1,2]');self.run_report(empty,expected=2)

    def test_output_cannot_replace_input(self):
        source=self.source(frames([(0,0)]));before=source.read_bytes()
        self.run_report(source,output=source,expected=2)
        self.assertEqual(source.read_bytes(),before)

    def test_empty_completed_capture_has_honest_zero_counts(self):
        data,_=self.run_report(self.source([event('summary',totals={'subframes':0,'status_records':0})]))
        self.assertEqual(data['stats']['subframes'],0)
        self.assertEqual(data['range'],['0','0'])
        self.assertFalse(data['events'])


if __name__ == '__main__':
    unittest.main()
