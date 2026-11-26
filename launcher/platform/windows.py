import ctypes
import hashlib
import re
import os
import subprocess
from ctypes import wintypes
from pathlib import Path


class ProcessEntry(ctypes.Structure):
    _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD), ('th32ProcessID', wintypes.DWORD),
                ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wintypes.DWORD),
                ('cntThreads', wintypes.DWORD), ('th32ParentProcessID', wintypes.DWORD),
                ('pcPriClassBase', wintypes.LONG), ('dwFlags', wintypes.DWORD), ('szExeFile', wintypes.WCHAR * 260)]


def game_running(executable, owner_pid=None):
    try:
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
        kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
        kernel.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
        kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                       ctypes.POINTER(wintypes.DWORD)]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
        if snapshot == wintypes.HANDLE(-1).value:
            return False
        parents = {}
        candidates = []
        try:
            entry = ProcessEntry()
            entry.dwSize = ctypes.sizeof(entry)
            more = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
            while more:
                pid = int(entry.th32ProcessID)
                parents[pid] = int(entry.th32ParentProcessID)
                if entry.szExeFile.casefold() == os.path.basename(executable).casefold():
                    candidates.append(pid)
                more = kernel.Process32NextW(snapshot, ctypes.byref(entry))
        finally:
            kernel.CloseHandle(snapshot)
        for pid in candidates:
            if owner_pid is not None:
                parent = pid
                seen = set()
                while parent and parent not in seen and parent != owner_pid:
                    seen.add(parent)
                    parent = parents.get(parent, 0)
                if parent != owner_pid:
                    continue
            handle = kernel.OpenProcess(0x1000, False, pid)
            if not handle:
                continue
            try:
                buf = ctypes.create_unicode_buffer(32768)
                size = wintypes.DWORD(len(buf))
                if kernel.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size)):
                    actual = os.path.normcase(os.path.abspath(buf.value))
                    expected = os.path.normcase(os.path.abspath(executable))
                    if actual == expected:
                        return True
                    try:
                        if os.path.samefile(actual, expected):
                            return True
                    except OSError:
                        pass
            finally:
                kernel.CloseHandle(handle)
    except (OSError, AttributeError, ValueError):
        return False
    return False


class JobAccounting(ctypes.Structure):
    _fields_ = [('user_time', ctypes.c_longlong), ('kernel_time', ctypes.c_longlong),
                ('period_user_time', ctypes.c_longlong), ('period_kernel_time', ctypes.c_longlong),
                ('page_faults', wintypes.DWORD), ('total', wintypes.DWORD),
                ('active', wintypes.DWORD), ('terminated', wintypes.DWORD)]


def build_group_kernel():
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    k.CreateJobObjectW.restype = wintypes.HANDLE
    k.OpenJobObjectW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
    k.OpenJobObjectW.restype = wintypes.HANDLE
    k.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    k.WaitForSingleObject.restype = wintypes.DWORD
    k.GetCurrentProcess.restype = wintypes.HANDLE
    filetime = ctypes.POINTER(wintypes.FILETIME)
    k.GetProcessTimes.argtypes = [wintypes.HANDLE, filetime, filetime, filetime, filetime]
    k.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    k.CloseHandle.argtypes = [wintypes.HANDLE]
    k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    k.OpenProcess.restype = wintypes.HANDLE
    k.IsProcessInJob.argtypes = [wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL)]
    k.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    k.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
    k.QueryInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                              wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
    return k


def creation_time(kernel, process):
    created, exited, system, user = (wintypes.FILETIME() for _ in range(4))
    if not kernel.GetProcessTimes(process, ctypes.byref(created), ctypes.byref(exited),
                                  ctypes.byref(system), ctypes.byref(user)):
        raise ctypes.WinError(ctypes.get_last_error())
    return (created.dwHighDateTime << 32) | created.dwLowDateTime


def build_job_name(root, token):
    if not re.fullmatch(r'[a-f0-9]{32}', token):
        raise ValueError('invalid build token')
    digest = hashlib.sha256(os.fsencode(os.path.normcase(str(root.resolve())))).hexdigest()[:24]
    return f'Local\\NX2Saria-build-{digest}-{token}'


def active_build_members(kernel, job):
    accounting = JobAccounting()
    if not kernel.QueryInformationJobObject(job, 1, ctypes.byref(accounting), ctypes.sizeof(accounting), None):
        raise ctypes.WinError(ctypes.get_last_error())
    return accounting.active


def active_build_job(name):
    k = build_group_kernel()
    job = k.OpenJobObjectW(4, False, name)
    if not job:
        error = ctypes.get_last_error()
        if error == 2:
            return 0
        raise ctypes.WinError(error)
    try:
        return active_build_members(k, job)
    finally:
        k.CloseHandle(job)


def stop_build_job(name):
    k = build_group_kernel()
    job = k.OpenJobObjectW(8, False, name)
    if not job:
        if ctypes.get_last_error() == 2:
            return
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        if not k.TerminateJobObject(job, 1):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        k.CloseHandle(job)


def build_process_status(pid, start):
    k = build_group_kernel()
    process = k.OpenProcess(0x101000, False, pid)
    if not process:
        error = ctypes.get_last_error()
        if error == 87:
            return 'gone'
        raise ctypes.WinError(error)
    try:
        if creation_time(k, process) != start:
            return 'reused'
        status = k.WaitForSingleObject(process, 0)
        if status == 0:
            return 'gone'
        if status == 258:
            return 'alive'
        raise ctypes.WinError(ctypes.get_last_error())
    finally:
        k.CloseHandle(process)


def last_boot_utc():
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.GetSystemDirectoryW.argtypes = [wintypes.LPWSTR, wintypes.UINT]
    k.GetSystemDirectoryW.restype = wintypes.UINT
    directory = ctypes.create_unicode_buffer(32768)
    size = k.GetSystemDirectoryW(directory, len(directory))
    if not 0 < size < len(directory):
        return None
    powershell = Path(directory.value) / 'WindowsPowerShell' / 'v1.0' / 'powershell.exe'
    try:
        result = subprocess.run(
            [str(powershell), '-NoLogo', '-NoProfile', '-NonInteractive', '-Command',
             '$os = Get-CimInstance -ClassName Win32_OperatingSystem; $os.LastBootUpTime.ToUniversalTime().Ticks'],
            capture_output=True, timeout=5, creationflags=0x08000000)
    except (OSError, subprocess.TimeoutExpired):
        return None
    value = result.stdout.strip()
    return value.decode('ascii') if result.returncode == 0 and re.fullmatch(rb'[0-9]{15,19}', value) else None


class BuildJobGroup:
    def __init__(self, root, token):
        name = build_job_name(root, token)
        self.kernel = k = build_group_kernel()
        process = k.GetCurrentProcess()
        self.identity = {'kind': 'windows_pid', 'pid': os.getpid(), 'start': creation_time(k, process)}
        self.handle = k.CreateJobObjectW(None, name)
        if self.handle:
            existing = ctypes.get_last_error() == 183
            if existing or not k.AssignProcessToJobObject(self.handle, process):
                k.CloseHandle(self.handle)
                self.handle = None
            else:
                self.identity.update(kind='windows_job', name=name)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        if self.handle:
            self.kernel.CloseHandle(self.handle)

    def active_members(self):
        if not self.handle:
            return 0
        return max(0, active_build_members(self.kernel, self.handle) - 1)

    def stop(self, primary_pid):
        if not self.handle:
            if primary_pid:
                subprocess.run(['taskkill', '/T', '/F', '/PID', str(primary_pid)], capture_output=True,
                               creationflags=0x08000000, timeout=15)
            return
        k = self.kernel
        size = 8
        while True:
            size = max(size, self.active_members() + 1)
            ids = (ctypes.c_size_t * size)()

            class JobPids(ctypes.Structure):
                _fields_ = [('assigned', wintypes.DWORD), ('returned', wintypes.DWORD), ('ids', type(ids))]

            members = JobPids()
            if k.QueryInformationJobObject(self.handle, 3, ctypes.byref(members), ctypes.sizeof(members), None):
                break
            if ctypes.get_last_error() != 234:
                raise ctypes.WinError(ctypes.get_last_error())
            size *= 2
        for pid in members.ids[:members.returned]:
            if pid == self.identity['pid']:
                continue
            process = k.OpenProcess(0x1001, False, pid)
            if not process:
                continue
            try:
                verified = wintypes.BOOL()
                if k.IsProcessInJob(process, self.handle, ctypes.byref(verified)) and verified.value:
                    k.TerminateProcess(process, 1)
            finally:
                k.CloseHandle(process)
