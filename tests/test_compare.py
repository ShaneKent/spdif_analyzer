"""Independent subprocess fixtures for bounded PCM alignment and timeline holes."""
import csv
from decimal import Decimal, getcontext
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest
getcontext().prec=60
SCRIPT=Path(__file__).resolve().parents[1]/'scripts'/'compare_streams.py'
RATE=48000
CAPTURE=49152000  # exactly1024capture samples/stereo frame


def raw(value,bits=16):
    return (value & ((1<<bits)-1)) << (24-bits)


def sequence(n=120):
    rng=random.Random(61937)
    return [(rng.randrange(-20000,20000),rng.randrange(-20000,20000)) for _ in range(n)]


class CompareTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.directory=Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def jsonl(self,name,values,*,drop=(),invalid=(),unknown=(),bits=16,rate=RATE,
              first=0,origin=0,swap=False,mixed_bits=False,clock_ppm=0,no_layout=False,metadata_bits=0):
        path=self.directory/name
        rows=[{'schema_version':1,'event':'metadata','capture_rate_hz':CAPTURE,
               'origin_sample':str(origin),'nominal_rate_hz':0,'pcm_bits_override':metadata_bits}]
        # Independent known status declaration spanning the complete fixture.
        for ch in (0,1):
            rows.append({'schema_version':1,'event':'channel_status','start_sample':str(first),
                         'end_sample_inclusive':str(first+len(values)*1024),
                         'channel':ch,'integrity':True,'supported':True,'non_audio':False,
                         'declared_sample_rate_hz':rate})
        for index, pair in enumerate(values):
            if index in drop:continue
            for ch in (0,1):
                value=pair[1-ch] if swap else pair[ch]
                stamp=first+int(Decimal(index*1024+ch*512)*(1+Decimal(clock_ppm)/1000000))
                width=24 if mixed_bits and index>=len(values)//2 else (bits[ch] if isinstance(bits,tuple) else bits)
                rows.append({'schema_version':1,'event':'subframe','start_sample':str(stamp),
                             'end_sample_inclusive':str(stamp+511),'channel':ch,'raw_slots_4_31':raw(value,width),
                             'parity_ok':index not in invalid,'V_invalid':False,
                             'sample_rate_hz':rate,'content':'Unknown' if index in unknown else 'PCM',
                             'layout':{'known':index not in unknown and not no_layout,'bits':width,'professional':False,'stereo':True}})
        path.write_text(''.join(json.dumps(row)+'\n' for row in rows),encoding='utf8')
        return path

    def csv(self,name,values,*,drop=(),invalid=(),missing_b=()):
        path=self.directory/name
        with path.open('w',newline='') as f:
            writer=csv.DictWriter(f,fieldnames=['start_seconds','channel','raw_slots_4_31','content','parity_ok','V_invalid','measured_sample_rate_hz'])
            writer.writeheader()
            for index,pair in enumerate(values):
                if index in drop:continue
                for ch,value in enumerate(pair):
                    if ch==1 and index in missing_b:continue
                    writer.writerow({'start_seconds':str(Decimal(2*index+ch)/(2*RATE)),
                                     'channel':'B' if ch else 'A','raw_slots_4_31':hex(raw(value)),
                                     'content':'PCM','parity_ok':index not in invalid,
                                     'V_invalid':index in invalid,'measured_sample_rate_hz':RATE})
        return path

    def run_compare(self,a,b,*args,expected=0):
        run=subprocess.run([sys.executable,str(SCRIPT),str(a),str(b),'--max-lag','12','--min-overlap','32',*args],text=True,capture_output=True)
        self.assertEqual(run.returncode,expected,run.stdout+run.stderr)
        report=json.loads(run.stdout);self.assertEqual(report['schema_version'],1)
        return report

    def test_positive_negative_delay(self):
        values=sequence();delay=7
        a=self.jsonl('a.jsonl',values)
        b=self.jsonl('b.jsonl',[(121,244)]*delay+values[:-delay])
        report=self.run_compare(a,b);self.assertEqual(report['lag_frames'],delay)
        self.assertEqual(Decimal(report['offset_seconds']),Decimal(delay)/RATE)
        report=self.run_compare(b,a);self.assertEqual(report['lag_frames'],-delay)

    def test_changed_sample(self):
        values=sequence();changed=list(values);changed[57]=(values[57][0]+1,values[57][1])
        report=self.run_compare(self.jsonl('a.jsonl',values),self.jsonl('b.jsonl',changed),expected=1)
        self.assertEqual(report['lag_frames'],0);self.assertEqual(report['mismatch_frames'],1)
        self.assertEqual(report['mismatches'][0]['a_frame'],'57')

    def test_dropped_segment_preserves_positions(self):
        values=sequence();report=self.run_compare(self.jsonl('a.jsonl',values),self.jsonl('b.jsonl',values,drop=range(40,51)),expected=1)
        self.assertEqual(report['lag_frames'],0);self.assertEqual(report['mismatch_frames'],0)
        self.assertEqual(report['hole_frames'],11);self.assertEqual(report['holes'][0]['a_first_frame'],'40')

    def test_initial_missing_not_compressed(self):
        values=sequence();report=self.run_compare(self.jsonl('a.jsonl',values),self.jsonl('b.jsonl',values,drop=range(5)))
        self.assertEqual(report['lag_frames'],0)
        self.assertEqual(report['sources'][1]['first_frame_index'],'5')

    def test_invalid_and_unknown_are_holes(self):
        values=sequence();report=self.run_compare(self.jsonl('a.jsonl',values),self.jsonl('b.jsonl',values,invalid=(40,),unknown=(50,)),expected=1)
        self.assertEqual(report['lag_frames'],0);self.assertEqual(report['hole_frames'],2)

    def test_swap_explicit(self):
        values=sequence();a=self.jsonl('a.jsonl',values);b=self.jsonl('b.jsonl',values,swap=True)
        self.run_compare(a,b,expected=1)
        self.assertEqual(self.run_compare(a,b,'--swap-b')['lag_frames'],0)

    def test_silence_and_repetition_ambiguous(self):
        for values in ([(0,0)]*120,[(1,2),(3,4)]*60):
            a=self.jsonl('a.jsonl',values);b=self.jsonl('b.jsonl',values)
            self.assertEqual(self.run_compare(a,b,expected=1)['status'],'ambiguous')
            self.assertEqual(self.run_compare(a,b,'--max-lag','0',expected=1)['status'],'ambiguous')

    def test_noise_does_not_establish_delay_for_silent_or_repeating_source(self):
        for values in ([(0,0)]*120,[(1,2),(3,4)]*60):
            changed=list(values);changed[57]=(123,456)
            a=self.jsonl('a.jsonl',values);b=self.jsonl('b.jsonl',changed)
            self.assertEqual(self.run_compare(a,b,expected=1)['status'],'ambiguous')
            self.assertEqual(self.run_compare(b,a,expected=1)['status'],'ambiguous')

    def test_periodic_mutation_with_invalid_or_missing_positions_is_ambiguous(self):
        values=[(1,2),(3,4)]*60;changed=list(values);changed[57]=(123,456)
        for holes in ({'invalid':(40,)},{'drop':(40,)},{'drop':range(40,45)}):
            a=self.jsonl('a.jsonl',values);b=self.jsonl('b.jsonl',changed,**holes)
            self.assertEqual(self.run_compare(a,b,expected=1)['status'],'ambiguous')
            self.assertEqual(self.run_compare(b,a,expected=1)['status'],'ambiguous')

    def test_two_cycles_with_error_cannot_force_a_whole_cycle_delay(self):
        period=sequence(64);values=period*2;changed=list(values);changed[57]=(123,456)
        a=self.jsonl('a.jsonl',values);b=self.jsonl('b.jsonl',changed,invalid=(40,))
        self.assertEqual(self.run_compare(a,b,'--max-lag','70',expected=1)['status'],'ambiguous')
        self.assertEqual(self.run_compare(b,a,'--max-lag','70',expected=1)['status'],'ambiguous')

    def test_truncated_second_cycle_still_has_strong_repetition_constraints(self):
        period=sequence(64);values=(period*2)[:-1];changed=list(values);changed[57]=(123,456)
        a=self.jsonl('a.jsonl',values);b=self.jsonl('b.jsonl',changed,invalid=(40,))
        self.assertEqual(self.run_compare(a,b,'--max-lag','70',expected=1)['status'],'ambiguous')
        self.assertEqual(self.run_compare(b,a,'--max-lag','70',expected=1)['status'],'ambiguous')
        # A few equal intro/outro values are not evidence that an otherwise
        # unique complete clip repeats over a near-full-length period.
        unique=sequence(120);unique[-3:]=unique[:3]
        a=self.jsonl('unique-a.jsonl',unique);b=self.jsonl('unique-b.jsonl',unique)
        self.run_compare(a,b)

    def test_periodicity_with_holes_in_both_inputs_uses_original_positions(self):
        values=[(1,2),(3,4)]*60;changed=list(values);changed[57]=(123,456)
        a=self.jsonl('a.jsonl',values,drop=(20,))
        b=self.jsonl('b.jsonl',changed,drop=(40,))
        self.assertEqual(self.run_compare(a,b,expected=1)['status'],'ambiguous')
        self.assertEqual(self.run_compare(b,a,expected=1)['status'],'ambiguous')
        # Sparse periodic evidence: known indices0,2,4,... carry a period-four
        # sequence. Joining them would incorrectly report period two.
        values=[(1,2),(11,12),(3,4),(13,14)]*40
        a=self.jsonl('a.jsonl',values,drop=range(1,len(values),2))
        b=self.jsonl('b.jsonl',values,drop=range(1,len(values),2))
        report=self.run_compare(a,b,'--max-lag','0',expected=1)
        self.assertEqual(report['status'],'ambiguous')
        self.assertEqual(report['repetition_period_frames'],4)

    def test_sparse_period_not_visible_in_most_common_anchor(self):
        # Independent residue classes: period100 repeats B/C/D across cycles;
        # the more common A occurs only at0/10/20 in the first cycle.
        values=[(0,0)]*200
        observed={0:(1,1),10:(1,1),20:(1,1),30:(2,2),40:(3,3),50:(4,4),
                  130:(2,2),140:(3,3),150:(4,4),199:(5,5)}
        for index,value in observed.items():values[index]=value
        a=self.csv('sparse.csv',values,invalid=[i for i in range(200) if i not in observed])
        report=self.run_compare(a,a,'--max-lag','0','--min-overlap','3','--rate','48000','--bits','16',expected=1)
        self.assertEqual(report['status'],'ambiguous');self.assertEqual(report['repetition_period_frames'],100)

    def test_sparse_period_requires_later_pairs_not_first_anchor_differences(self):
        values=[(0,0)]*200
        observed={0:(1,1),10:(2,2),30:(1,1),40:(2,2),60:(1,1),70:(2,2),
                  130:(1,1),140:(2,2),160:(1,1),170:(2,2),199:(3,3)}
        for index,value in observed.items():values[index]=value
        a=self.csv('later-pairs.csv',values,invalid=[i for i in range(200) if i not in observed])
        report=self.run_compare(a,a,'--max-lag','0','--min-overlap','3','--rate','48000','--bits','16',expected=1)
        self.assertEqual(report['status'],'ambiguous')
        self.assertIn(report['repetition_period_frames'],(100,130))
        # Break the other valid P130 witness while retaining P100. This makes
        # later A/B pairs essential even after allowing partial-cycle periods.
        observed[5]=(9,9);observed[135]=(10,10)
        for index,value in observed.items():values[index]=value
        a=self.csv('later-pairs-only.csv',values,invalid=[i for i in range(200) if i not in observed])
        report=self.run_compare(a,a,'--max-lag','0','--min-overlap','3','--rate','48000','--bits','16',expected=1)
        self.assertEqual(report['status'],'ambiguous');self.assertEqual(report['repetition_period_frames'],100)
        # Search itself fits this budget; the additional all-pairs ambiguity
        # audit must fail explicitly rather than silently calling it unique.
        self.run_compare(a,a,'--max-lag','0','--min-overlap','3','--rate','48000','--bits','16','--max-work','201',expected=2)

    def test_layout_rate_rejected(self):
        values=sequence();a=self.jsonl('a.jsonl',values)
        self.run_compare(a,self.jsonl('b.jsonl',values,bits=24),expected=2)
        self.run_compare(a,self.jsonl('c.jsonl',values,rate=44100),expected=2)
        self.run_compare(a,self.jsonl('d.jsonl',values,mixed_bits=True),expected=2)
        self.assertEqual(self.run_compare(a,self.jsonl('e.jsonl',values,mixed_bits=True),'--end-seconds','0.001')['bits'],16)

    def test_csv_requires_metadata_and_keeps_holes(self):
        values=sequence();a=self.csv('a.csv',values);b=self.csv('b.csv',values,missing_b=(45,),invalid=(46,))
        self.run_compare(a,b,expected=2)
        report=self.run_compare(a,b,'--rate','48000','--bits','16',expected=1)
        self.assertEqual(report['lag_frames'],0);self.assertEqual(report['hole_frames'],2)

    def test_json_csv_compatible(self):
        values=sequence();self.run_compare(self.jsonl('a.jsonl',values),self.csv('b.csv',values),'--rate','48000','--bits','16')

    def test_compatible_asymmetric_channel_layouts(self):
        values=sequence();a=self.jsonl('a.jsonl',values,bits=(16,24))
        b=self.jsonl('b.jsonl',values,bits=(16,24))
        self.assertEqual(self.run_compare(a,b)['channel_bits'],[16,24])
        swapped=self.jsonl('swapped.jsonl',values,bits=(24,16),swap=True)
        self.run_compare(a,swapped,expected=2)
        self.assertEqual(self.run_compare(a,swapped,'--swap-b')['lag_frames'],0)

    def test_long_origin_lossless(self):
        values=sequence();origin=(1<<54)+17
        a=self.jsonl('a.jsonl',values,first=origin,origin=origin)
        changed=list(values);changed[70]=(12345,23456)
        report=self.run_compare(a,self.jsonl('b.jsonl',changed,first=origin,origin=origin),expected=1)
        self.assertEqual(report['lag_frames'],0)
        self.assertEqual(report['mismatches'][0]['a_start_sample'],str(origin+70*1024))

    def test_clock_drift_does_not_accumulate_absolute_grid_error(self):
        values=sequence(25000)
        a=self.jsonl('a.jsonl',values);b=self.jsonl('b.jsonl',values,clock_ppm=50)
        self.assertEqual(self.run_compare(a,b,'--max-lag','1')['lag_frames'],0)

    def test_explicit_layout_override_never_promotes_unknown_content(self):
        values=sequence();a=self.jsonl('a.jsonl',values,no_layout=True)
        self.run_compare(a,a,expected=2)
        self.run_compare(a,a,'--bits','16')
        b=self.jsonl('b.jsonl',values,no_layout=True,metadata_bits=16)
        self.run_compare(b,b)
        known=self.jsonl('known.jsonl',values,bits=24)
        self.run_compare(known,known,'--bits','16',expected=2)

    def test_unknown_prefix_and_orphan_channel_are_positions(self):
        values=sequence();a=self.jsonl('a.jsonl',values)
        b=self.jsonl('b.jsonl',values,unknown=range(20))
        report=self.run_compare(a,b,expected=1)
        self.assertEqual(report['lag_frames'],0);self.assertEqual(report['hole_frames'],20)
        rows=[json.loads(line) for line in a.read_text().splitlines()]
        rows=[r for r in rows if not (r['event']=='subframe' and r['start_sample']=='0' and r['channel']==0)]
        c=self.directory/'orphan.jsonl';c.write_text(''.join(json.dumps(r)+'\n' for r in rows))
        report=self.run_compare(a,c,expected=1)
        self.assertEqual(report['lag_frames'],0);self.assertEqual(report['hole_frames'],1)

    def test_uint64_sample_indices_and_negative_csv_time(self):
        values=sequence();a=self.jsonl('a.jsonl',values)
        rows=[json.loads(line) for line in a.read_text().splitlines()]
        for invalid in (-1,1<<64):
            changed=[dict(r) for r in rows]
            next(r for r in changed if r['event']=='subframe')['start_sample']=str(invalid)
            bad=self.directory/'bad.jsonl';bad.write_text(''.join(json.dumps(r)+'\n' for r in changed))
            self.run_compare(a,bad,expected=2)
        csv_path=self.csv('negative.csv',values)
        with csv_path.open(newline='') as f: rows=list(csv.DictReader(f))
        for row in rows: row['start_seconds']=str(Decimal(row['start_seconds'])-Decimal('0.01'))
        with csv_path.open('w',newline='') as f:
            writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
        report=self.run_compare(csv_path,csv_path,'--rate','48000','--bits','16')
        self.assertEqual(report['lag_frames'],0)

    def test_selected_interval_can_use_latest_preceding_status(self):
        values=sequence();a=self.jsonl('a.jsonl',values)
        rows=[json.loads(line) for line in a.read_text().splitlines()]
        for row in rows:
            if row['event']=='channel_status':row['end_sample_inclusive']=str(49*1024+1023)
        a.write_text(''.join(json.dumps(row)+'\n' for row in rows))
        report=self.run_compare(a,a,'--start-seconds','0.00125')
        self.assertEqual(report['lag_frames'],0);self.assertEqual(report['sources'][0]['first_frame_index'],'60')

    def test_no_pcm_empty_and_bounds(self):
        values=sequence();a=self.jsonl('a.jsonl',values)
        b=self.jsonl('b.jsonl',values,unknown=range(len(values)))
        self.run_compare(a,b,expected=2)
        empty=self.directory/'empty';empty.write_text('')
        self.run_compare(a,empty,expected=2)
        self.run_compare(a,a,'--max-work','10',expected=2)


    def window_args(self, frames=64, hop=32):
        return ['--window-seconds',str(Decimal(frames)/RATE),'--hop-seconds',str(Decimal(hop)/RATE)]

    def test_windowed_constant_delay_preserves_fixed_mode(self):
        values=sequence(384);a=self.jsonl('a.jsonl',values)
        b=self.jsonl('b.jsonl',[(111,222)]*5+values)
        report=self.run_compare(a,b,*self.window_args())
        self.assertEqual(report['mode'],'windowed')
        self.assertTrue(all(w['lag_frames']==5 for w in report['windows']))
        self.assertEqual(report['delay_change_candidates'],[])
        self.assertTrue(report['drift_segments'])
        for segment in report['drift_segments']:
            self.assertLess(abs(Decimal(segment['relative_drift_ppm'])),Decimal('0.001'))
        self.assertTrue(all(isinstance(w['first_matched_A_sample'],str) for w in report['windows']))
        # Decimal rounding in repeating frame durations must not mark ordinary
        # hop windows as a special final-window placement.
        self.assertLessEqual(sum(w['tail_anchored'] for w in report['windows']),1)
        self.assertFalse(any(w['tail_anchored'] for w in report['windows'][:-1]))

    def test_windowed_inserted_and_dropped_pcm_candidates(self):
        values=sequence(384);a=self.jsonl('a.jsonl',values)
        for name,changed,delta in [('insert',[(123,456)]*5+values[:128]+[(777,888)]*3+values[128:],3),
                                   ('drop',[(123,456)]*5+values[:128]+values[131:],-3)]:
            b=self.jsonl(name+'.jsonl',changed)
            report=self.run_compare(a,b,*self.window_args(),expected=1)
            candidates=report['delay_change_candidates']
            self.assertTrue(candidates,report)
            found=next(c for c in candidates if c['delta_frames']==delta)
            self.assertTrue(found['ordered_single_transition_supported'])
            self.assertGreaterEqual(found['old_mapping_support'],32)
            self.assertGreaterEqual(found['new_mapping_support'],32)
            self.assertLessEqual(Decimal(found['interval_start_seconds']),Decimal(128)/RATE)
            self.assertGreaterEqual(Decimal(found['interval_end_seconds_exclusive']),Decimal(128)/RATE)
            self.assertIn('Candidate',found['certainty'])
            self.assertTrue(any(w['status']!='aligned' for w in report['windows']))

    def test_windowed_large_step_does_not_skip_unobserved_insert_span(self):
        values=sequence(512);a=self.jsonl('a.jsonl',values)
        changed=values[:128]+[(30000+i,-30000-i) for i in range(200)]+values[128:]
        b=self.jsonl('b.jsonl',changed,drop=(220,))
        report=self.run_compare(a,b,*self.window_args(),'--max-lag','256',expected=1)
        self.assertEqual(report['delay_change_candidates'],[])

    def test_windowed_long_clock_drift_does_not_reanchor_false_integer_steps(self):
        values=sequence(25000);a=self.jsonl('a.jsonl',values)
        b=self.jsonl('b.jsonl',values,clock_ppm=50)
        report=self.run_compare(a,b,*self.window_args(2048,1024),'--max-lag','0')
        self.assertTrue(all(w['lag_frames']==0 for w in report['windows']))
        self.assertEqual(report['delay_change_candidates'],[])
        self.assertLess(abs(Decimal(report['drift_segments'][0]['relative_drift_ppm'])-50),Decimal('0.1'))

    def test_windowed_timestamp_drift_sign_and_scale(self):
        values=sequence(768);a=self.jsonl('a.jsonl',values)
        for ppm in (-200,-50,50,200):
            b=self.jsonl('drift.jsonl',values,clock_ppm=ppm)
            report=self.run_compare(a,b,*self.window_args(96,48),'--max-lag','2')
            self.assertTrue(report['drift_segments'])
            estimate=Decimal(report['drift_segments'][0]['relative_drift_ppm'])
            self.assertLess(abs(estimate-ppm),Decimal('1'),(ppm,estimate))
            self.assertTrue(all(w['lag_frames']==0 for w in report['windows']))
            self.assertEqual(report['delay_change_candidates'],[])

    def test_windowed_holes_and_faults_are_barriers(self):
        values=sequence(384);a=self.jsonl('a.jsonl',values)
        for kwargs in ({'drop':range(140,146)},{'invalid':(150,)},{'unknown':range(140,146)}):
            b=self.jsonl('holes.jsonl',values,**kwargs)
            report=self.run_compare(a,b,*self.window_args(),expected=1)
            self.assertEqual(report['delay_change_candidates'],[])
            bad={w['index'] for w in report['windows'] if not w.get('continuous_full_window_evidence',False)}
            self.assertTrue(bad)
            for fit in report['drift_segments']:
                self.assertFalse(bad.intersection(fit['support_windows']))

    def test_windowed_edge_drops_in_either_source_are_uncertainty_barriers(self):
        values=sequence(384);full=self.jsonl('full.jsonl',values)
        for dropped in (range(64,96), range(96,128), (64,), (127,)):
            short=self.jsonl('edge-hole.jsonl',values,drop=dropped)
            for a,b in ((full,short),(short,full)):
                report=self.run_compare(a,b,*self.window_args(64,64),'--max-lag','0',expected=1)
                affected=report['windows'][1]
                self.assertEqual(affected['status'],'incomplete_coverage')
                self.assertFalse(affected['continuous_full_window_evidence'])
                self.assertEqual(report['status'],'changes_or_uncertainty')
                self.assertEqual(report['delay_change_candidates'],[])
                for fit in report['drift_segments']:
                    self.assertNotIn(1,fit['support_windows'])

    def test_windowed_clipped_delay_tail_is_explicit(self):
        values=sequence(384);a=self.jsonl('a.jsonl',values)
        b=self.jsonl('tail.jsonl',[(111,222)]*5+values[:-5])
        report=self.run_compare(a,b,*self.window_args(),expected=1)
        self.assertEqual(report['windows'][-1]['status'],'incomplete_coverage')
        self.assertEqual(report['windows'][-1]['alignment_status'],'aligned')
        self.assertFalse(report['windows'][-1]['continuous_full_window_evidence'])
        # Fixed comparison still accepts its unique, exact observed overlap.
        self.assertEqual(self.run_compare(a,b)['status'],'aligned')

    def test_windowed_silence_and_repetition_stay_ambiguous(self):
        for values in ([(0,0)]*384,[(1,2),(3,4)]*192):
            a=self.jsonl('periodic.jsonl',values)
            report=self.run_compare(a,a,*self.window_args(),expected=1)
            self.assertTrue(all(w['status']=='ambiguous' for w in report['windows']))
            self.assertEqual(report['delay_change_candidates'],[])
            self.assertEqual(report['drift_segments'],[])

    def test_windowed_layout_boundary_is_explicit_and_resumes(self):
        values=sequence(384);a=self.jsonl('mixed.jsonl',values,mixed_bits=True)
        report=self.run_compare(a,a,*self.window_args(),expected=1)
        self.assertTrue(any(w['status']=='incompatible_interval' for w in report['windows']))
        self.assertTrue(any(w.get('channel_bits')==[16,16] and w['status']=='aligned' for w in report['windows']))
        self.assertTrue(any(w.get('channel_bits')==[24,24] and w['status']=='aligned' for w in report['windows']))
        self.assertEqual(report['delay_change_candidates'],[])
        for fit in report['drift_segments']:
            keys={(report['windows'][i]['a_epoch'],report['windows'][i]['b_epoch']) for i in fit['support_windows']}
            self.assertEqual(len(keys),1)

    def test_windowed_rate_boundary_is_explicit_and_resumes(self):
        values=sequence(384);path=self.directory/'rate-change.jsonl';rows=[{'schema_version':1,'event':'metadata','capture_rate_hz':CAPTURE,'origin_sample':'0'}]
        cut=192;boundary=cut*1024
        for ch in (0,1):
            for start,end,rate in [(0,boundary-1,RATE),(boundary,boundary+(len(values)-cut)*512-1,96000)]:
                rows.append({'schema_version':1,'event':'channel_status','start_sample':str(start),'end_sample_inclusive':str(end),'channel':ch,'integrity':True,'supported':True,'non_audio':False,'declared_sample_rate_hz':rate})
        for i,pair in enumerate(values):
            rate=RATE if i<cut else 96000;step=1024 if i<cut else 512;base=i*1024 if i<cut else boundary+(i-cut)*512
            for ch in (0,1):
                stamp=base+ch*(step//2)
                rows.append({'schema_version':1,'event':'subframe','start_sample':str(stamp),'end_sample_inclusive':str(stamp+step//2-1),'channel':ch,'raw_slots_4_31':raw(pair[ch]),'parity_ok':True,'V_invalid':False,'sample_rate_hz':rate,'content':'PCM','layout':{'known':True,'bits':16,'stereo':True,'professional':False}})
        path.write_text(''.join(json.dumps(r)+'\n' for r in rows))
        report=self.run_compare(path,path,*self.window_args(),expected=1)
        self.assertTrue(any(w['status']=='incompatible_interval' for w in report['windows']))
        self.assertTrue(any(w.get('rate_hz')=='96000' and w['status']=='aligned' for w in report['windows']))
        self.assertEqual(report['delay_change_candidates'],[])

    def test_windowed_global_limits_not_per_window_resets(self):
        values=sequence(512);a=self.jsonl('bounded.jsonl',values)
        report=self.run_compare(a,a,*self.window_args(),'--max-lag','0')
        budget=max(1000,report['work_used']//2)
        self.run_compare(a,a,*self.window_args(),'--max-lag','0','--max-work',str(budget),expected=2)
        self.run_compare(a,a,*self.window_args(),'--max-lag','0','--max-work',str(budget),'--end-seconds',str(Decimal(64)/RATE))
        self.run_compare(a,a,*self.window_args(),'--max-windows','2',expected=2)
        self.run_compare(a,a,*self.window_args(),'--hop-seconds','1e-100',expected=2)
        self.assertLessEqual(report['work_used'],report['work_limit'])

    def test_windowed_swap_and_negative_csv_origins(self):
        values=sequence(256);a=self.csv('a.csv',values);b=self.csv('b.csv',[(y,x) for x,y in values])
        for path in (a,b):
            with path.open(newline='') as f:rows=list(csv.DictReader(f))
            for row in rows:row['start_seconds']=str(Decimal(row['start_seconds'])-Decimal('.01'))
            with path.open('w',newline='') as f:
                writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
        report=self.run_compare(a,b,*self.window_args(),'--rate','48000','--bits','16','--swap-b')
        self.assertLess(Decimal(report['windows'][0]['start_seconds']),0)
        self.assertTrue(all(w['lag_frames']==0 for w in report['windows']))



if __name__=='__main__':unittest.main()
