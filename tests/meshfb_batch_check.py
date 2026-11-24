# needs: exe, gpu
# args: {exe}
import os
import subprocess
import sys

USAGE = """Run the game executable's --probe-meshfb twice: with the mesh fallback's batch off and on
(NX2SARIA_MESH_FALLBACK_BATCH, runtime/vk/vk_exec_draw.c mfb_draw, runtime/gfx/mfb_batch.h).

The probe (runtime/gfx/mesh_fb_probe.c) draws mesh programs of the runtime's own through the replay, with and without a
task stage, with direct and GPU-written counts, one and two in one pass. It compares each picture of the fallback with
the mesh pipeline's (where the device has mesh shaders), and checks that a draw past the mesh buffer and a task draw
past the task buffer are drawn whole up to what fits and are counted.

With the batch on, the same pictures go through mfb_draw's batch and its way back to one by one: a task program's
first draws (no history yet) go one by one in a pass whose other draws are batched, and the passes are cut. The probe
prints a "batch:" line with its counts and checks that draws were batched, that some went one by one for want of
history, that passes were cut, and that no draw lacked a pre-pass command buffer. Validation is on in both runs; an
error while drawing fails the check, while those of the device's creation and teardown are only printed.

Usage: meshfb_batch_check.py PATH_TO_GAME_EXE
"""


def run(exe, batch):
    env = dict(os.environ)
    env['NX2SARIA_VK_VALIDATION'] = '1'
    env['NX2SARIA_MESH_FALLBACK_BATCH'] = str(batch)
    r = subprocess.run([exe, '--probe-meshfb'], env=env, capture_output=True, text=True, errors='replace')
    out = r.stdout + r.stderr
    lines = [ln for ln in r.stdout.splitlines() if ln.startswith('probe-meshfb:')]
    print('--- batch %d' % batch)
    print('\n'.join(lines))
    ok = not any(ln.startswith('probe-meshfb: FAILED:') for ln in lines)
    ok = ok and any('with background builds the picture was whole' in ln for ln in lines)
    ok = ok and any('task draw of' in ln and 'counted' in ln for ln in lines)
    if batch:
        ok = ok and any(ln.startswith('probe-meshfb: batch:') for ln in lines)
    errs = [ln for ln in out.splitlines() if 'vk validation error' in ln]
    drawing = [ln for ln in errs if 'vkCreateDevice()' not in ln and 'vkDestroyDevice' not in ln]
    print('validation errors: %d while drawing, %d at the device\'s creation or destruction; exit code %d'
          % (len(drawing), len(errs) - len(drawing), r.returncode))
    for ln in drawing[:5]:
        print('  ' + ln[:300])
    return ok and not drawing


def main():
    if len(sys.argv) != 2:
        print(USAGE)
        return 2
    ok = run(sys.argv[1], 0) & run(sys.argv[1], 1)
    print('meshfb_batch_check: %s' % ('passed' if ok else 'FAILED'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
