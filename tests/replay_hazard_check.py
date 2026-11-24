# needs: clang, vma, file:runtime/wb_merge_spv.h
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path
import runtime_include

DESCRIPTION = 'Build and run the replay hazard probes on the real runtime sources, on Linux, without game files or a GPU.'

ROOT = Path(__file__).resolve().parents[1]


def build(cc, src, exe, flags, includes):
    subprocess.run([cc, '-std=gnu11', '-O1', '-g', '-pthread', '-ffunction-sections', '-fdata-sections',
                    '-fno-omit-frame-pointer', '-Wno-bitwise-instead-of-logical', *flags, *includes, str(src),
                    '-Wl,--gc-sections', '-o', str(exe)], check=True)


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    ap.add_argument('--cc', default=shutil.which('clang-20') or shutil.which('clang') or 'clang')
    ap.add_argument('--vma-include', type=Path, default=Path(os.environ.get('VMA_INCLUDE', ROOT / 'toolchain' / 'vma')))
    ap.add_argument('--vulkan-include', type=Path, help='Vulkan headers, when not provided by the system')
    ap.add_argument('--spv-include', type=Path, default=ROOT / 'runtime', help='folder with the *_spv.h shader headers')
    ap.add_argument('--work', type=Path, default=ROOT / 'build' / 'test' / 'replay-hazard')
    ap.add_argument('--tsan', action='store_true', help='also build and run the drop order probe with ThreadSanitizer')
    a = ap.parse_args()
    if not sys.platform.startswith('linux'):
        ap.error('these pthread and fork probes run on Linux')
    if not (a.vma_include / 'vk_mem_alloc.h').is_file():
        ap.error('provide --vma-include for the directory holding vk_mem_alloc.h')
    if not (a.spv_include / 'wb_merge_spv.h').is_file():
        ap.error('the shader headers are missing: run tools/build_shaders.py or pass --spv-include')
    a.work.mkdir(parents=True, exist_ok=True)
    inc = [*runtime_include.flags(), '-I', str(a.spv_include), '-I', str(a.vma_include)]
    if a.vulkan_include:
        inc += ['-I', str(a.vulkan_include)]
    order = ROOT / 'tests' / 'usnap_order_check.c'
    life = ROOT / 'tests' / 'vkmem_lifetime_check.c'
    runs = [(a.work / 'usnap-order', order, [], {})]
    if a.tsan:
        runs.append((a.work / 'usnap-order-tsan', order, ['-fsanitize=thread'], {'TSAN_OPTIONS': 'exitcode=66'}))
    runs.append((a.work / 'vkmem-lifetime-asan', life, ['-fsanitize=address'], {}))
    worst = 0
    for exe, src, flags, env in runs:
        build(a.cc, src, exe, flags, inc)
        print(f'== {exe.name}', flush=True)
        r = subprocess.run([str(exe)], env={**os.environ, **env}, timeout=300)
        worst = worst or r.returncode
    return worst


if __name__ == '__main__':
    sys.exit(main())
