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
