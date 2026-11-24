# needs: windows
import os
import shutil
import subprocess
import sys
from pathlib import Path
import runtime_include

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

STUBS = ['cfg_get', 'cfg_value', 'plat_mem_info', 'plat_now_ns', 'rc_vkCmdBindPipeline', 'rc_vkCmdCopyBuffer',
         'rc_vkCmdCopyBufferToImage', 'rc_vkCmdDispatch', 'rc_vkCmdPipelineBarrier2', 'rc_vkCmdPushConstants',
         'rc_vkCmdWriteTimestamp2', 'rec_push', 'rt_fatal', 'rt_nvn_api_version', 'vkCreateComputePipelines',
         'vkCreateDescriptorSetLayout', 'vkCreatePipelineLayout', 'vkCreateQueryPool', 'vkCreateShaderModule',
         'vkDestroyDescriptorSetLayout', 'vkDestroyPipeline', 'vkDestroyPipelineLayout', 'vkDestroyQueryPool',
         'vkDestroyShaderModule', 'vkGetPhysicalDeviceFeatures2', 'vkGetPhysicalDeviceFormatProperties',
         'vkGetPhysicalDeviceQueueFamilyProperties', 'vkGetQueryPoolResults', 'vkResetQueryPool', 'vk_crumb', 'vk_dpa',
         'vk_staging', 'vk_vma_destroy_buffer']
SHADERS = ['astc_decode', 'bc3_encode']


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def vma_include():
    if os.environ.get('VMA_INCLUDE'):
        return os.environ['VMA_INCLUDE']
    return str(MSYS_BIN.parent / 'include' / 'vma')


def main():
    if os.name != 'nt':
        print('astc_trim_check: Windows only (the check uses SRW locks and Windows threads); skipped')
        return 0
    work = ROOT / 'build' / 'test' / 'astc_trim'
    work.mkdir(parents=True, exist_ok=True)
    stubs = work / 'stubs.c'
    stubs.write_text('// Stand-ins written by tests/astc_trim_check.py: each returns 0.\n' +
                     ''.join(f'long long {s}() {{ return 0; }}\n' for s in STUBS))
    for name in SHADERS:
        (work / f'{name}_spv.h').write_text('// A stand-in written by tests/astc_trim_check.py.\n#pragma once\n'
                                            f'#include <stdint.h>\nstatic const uint32_t {name}_spv[] = {{0}};\n')
    exe = work / 'astc_trim_check.exe'
    env = dict(os.environ)
    if MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-g', '-w', '-fno-strict-aliasing', '-march=x86-64-v3',
                           *runtime_include.flags(rt), '-I', str(work), '-I', vma_include(),
                           str(ROOT / 'tests' / 'astc_trim_check.c'), str(rt / 'platform' / 'common.c'), str(stubs),
                           '-o', str(exe), '-lws2_32'], env=env)
    try:
        return subprocess.run([str(exe)], env=env, timeout=60).returncode
    except subprocess.TimeoutExpired:
        print('astc_trim_check: FAIL: the check did not end within 60 s')
        return 1


if __name__ == '__main__':
    sys.exit(main())
