"""Independent accounting examples; native Win32 collection is root-run evidence."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

PATH = Path(__file__).resolve().parents[1] / 'scripts' / 'native_metrics.py'
spec = importlib.util.spec_from_file_location('native_metrics', PATH)
metrics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(metrics)
SECOND = 10000000


def row(pid, born, cpu, ws=0, private=0, parent=1, active=True):
    return {'pid':pid,'ppid':parent,'name':'Logic.exe','creation_filetime':born,
            'cpu_100ns':cpu*SECOND,'working_set_bytes':ws,'private_usage_bytes':private,'active':active}


class FakeWindows(metrics._Windows):
    """Mock OS primitives, exercising the real native snapshot/lineage logic."""
    def __init__(self, snapshots, processes):
        self.root_pid=10;self.root_identity=None;self.handles={}
        self.snapshots=iter(snapshots);self.processes=processes
        self.discarded=[];self.memory_queries=[]

    def entries(self):
        return next(self.snapshots)

    def opened(self, entry):
        key=(entry['pid'],self.processes[entry['pid']]['creation_filetime'])
        self.handles.setdefault(key,{'handle':entry['pid'],'entry':entry})
        return key,entry['pid']

    def times(self, handle):
        p=self.processes[handle]
        return p['creation_filetime'],p.get('exit_filetime',0),p['cpu_100ns']

    def row(self, key, handle):
        p=self.processes[handle]
        active=not bool(p.get('exit_filetime',0))
        if active:self.memory_queries.append(handle)
        return {**self.handles[key]['entry'],**p,'active':active,
                'working_set_bytes':p['working_set_bytes'] if active else 0,
                'private_usage_bytes':p['private_usage_bytes'] if active else 0}

    def discard(self, key):
        self.discarded.append(key);self.handles.pop(key,None)


def entry(pid, parent):
    return {'pid':pid,'ppid':parent,'name':'Logic.exe'}


class AccountingTests(unittest.TestCase):
    def test_concurrent_peak_is_not_sum_of_lifetime_peaks(self):
        a=metrics._Aggregate(1,100)
        a.observe([row(1,1,20,100,300),row(2,2,40,900,700)])
        a.observe([row(1,1,23,800,600),row(2,2,44,200,400)])
        report=a.result()
        self.assertTrue(report['passed'])
        self.assertEqual(report['peak_sampled_tree_working_set_bytes'],1000)
        self.assertEqual(report['peak_sampled_tree_private_usage_bytes'],1000)
        self.assertEqual(report['root_cpu_delta_seconds'],3)
        self.assertEqual(report['children_cpu_delta_seconds'],4)
        self.assertEqual(report['tree_cpu_delta_seconds'],7)
        individual_sum=sum(p['peak_sampled_working_set_bytes'] for p in report['observed_identities'])
        self.assertEqual(individual_sum,1700)
        self.assertNotEqual(individual_sum,report['peak_sampled_tree_working_set_bytes'])

    def test_late_discovery_does_not_charge_preexisting_child_history(self):
        a=metrics._Aggregate(1,100)
        a.observe([row(1,1,10)])
        a.observe([row(1,1,11),row(2,20,900)])
        a.observe([row(1,1,12),row(2,20,905)])
        report=a.result()
        self.assertEqual(report['children_cpu_delta_seconds'],5)
        child=report['observed_identities'][1]
        self.assertFalse(child['cpu_from_birth'])
        self.assertEqual(child['cpu_baseline_100ns'],str(900*SECOND))

    def test_new_child_includes_cpu_before_its_first_observation(self):
        a=metrics._Aggregate(1,100)
        a.observe([row(1,1,10)])
        a.observe([row(1,1,11),row(2,150,3)])
        a.observe([row(1,1,12),row(2,150,7)])
        report=a.result()
        self.assertEqual(report['children_cpu_delta_seconds'],7)
        self.assertTrue(report['observed_identities'][1]['cpu_from_birth'])
        self.assertEqual(report['observed_identities'][1]['creation_filetime'],'150')

    def test_reused_child_pid_is_two_separate_identities(self):
        a=metrics._Aggregate(1,100)
        a.observe([row(1,1,10),row(8,10,100,200,300)])
        a.observe([row(1,1,12),row(8,10,104,active=False),row(8,101,2,500,700)])
        a.observe([row(1,1,13),row(8,10,104,active=False),row(8,101,5,100,200)])
        report=a.result()
        self.assertEqual(report['children_cpu_delta_seconds'],9)
        self.assertEqual(len(report['observed_identities']),3)
        self.assertEqual(report['peak_sampled_tree_working_set_bytes'],500)
        self.assertFalse(report['observed_identities'][1]['active_at_last_observation'])

    def test_root_reuse_and_missing_never_report_success(self):
        a=metrics._Aggregate(1,100)
        a.observe([row(1,10,100,100,200)])
        a.observe([row(1,110,1,1000,2000)])
        report=a.result()
        self.assertFalse(report['passed'])
        self.assertEqual(report['root_creation_filetime'],'10')
        self.assertEqual(len(report['observed_identities']),1)
        self.assertEqual(report['peak_sampled_tree_working_set_bytes'],100)
        self.assertTrue(any('reused' in error for error in report['errors']))
        a.observe([])
        self.assertTrue(any('missing' in error for error in a.result()['errors']))

    def test_exited_observed_process_final_cpu_is_included(self):
        a=metrics._Aggregate(1,100)
        a.observe([row(1,10,100),row(2,110,2,500,700)])
        a.observe([row(1,10,102),row(2,110,5,active=False)])
        report=a.result()
        self.assertTrue(report['passed'])
        self.assertEqual(report['children_cpu_delta_seconds'],5)
        self.assertEqual(report['last_sample_tree_working_set_bytes'],0)
        self.assertEqual(report['peak_sampled_tree_private_usage_bytes'],700)

    def test_counter_failure_and_decrease_are_explicit(self):
        a=metrics._Aggregate(1,100)
        a.observe([row(1,10,100)],['GetProcessMemoryInfo(pid=2) failed: WinError 5'])
        a.observe([row(1,10,90)])
        report=a.result()
        self.assertFalse(report['passed'])
        self.assertEqual(report['root_cpu_delta_seconds'],0)
        self.assertTrue(any('decreased' in e for e in report['errors']))
        self.assertTrue(any('WinError 5' in e for e in report['errors']))

    def test_tree_uses_parent_relationships_not_shared_executable_names(self):
        entries=[{'pid':1,'ppid':0},{'pid':2,'ppid':1},{'pid':3,'ppid':2},
                 {'pid':4,'ppid':2},{'pid':8,'ppid':7},{'pid':7,'ppid':8}]
        self.assertEqual(metrics._descendants(entries,1),{1,2,3,4})
        self.assertEqual(metrics._descendants(entries,8),{7,8})

    def test_public_monitor_initial_final_samples_and_idempotence(self):
        class Backend:
            def __init__(self,pid):self.calls=0;self.closed=False
            def snapshot(self):
                self.calls+=1
                return [row(1,1,10+self.calls,100,200)],[]
            def close(self):self.closed=True
        with patch.object(metrics,'_Windows',Backend):
            monitor=metrics.Monitor(1,60).start()
            report=monitor.stop()
            self.assertIs(monitor.stop(),report)
            self.assertTrue(report['passed'])
            self.assertEqual(report['sample_count'],2)
            self.assertEqual(report['root_cpu_delta_seconds'],1)
            self.assertTrue(monitor._backend.closed)
            self.assertIn('started_at_utc',report)
            self.assertIn('ended_at_utc',report)
            with self.assertRaises(RuntimeError):monitor.start()

    def test_backend_exceptions_return_failed_measurement_and_cleanup(self):
        class Broken:
            def __init__(self,pid):self.closed=False
            def snapshot(self):raise OSError('Native counter failure')
            def close(self):self.closed=True
        with patch.object(metrics,'_Windows',Broken):
            monitor=metrics.Monitor(1,60).start();report=monitor.stop()
            self.assertFalse(report['passed'])
            self.assertTrue(any('Native counter failure' in e for e in report['errors']))
            self.assertTrue(monitor._backend.closed)

    def test_validation_and_crossplatform_import(self):
        for pid in (0,-1,True,1.5,1<<32):
            with self.assertRaises(ValueError):metrics.Monitor(pid)
        for interval in (0,-1,.001,61,float('nan'),float('inf')):
            with self.assertRaises(ValueError):metrics.Monitor(1,interval)
        with self.assertRaises(RuntimeError):metrics.Monitor(1).stop()
        if sys.platform!='win32':
            with self.assertRaises(OSError):metrics.Monitor(1).start()

    def test_identity_budget_failure_is_explicit_and_bounded(self):
        with patch.object(metrics,'MAX_IDENTITIES',2):
            a=metrics._Aggregate(1,100)
        a.observe([row(1,1,10,10,20),row(2,110,2,100,200),row(3,120,3,1000,2000)])
        report=a.result()
        self.assertFalse(report['passed'])
        self.assertEqual(len(report['observed_identities']),2)
        self.assertEqual(report['observed_identity_limit'],2)
        self.assertEqual(report['peak_sampled_tree_working_set_bytes'],110)
        self.assertTrue(any('budget exceeded' in e for e in report['errors']))

    def test_native_snapshot_rejects_old_orphan_and_its_descendants(self):
        processes={10:row(10,1000,2,100,200),20:row(20,100,8,900,1800),30:row(30,1100,3,700,1400)}
        native=FakeWindows([[entry(10,1),entry(20,10),entry(30,20)]],processes)
        rows,errors=native.snapshot()
        self.assertEqual([r['pid'] for r in rows],[10])
        self.assertEqual(native.memory_queries,[10])
        self.assertEqual(set(native.handles),{(10,1000)})
        self.assertEqual(native.discarded,[(20,100)])
        self.assertFalse(errors)
        a=metrics._Aggregate(10,2000);a.observe(rows,errors)
        self.assertTrue(a.result()['passed'])
        self.assertEqual(a.result()['peak_sampled_tree_working_set_bytes'],100)

    def test_native_snapshot_validates_recursive_parent_creation(self):
        processes={10:row(10,1000,2,100,200),20:row(20,2000,8,900,1800),
                   30:row(30,1500,3,700,1400),40:row(40,2100,4,800,1600)}
        native=FakeWindows([[entry(10,1),entry(20,10),entry(30,20),entry(40,30)]],processes)
        rows,errors=native.snapshot()
        self.assertEqual({r['pid'] for r in rows},{10,20})
        self.assertEqual(set(native.memory_queries),{10,20})
        self.assertNotIn((30,1500),native.handles)
        self.assertFalse(errors)

    def test_native_snapshot_finds_live_child_of_observed_exited_parent(self):
        processes={10:row(10,100,2,100,200),20:row(20,200,8,200,400),30:row(30,1100,3,900,1800)}
        native=FakeWindows([[entry(10,1),entry(20,10)],[entry(10,1),entry(30,20)]],processes)
        a=metrics._Aggregate(10,1000);a.observe(*native.snapshot())
        processes[20]['exit_filetime']=2000
        a.observe(*native.snapshot())
        report=a.result()
        self.assertTrue(report['passed'])
        self.assertEqual({p['pid'] for p in report['observed_identities']},{10,20,30})
        self.assertEqual(report['last_sample_tree_working_set_bytes'],1000)
        self.assertEqual(report['last_sample_tree_private_usage_bytes'],2000)

    def test_native_snapshot_rejects_child_born_after_parent_exit(self):
        processes={10:row(10,100,2,100,200),20:row(20,200,8,200,400),30:row(30,2100,3,900,1800)}
        native=FakeWindows([[entry(10,1),entry(20,10)],[entry(10,1),entry(30,20)]],processes)
        native.snapshot();processes[20]['exit_filetime']=2000
        rows,errors=native.snapshot()
        self.assertEqual({r['pid'] for r in rows},{10,20})
        self.assertNotIn(30,native.memory_queries)
        self.assertNotIn((30,2100),native.handles)
        self.assertFalse(errors)

    def test_monitor_thread_start_failure_closes_native_handles(self):
        class Backend:
            def __init__(self,pid):self.closed=False
            def snapshot(self):return [row(10,1,2,100,200)],[]
            def close(self):self.closed=True
        with patch.object(metrics,'_Windows',Backend),patch.object(metrics.threading.Thread,'start',side_effect=RuntimeError('cannot start thread')):
            monitor=metrics.Monitor(10)
            with self.assertRaisesRegex(RuntimeError,'cannot start thread'):monitor.start()
            self.assertTrue(monitor._backend.closed)

    def test_cli_existing_output_is_not_replaced(self):
        with tempfile.TemporaryDirectory() as directory:
            output=Path(directory)/'metrics.json';output.write_text('existing')
            result=subprocess.run([sys.executable,str(PATH),'--pid','1','--seconds','.05','--output',str(output)],text=True,capture_output=True,timeout=10)
            self.assertEqual(result.returncode,2)
            self.assertEqual(output.read_text(),'existing')
            self.assertIn('new path',result.stderr)


if __name__=='__main__':unittest.main()
