"""Build and run tests/log_stall_check.c: runtime/log_stall.c (the local time of the lines that mark an event, the counts
of the events whose own lines are limited, the memory and I/O pressure line) and runtime/platform/proc_pressure.h (the
readers of /proc/pressure/* and /proc/meminfo), with the platform's clocks, zone and readings replaced by stand-ins.
Then checks statically that each place that writes one of those lines goes through them: the worst frame of a
frame-time window, the presenter's failed acquires and watchdog, the physical memory map's refusals, a slow pipeline
cache save, a fatal error, a crash report and a lost device, and the dma-buf spans at the end of the run.
No game files, no GPU, any Python.

Usage: log_stall_check.py
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / 'runtime'
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    """$CC, else MSYS2 UCRT64's clang, else clang on PATH (as tests/diff/common.py picks it, without its Unicorn)."""
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def source(name):
    """A runtime file without its comments, so a name in a comment is not a call."""
    text = (RT / name).read_text(encoding='utf-8')
    return re.sub(r'//[^\n]*', '', re.sub(r'/\*.*?\*/', '', text, flags=re.S))


def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def static_checks():
    bad = 0

    def check(what, ok):
        nonlocal bad
        print(f'static: {what}: {"ok" if ok else "FAIL"}', flush=True)
        bad += not ok

    perf = source('perf.c')
    report = function(perf, 'static void ft_report(uint64_t now, const char *tag)')
    worst = report[report.index('worst frame'):]
    check('perf.c ft_report writes the local time on each worst frame', 'log_stamp(' in report[:report.index('worst frame')]
          and 'at %s;' in worst and '->at' in report)
    check('perf.c ft_report ends with the events line and the system line, whole for the exit and a fatal error',
          'log_events_text(' in report and 'log_pressure_text(' in report and '"exit"' in report
          and '"at a fatal error"' in report and report.index('log_events_text(') > report.index('worst frame'))
    frame = function(perf, 'static uint64_t ft_frame(')
    check('perf.c ft_frame keeps the present time of a worst frame and starts the pressure baseline with the first period',
          'ft.worst[i].at = now' in frame and 'log_pressure_baseline(' in frame)
    check('perf.c: a fatal error\'s summary goes through ft_report, so it has the events and the system line',
          'ft_report(kern_now_ns(), "at a fatal error")' in function(perf, 'void perf_summary_flush_try(void)'))

    present = source('gfx_present.c')
    show = function(present, 'static int ng_show(')
    check('gfx_present.c ng_show counts the failed acquires and writes the first few with the time',
          'log_event(LS_ACQUIRE_FAILED)' in show and 'log_stamp(' in show and 'static int told' not in show)
    thread = function(present, 'static void ng_thread(void *arg)')
    check('gfx_present.c ng_thread counts the watchdog\'s firings and writes the first few with the time',
          'log_event(LS_FG_WATCHDOG)' in thread and 'log_stamp(' in thread and 'static int told' not in thread)

    memory = source('memory.c')
    alias = function(memory, 'static uint32_t alias_map(')
    check('memory.c alias_map counts the refusals of the physical memory map, and writes the first of each kind',
          'log_event(unmap ? LS_UNMAP_PHYS_FAILED : LS_MAP_PHYS_FAILED)' in alias and 'if (due || other)' in alias)

    save = function(source('vk_pipeline.c'), 'static int cache_save_one(')
    check('vk_pipeline.c cache_save_one says when a slow save ended',
          'SAVE_SLOW_MS' in save and 'log_stamp(' in save and 'copy_ms' in save)

    check('dispatch.c fatal_end writes the local time after the FATAL line',
          'log_stamp(' in function(source('dispatch.c'), 'static void fatal_end(int code, const char *fmt, va_list ap) {'))
    check('main.c crash_handler writes the local time after the FATAL line',
          'log_stamp(' in function(source('main.c'), 'static void crash_handler(const PlatCrash *x)'))
    lost = function(source('vk_device.c'), 'void vk_device_lost_check(VkResult r, const char *where)')
    check('vk_device.c vk_device_lost_check writes when the loss was seen, before the report',
          'log_stamp(' in lost and lost.index('log_stamp(') < lost.index('device_fault_report('))

    exit_log = function(source('vk_memory.c'), 'void vk_mem_exit_log(void)')
    check('vk_memory.c vk_mem_exit_log writes the dma-buf spans of the end of the run', 'span_report(' in exit_log)
    spans = source('vk_memory.c')
    check('vk_memory.c: the spans at frames 600 and 1800 go through the same report',
          'span_report(when, 0)' in function(spans, 'static void span_log(uint64_t frame)'))

    stall = (RT / 'log_stall.c').read_text(encoding='utf-8')
    check('log_stall.c defines LOG_STALL_IMPL before its header, so it does not take the header\'s stand-ins',
          stall.index('#define LOG_STALL_IMPL') < stall.index('#include "log_stall.h"'))
    return bad


def main():
    bad = static_checks()
    work = ROOT / 'build' / 'test' / 'log_stall'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('log_stall_check.exe' if os.name == 'nt' else 'log_stall_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread', '-I', str(RT),
                           str(ROOT / 'tests' / 'log_stall_check.c'), '-o', str(exe)], env=env)
    return subprocess.run([str(exe)], env=env).returncode or (1 if bad else 0)


if __name__ == '__main__':
    sys.exit(main())
