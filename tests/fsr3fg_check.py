"""Run the game executable's --probe-fsr3fg with Vulkan's validation on (runtime/fsr3fg_probe.c: FSR 3.1 frame
generation with the runtime's own passes, runtime/vk_fsr3fg.c, on a synthetic scene, without a window or the game).

The probe checks that the generated frame puts a moving square halfway between the two frames it is made from, keeps
the HUD drawn over the picture as it is, leaves the black bars around the picture black, shows nothing at the first
frame and at a reset, keeps a still scene still, frees its images when frame generation goes off, and that the passes
raise no validation error; then it times one generated frame at 2560x1080 and 1920x1080. This test passes when the
probe does, and prints its lines.

The executable needs a Vulkan device that runs the passes; without one, and in the Windows build (AMD's library
generates the frames there), the probe says SKIP and so does this test.

Usage: fsr3fg_check.py PATH_TO_GAME_EXE
"""
import os
import subprocess
import sys


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    env = dict(os.environ)
    env['NX2SARIA_VK_VALIDATION'] = '1'
    r = subprocess.run([sys.argv[1], '--probe-fsr3fg'], env=env, capture_output=True, text=True, errors='replace')
    lines = [ln for ln in r.stdout.splitlines() if ln.startswith('probe-fsr3fg:')]
    print('\n'.join(lines))
    last = lines[-1] if lines else ''
    if 'SKIP' in last:
        print('fsr3fg_check: SKIP')
        return 0
    ok = r.returncode == 0 and last.endswith('passed (0 validation errors)')
    print('fsr3fg_check: %s' % ('passed' if ok else 'FAILED'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
