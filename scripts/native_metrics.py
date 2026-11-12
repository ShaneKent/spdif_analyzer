#!/usr/bin/env python3
"""Read-only Windows process-tree CPU/memory sampling; no application connection."""
from __future__ import annotations
import argparse
import ctypes
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import threading
import time

FILETIME_EPOCH = 116444736000000000
MAX_IDENTITIES = 4096


def _utc():
    return datetime.now(timezone.utc).isoformat()


def _filetime_now():
    return FILETIME_EPOCH + time.time_ns() // 100


def _descendants(entries, root):
    children = {}
    for row in entries:
        children.setdefault(row['ppid'], []).append(row['pid'])
    found, queue = {root}, [root]
    while queue:
        for pid in children.get(queue.pop(), []):
            if pid not in found:
                found.add(pid)
                queue.append(pid)
    return found


class _Aggregate:
    """Incremental accounting; mockable independently of Win32 enumeration."""
    def __init__(self, root_pid, started_filetime):
        self.root_pid = root_pid
        self.identity_limit = MAX_IDENTITIES
        self.started_filetime = started_filetime
        self.root_identity = None
        self.identities = {}
        self.errors = []
        self.samples = 0
        self.peak_ws = self.peak_private = 0
        self.last_ws = self.last_private = 0

    def error(self, message):
        message = str(message)[:1000]
        if message not in self.errors and len(self.errors) < 1000:
            self.errors.append(message)

    def observe(self, rows, errors=()):
        self.samples += 1
        for error in errors:
            self.error(error)
        active = [row for row in rows if row.get('active', True)]
        root = next((row for row in active if row['pid'] == self.root_pid), None)
        if root is None:
            self.error('Root process missing or inaccessible during sampling')
        else:
            key = (root['pid'], root['creation_filetime'])
            if self.root_identity is None:
                self.root_identity = key
            elif key != self.root_identity:
                self.error('Root PID was reused; original process identity no longer exists')
        ws = private = 0
        for row in rows:
            key = (row['pid'], row['creation_filetime'])
            # A reused root is not the authorized application. The native backend
            # also refuses to enumerate descendants of the replacement process.
            if row['pid'] == self.root_pid and key != self.root_identity:
                continue
            cpu = int(row['cpu_100ns'])
            if cpu < 0:
                self.error('Negative process CPU counter')
                continue
            state = self.identities.get(key)
            if state is None:
                if len(self.identities) >= self.identity_limit:
                    self.error('Process identity budget exceeded ({})'.format(self.identity_limit))
                    continue
                # Pre-existing children have an observation baseline, not zero:
                # their earlier lifetime CPU must not be attributed to this run.
                birth_counted = row['creation_filetime'] >= self.started_filetime
                state = {'pid': row['pid'], 'ppid': row['ppid'], 'name': row.get('name', ''),
                         'creation_filetime': str(row['creation_filetime']),
                         'first_sample': self.samples, 'last_sample': self.samples,
                         'cpu_baseline_100ns': 0 if birth_counted else cpu,
                         'cpu_last_100ns': cpu, 'cpu_from_birth': birth_counted,
                         'peak_sampled_working_set_bytes': 0, 'peak_sampled_private_usage_bytes': 0,
                         'active_at_last_observation': row.get('active', True)}
                self.identities[key] = state
            elif cpu < state['cpu_last_100ns']:
                self.error('CPU counter decreased for the same PID/creation identity')
                continue
            state['cpu_last_100ns'] = cpu
            state['last_sample'] = self.samples
            state['active_at_last_observation'] = row.get('active', True)
            if row.get('active', True):
                current_ws, current_private = int(row['working_set_bytes']), int(row['private_usage_bytes'])
                if current_ws < 0 or current_private < 0:
                    self.error('Negative process memory counter')
                    continue
                ws += current_ws
                private += current_private
                state['peak_sampled_working_set_bytes'] = max(state['peak_sampled_working_set_bytes'], current_ws)
                state['peak_sampled_private_usage_bytes'] = max(state['peak_sampled_private_usage_bytes'], current_private)
        self.last_ws, self.last_private = ws, private
        self.peak_ws = max(self.peak_ws, ws)
        self.peak_private = max(self.peak_private, private)

    def result(self):
        identities = []
        root_cpu = children_cpu = 0
        for key, state in sorted(self.identities.items()):
            item = dict(state)
            delta = state['cpu_last_100ns'] - state['cpu_baseline_100ns']
            item['cpu_delta_100ns'] = str(delta)
            item['cpu_delta_seconds'] = delta / 10000000
            item['cpu_baseline_100ns'] = str(item['cpu_baseline_100ns'])
            item['cpu_last_100ns'] = str(item['cpu_last_100ns'])
            if key == self.root_identity:
                root_cpu += delta
            else:
                children_cpu += delta
            identities.append(item)
        return {'schema_version': 1, 'passed': not self.errors, 'root_pid': self.root_pid,
                'root_creation_filetime': str(self.root_identity[1]) if self.root_identity else None,
                'sample_count': self.samples, 'observed_identities': identities,
                'monitor_start_filetime': str(self.started_filetime), 'observed_identity_limit': self.identity_limit,
                'creation_filetime_encoding': 'uint64 decimal string; 100ns intervals since 1601-01-01 UTC',
                'peak_sampled_tree_working_set_bytes': self.peak_ws,
                'peak_sampled_tree_private_usage_bytes': self.peak_private,
                'last_sample_tree_working_set_bytes': self.last_ws,
                'last_sample_tree_private_usage_bytes': self.last_private,
                'root_cpu_delta_seconds': root_cpu / 10000000,
                'children_cpu_delta_seconds': children_cpu / 10000000,
                'tree_cpu_delta_seconds': (root_cpu + children_cpu) / 10000000,
                'errors': list(self.errors),
                'measurement_notes': [
                    'Read-only Toolhelp32 enumeration, GetProcessTimes and PROCESS_MEMORY_COUNTERS_EX.PrivateUsage.',
                    'Working-set sums include shared pages counted in multiple processes; they are not unique physical RAM.',
                    'Peaks are maxima of concurrent sampled sums, not sums of individual process lifetime peaks.',
                    'CPU for pre-existing identities begins at first observation; newly born children count from birth only if born after monitor start.',
                    'Short-lived processes entirely between samples can be missed; process-tree samples are not globally atomic.',
                    'If an intermediate parent was never observed before exit, its live descendants may be unidentifiable from sampled parent-PID links.'
                ]}


class _Windows:
    def __init__(self, root_pid):
        if os.name != 'nt':
            raise OSError('native_metrics requires Windows; pure aggregation tests remain portable')
        from ctypes import wintypes as w
        self.root_pid = root_pid
        self.root_identity = None
        self.handles = {}
        self.k = ctypes.WinDLL('kernel32', use_last_error=True)
        self.psapi = ctypes.WinDLL('psapi', use_last_error=True)
        class Entry(ctypes.Structure):
            _fields_ = [('dwSize', w.DWORD), ('cntUsage', w.DWORD), ('th32ProcessID', w.DWORD),
                        ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', w.DWORD),
                        ('cntThreads', w.DWORD), ('th32ParentProcessID', w.DWORD),
                        ('pcPriClassBase', w.LONG), ('dwFlags', w.DWORD), ('szExeFile', w.WCHAR * 260)]
        class Memory(ctypes.Structure):
            _fields_ = [('cb', w.DWORD), ('PageFaultCount', w.DWORD)] + [
                (name, ctypes.c_size_t) for name in ('PeakWorkingSetSize', 'WorkingSetSize', 'QuotaPeakPagedPoolUsage',
                'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage', 'QuotaNonPagedPoolUsage',
                'PagefileUsage', 'PeakPagefileUsage', 'PrivateUsage')]
        self.Entry, self.Memory, self.Filetime = Entry, Memory, w.FILETIME
        self.k.CreateToolhelp32Snapshot.argtypes = [w.DWORD, w.DWORD]
        self.k.CreateToolhelp32Snapshot.restype = w.HANDLE
        self.k.Process32FirstW.argtypes = self.k.Process32NextW.argtypes = [w.HANDLE, ctypes.POINTER(Entry)]
        self.k.Process32FirstW.restype = self.k.Process32NextW.restype = w.BOOL
        self.k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
        self.k.OpenProcess.restype = w.HANDLE
        self.k.CloseHandle.argtypes = [w.HANDLE]
        self.k.CloseHandle.restype = w.BOOL
        self.k.GetProcessTimes.argtypes = [w.HANDLE] + [ctypes.POINTER(w.FILETIME)] * 4
        self.k.GetProcessTimes.restype = w.BOOL
        self.psapi.GetProcessMemoryInfo.argtypes = [w.HANDLE, ctypes.POINTER(Memory), w.DWORD]
        self.psapi.GetProcessMemoryInfo.restype = w.BOOL

    @staticmethod
    def ft(value):
        return (value.dwHighDateTime << 32) | value.dwLowDateTime

    def times(self, handle):
        values = [self.Filetime() for _ in range(4)]
        if not self.k.GetProcessTimes(handle, *(ctypes.byref(v) for v in values)):
            raise OSError('GetProcessTimes failed: WinError {}'.format(ctypes.get_last_error()))
        return self.ft(values[0]), self.ft(values[1]), self.ft(values[2]) + self.ft(values[3])

    def entries(self):
        handle = self.k.CreateToolhelp32Snapshot(2, 0)
        if handle == ctypes.c_void_p(-1).value:
            raise OSError('Toolhelp32 snapshot failed: WinError {}'.format(ctypes.get_last_error()))
        try:
            result = []
            entry = self.Entry();entry.dwSize = ctypes.sizeof(entry)
            available = self.k.Process32FirstW(handle, ctypes.byref(entry))
            while available:
                result.append({'pid': int(entry.th32ProcessID), 'ppid': int(entry.th32ParentProcessID), 'name': entry.szExeFile})
                available = self.k.Process32NextW(handle, ctypes.byref(entry))
            error = ctypes.get_last_error()
            if error != 18:  # ERROR_NO_MORE_FILES is normal enumeration EOF.
                raise OSError('Toolhelp32 enumeration failed: WinError {}'.format(error))
            return result
        finally:
            self.k.CloseHandle(handle)

    def opened(self, entry):
        handle = self.k.OpenProcess(0x410, False, entry['pid'])  # QUERY_INFORMATION | VM_READ
        if not handle:
            raise OSError('OpenProcess(pid={}) failed: WinError {}'.format(entry['pid'], ctypes.get_last_error()))
        try:
            created, exited, cpu = self.times(handle)
            key = (entry['pid'], created)
            if key in self.handles:
                self.k.CloseHandle(handle)
                return key, self.handles[key]['handle']
            if len(self.handles) >= MAX_IDENTITIES:
                raise OSError('Process identity handle budget exceeded ({})'.format(MAX_IDENTITIES))
            self.handles[key] = {'handle': handle, 'entry': entry}
            return key, handle
        except BaseException:
            self.k.CloseHandle(handle)
            raise

    def row(self, key, handle):
        created, exited, cpu = self.times(handle)
        entry = self.handles[key]['entry']
        row = {**entry, 'creation_filetime': created, 'cpu_100ns': cpu, 'active': not bool(exited),
               'working_set_bytes': 0, 'private_usage_bytes': 0}
        if not exited:
            memory = self.Memory();memory.cb = ctypes.sizeof(memory)
            if not self.psapi.GetProcessMemoryInfo(handle, ctypes.byref(memory), memory.cb):
                raise OSError('GetProcessMemoryInfo(pid={}) failed: WinError {}'.format(key[0], ctypes.get_last_error()))
            row['working_set_bytes'] = int(memory.WorkingSetSize)
            row['private_usage_bytes'] = int(memory.PrivateUsage)
        return row

    def discard(self, key):
        record = self.handles.pop(key, None)
        if record is not None:
            self.k.CloseHandle(record['handle'])

    def snapshot(self):
        entries = self.entries()
        current = {e['pid']: e for e in entries}
        children = {}
        for entry in entries:
            children.setdefault(entry['ppid'], []).append(entry)
        errors, rows = [], []
        # Only admitted identities may seed lineage or contribute counters.
        # Parent PIDs alone are insufficient: an older orphan can retain a
        # recycled parent's PID, even when the monitored root is still valid.
        admitted = {key for key, record in self.handles.items() if record.get('admitted', False)}
        root = current.get(self.root_pid)
        if root is None:
            errors.append('Root PID missing from Toolhelp32 snapshot')
        else:
            try:
                key, handle = self.opened(root)
                if self.root_identity is None:
                    self.root_identity = key
                if key != self.root_identity:
                    errors.append('Root PID reused; replacement process is excluded')
                    if key not in admitted:
                        self.discard(key)
                else:
                    self.handles[key]['admitted'] = True
                    admitted.add(key)
            except OSError as error:
                errors.append(str(error))
        queue = list(admitted)
        visited = set()
        while queue:
            parent = queue.pop()
            if parent in visited:
                continue
            visited.add(parent)
            try:
                parent_created, parent_exited, _ = self.times(self.handles[parent]['handle'])
            except OSError as error:
                errors.append(str(error))
                continue
            # An observed parent can exit after spawning a still-live child.
            # Its retained creation/exit window establishes that child's link
            # even when the parent is absent from the current Toolhelp list.
            for entry in children.get(parent[0], []):
                if entry['pid'] == parent[0]:
                    continue
                child = None
                try:
                    child, handle = self.opened(entry)
                    child_created, _, _ = self.times(handle)
                    valid = child_created >= parent_created and (not parent_exited or child_created <= parent_exited)
                    if not valid:
                        if child not in admitted:
                            self.discard(child)
                        continue
                    self.handles[child]['admitted'] = True
                    admitted.add(child)
                    if child not in visited:
                        queue.append(child)
                except OSError as error:
                    if child is not None and child not in admitted:
                        self.discard(child)
                    errors.append(str(error))
        for key in sorted(admitted):
            try:
                rows.append(self.row(key, self.handles[key]['handle']))
            except OSError as error:
                errors.append(str(error))
        return rows, errors

    def close(self):
        for record in self.handles.values():
            self.k.CloseHandle(record['handle'])
        self.handles.clear()


class Monitor:
    def __init__(self, root_pid, interval=.25):
        if isinstance(root_pid, bool) or not isinstance(root_pid, int) or not 1 <= root_pid <= 0xffffffff:
            raise ValueError('root_pid must be a positive Windows PID')
        if isinstance(interval, bool) or not isinstance(interval, (int, float)) or not .05 <= interval <= 60:
            raise ValueError('interval must be .05..60 seconds')
        self.root_pid, self.interval = root_pid, float(interval)
        self._backend = self._aggregate = self._thread = None
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._report = None

    def _sample(self):
        try:
            rows, errors = self._backend.snapshot()
        except Exception as error:
            rows, errors = [], [type(error).__name__ + ": " + str(error)]
        with self._lock:
            self._aggregate.observe(rows, errors)

    def start(self):
        if self._backend is not None:
            raise RuntimeError('Monitor can only be started once')
        self._backend = _Windows(self.root_pid)
        self.started_at_utc = _utc();self._start_clock = time.perf_counter()
        self._aggregate = _Aggregate(self.root_pid, _filetime_now())
        try:
            self._sample()
            def run():
                while not self._stop.wait(self.interval):
                    self._sample()
            self._thread = threading.Thread(target=run, name='native-metrics', daemon=True)
            self._thread.start()
            return self
        except BaseException:
            self._stop.set()
            self._backend.close()
            raise

    def stop(self):
        if self._backend is None:
            raise RuntimeError('Monitor has not started')
        if self._report is not None:
            return self._report
        self._stop.set();self._thread.join()
        try:
            self._sample()
            with self._lock:
                report = self._aggregate.result()
            report.update({'started_at_utc': self.started_at_utc, 'ended_at_utc': _utc(),
                           'elapsed_seconds': time.perf_counter() - self._start_clock,
                           'interval_seconds': self.interval})
            self._report = report
            return report
        finally:
            self._backend.close()


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pid', type=int, required=True)
    p.add_argument('--seconds', type=float, default=10)
    p.add_argument('--interval', type=float, default=.25)
    p.add_argument('--output', required=True, help='New JSON output path; existing files are not overwritten')
    args = p.parse_args(argv)
    if not .05 <= args.seconds <= 86400:
        p.error('--seconds must be .05..86400')
    try:
        output = Path(args.output)
        if output.exists():
            raise ValueError('Output must be a new path')
        monitor = Monitor(args.pid, args.interval).start()
        try:
            time.sleep(args.seconds)
        finally:
            result = monitor.stop()
        with output.open('x', encoding='utf-8') as stream:
            json.dump(result, stream, indent=2, allow_nan=False);stream.write('\n')
        return 0 if result['passed'] else 1
    except (OSError, ValueError, RuntimeError) as error:
        print('native_metrics: ' + str(error), file=__import__('sys').stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
