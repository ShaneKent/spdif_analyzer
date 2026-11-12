"""Offline counterexamples for the native codec smoke verifier; no Logic use."""
import csv
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

SPEC=importlib.util.spec_from_file_location('smoke_codecs',Path(__file__).resolve().parents[1]/'scripts/smoke_codecs.py')
s=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(s)


class SmokeCodecsTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name)

    def tearDown(self):self.tmp.cleanup()

    def write_jsonl(self,path,records):
        path.parent.mkdir(parents=True,exist_ok=True)
        path.write_text(''.join(json.dumps(r)+'\n' for r in records))

    def partial(self,detail='Samples'):
        directory=self.root/detail.lower();directory.mkdir()
        case={'provenance':'synthetic_partial','capture_rate_hz':1000}
        channel={'channel':0,'_cli':None}
        metadata={'schema_version':1,'event':'metadata','analyzer_version':'0.3.0','input_channel':'D0',
                  'capture_rate_hz':1000,'live_snapshot':True,'coverage_start_sample':'0','coverage_end_sample_inclusive':'100',
                  'settings':{'trigger_sample':'0','waveform_detail':1 if detail=='Samples' else 0,'structured_sample_rows':True}}
        codec={'schema_version':1,'event':'codec_frame','start_sample':'10','end_sample_inclusive':'20',
               'source_interval_known':False,'frame_index':0,'byte_offset':0,'codec':'Unknown','header_state':'truncated',
               'structure':'truncated','transport_start_sample':'10','transport_end_sample_inclusive':'20',
               'transport_complete':False,'transport_integrity':False,'snapshot_prefix':True,'live_snapshot':True}
        self.write_jsonl(directory/'codecs.jsonl',[metadata,codec])
        (directory/'subframes.csv').write_text('start_seconds,end_seconds,channel\n')
        (directory/'table.csv').write_text('name,type,start_time,duration\n')
        burst={'type':'burst','accepted':False,'file':None,'start_sample':'10','end_sample':'20','reason':'pending header'}
        self.write_jsonl(directory/'payload.bursts/index.jsonl',[{'type':'configuration'},
            {'type':'codec_frame','record':s.canonical(codec)},burst,{'type':'finish','complete':True}])
        return case,channel,directory,metadata,codec

    def test_partial_prefix_passes_without_codec_claim(self):
        case,ch,directory,_,_=self.partial()
        report=s.check_one(case,ch,'Samples',directory,'0.3.0',None)
        self.assertEqual(report['confirmed_codec_rows'],0);self.assertEqual(report['snapshot_prefix_rows'],1)
        self.assertEqual(report['accepted_payloads'],0)

    def test_partial_cannot_falsely_establish_complete_transport(self):
        case,ch,directory,metadata,codec=self.partial()
        codec.update(transport_complete=True,transport_integrity=True)
        self.write_jsonl(directory/'codecs.jsonl',[metadata,codec])
        with self.assertRaisesRegex(ValueError,'Prefix falsely'):s.check_one(case,ch,'Samples',directory,'0.3.0',None)

    def test_partial_missing_codec_index_fails(self):
        case,ch,directory,_,_=self.partial()
        self.write_jsonl(directory/'payload.bursts/index.jsonl',[{'type':'configuration'},{'type':'finish','complete':True}])
        with self.assertRaisesRegex(ValueError,'index and Codec'):s.check_one(case,ch,'Samples',directory,'0.3.0',None)

    def test_field_setting_and_table_overlap_fail(self):
        case,ch,directory,_,_=self.partial()
        with self.assertRaisesRegex(ValueError,'waveform detail'):s.check_one(case,ch,'Fields',directory,'0.3.0',None)
        (directory/'table.csv').write_text('name,type,start_time,duration\na,notice,0,0.1\na,notice,0.05,0\n')
        with self.assertRaisesRegex(ValueError,'overlap'):s.check_one(case,ch,'Samples',directory,'0.3.0',None)

    def test_sparse_table_retains_complete_legacy_csv(self):
        case,ch,directory,metadata,codec=self.partial()
        metadata['settings']['structured_sample_rows']=False
        self.write_jsonl(directory/'codecs.jsonl',[metadata,codec])
        (directory/'subframes.csv').write_text('start_seconds,end_seconds,channel,raw_slots_4_31,signed_container_24\n0,0.001,A,0x000001,1\n')
        report=s.check_one(case,ch,'Samples',directory,'0.3.0',None,sparse_table=True)
        self.assertEqual(report['subframes'],1);self.assertEqual(report['table_subframe_rows'],0)
        with self.assertRaisesRegex(ValueError,'structured sample rows'):s.check_one(case,ch,'Samples',directory,'0.3.0',None)

    def test_conflict_is_not_confirmed_payload(self):
        r={'transport_complete':True,'transport_integrity':True,'source_interval_known':True,
           'transport_format':'Conflict: PCM status / IEC 61937 header'}
        self.assertTrue(s.complete(r));self.assertFalse(s.confirmed(r))
        r['transport_format']='DTS type I';self.assertTrue(s.confirmed(r))

    def test_legacy_crc_alias_is_explicit_and_contradictions_fail(self):
        self.assertEqual(s.canonical({'codec':'DTS core','crc_present':False})['dts_crc_present'],False)
        self.assertIsNone(s.canonical({'codec':'AC-3','crc_present':False})['dts_crc_present'])
        with self.assertRaises(ValueError):s.canonical({'codec':'DTS core','crc_present':False,'dts_crc_present':True})

    def test_independent_carrier_word_order_and_corruption(self):
        carrier=self.root/'source.spdif'
        data=struct.pack('<4H',0xf872,0x4e1f,1,16)+bytes.fromhex('770b')+bytes(6144-10)
        carrier.write_bytes(data);source_sha=s.digest(carrier)
        receipt=self.root/'receipt.json';receipt.write_text(json.dumps({'source_sha256':source_sha,'finite_playback_complete':True,'capture_rate':100000000}))
        validation=self.root/'validation.json';validation.write_text(json.dumps({'source_sha256':source_sha,'every_payload_byte_exact_source_match':True,
            'ffmpeg_returncode':0,'independent_ffmpeg_decode_crc_errors':'','source_payload_frames':1}))
        case={'provenance':'physical','capture_rate_hz':100000000,'independent':{'_source_receipt':receipt,'_validation':validation,
              '_carrier':carrier,'period_frames':1536,'Pc':1,'expected_codec_fields':{'codec':'AC-3'}}}
        result=s.independent(case);self.assertEqual(result['payloads'],{bytes.fromhex('0b77')})
        carrier.write_bytes(data[:-1]+b'x')
        with self.assertRaisesRegex(ValueError,'SHA mismatch'):s.independent(case)

    def test_source_interval_and_incomplete_reuse_fail(self):
        _,_,directory,_,codec=self.partial()
        codec['transport_end_sample_inclusive']='101'
        with self.assertRaisesRegex(ValueError,'outside snapshot'):s.check_record(codec,(0,100))
        (directory/'one').write_text('partial')
        with self.assertRaisesRegex(ValueError,'incomplete'):s.reusable(directory,[directory/'one',directory/'two'])

    def test_verify_only_never_imports_automation_and_replaces_stale_pass(self):
        capture=self.root/'capture.sal';capture.write_bytes(b'offline unit fixture; never loaded in Logic')
        manifest=self.root/'manifest.json';manifest.write_text(json.dumps({'schema_version':1,'analyzer_version':'0.3.0','cases':[
            {'name':'partial','provenance':'synthetic_partial','capture':str(capture),'capture_rate_hz':1000,'channels':[{'channel':0}]}]}))
        output=self.root/'output';output.mkdir();(output/'verification-report.json').write_text('{"result":"PASS"}')
        original=__import__
        def guarded(name,*a,**kw):
            self.assertFalse(name.startswith('saleae'),'Automation imported during offline verification')
            return original(name,*a,**kw)
        with patch.object(sys,'argv',['smoke_codecs.py','--manifest',str(manifest),'--output',str(output),'--verify-only']),patch('builtins.__import__',side_effect=guarded):
            with self.assertRaises(ValueError):s.main()
        self.assertEqual(json.loads((output/'verification-report.json').read_text())['result'],'FAIL')


if __name__=='__main__':unittest.main()
