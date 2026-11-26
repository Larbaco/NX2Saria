import ctypes
import os
from ctypes import wintypes


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
    k.GetCurrentProcess.restype = wintypes.HANDLE
    filetime = ctypes.POINTER(wintypes.FILETIME)
    k.GetProcessTimes.argtypes = [wintypes.HANDLE, filetime, filetime, filetime, filetime]
    k.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    k.CloseHandle.argtypes = [wintypes.HANDLE]
    k.QueryInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                              wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
    return k


def creation_time(kernel, process):
    created, exited, system, user = (wintypes.FILETIME() for _ in range(4))
    if not kernel.GetProcessTimes(process, ctypes.byref(created), ctypes.byref(exited),
                                  ctypes.byref(system), ctypes.byref(user)):
        raise ctypes.WinError(ctypes.get_last_error())
    return (created.dwHighDateTime << 32) | created.dwLowDateTime


class BuildJobGroup:
    def __init__(self, root, token):
        import hashlib
        import re
        if not re.fullmatch(r'[a-f0-9]{32}', token):
            raise ValueError('invalid build token')
        digest = hashlib.sha256(os.fsencode(os.path.normcase(str(root.resolve())))).hexdigest()[:24]
        name = f'Local\\NX2Saria-build-{digest}-{token}'
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

    def drain(self):
        import time
        if not self.handle:
            return
        accounting = JobAccounting()
        while True:
            if not self.kernel.QueryInformationJobObject(self.handle, 1, ctypes.byref(accounting),
                                                          ctypes.sizeof(accounting), None):
                raise ctypes.WinError(ctypes.get_last_error())
            if accounting.active <= 1:
                return
            time.sleep(0.1)
