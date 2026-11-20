"""Check the 1.1.1 lens patch through the normal lifter and code emitter."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'recomp'))
from gen import Emitter  # noqa: E402
from layout import FIRST_BASE  # noqa: E402
from lifter import Lifter  # noqa: E402
from patches import load  # noqa: E402


def main():
    source = (ROOT / 'runtime/debug_tools.c').read_text()
    assert 'atomic_fetch_or_explicit(&fov_diagnostics, reason' in source
    assert 'if (fov_first(reason)) rt_log("fov: left lens alone: %s", text);' in source
    assert 'atomic_flag_test_and_set_explicit(&fov_hook_logged' in source
    assert '!memcmp(name, "PlayerCamera", 12)' in source
    assert '!memcmp(name, "EventCamera", 11)' in source
    assert 'Camera zero\'s bit 0 at +0x88 stayed set in gameplay' in source
    assert 'FOV_MESSAGE_LOG' not in source
    resolution = (ROOT / 'runtime/resolution.c').read_text()
    assert 'excludes non-player cameras, pause/map/system menus, active events and first-person aiming' in resolution
    assert 'message log' not in resolution.lower()
    for reason in (
        'camera actor pointer is null', 'camera name is unreadable', 'camera is EventCamera',
        'not PlayerCamera', 'pause, map or system menu is open', 'event manager is unavailable',
        'active event pointer is unreadable', 'an event is active (%#llx)', 'player camera component is unavailable',
        'player camera flags are unreadable', 'first-person aiming flag is set',
        'scaled lens angle is outside (0, pi)',
        'configured scale leaves the angle unchanged',
    ):
        assert f'fov_skip_once(' in source and reason in source

    size = 0x4ea0000
    table = load(ROOT / 'recomp/patches-v111.txt', [('main', FIRST_BASE, size)])
    site = 0x4c52f8
    original = 0xbd413e60
    alts = table.sites['main'][site]
    index, words, expected = next(alt for alt in alts if table.sets[alt[0]][0] == 'fov')
    assert table.sets[index] == ('fov', 'NX2SARIA_FOV_PATCH', 1)
    assert expected == original and words[0] == original
    pc = FIRST_BASE + site
    lifter = Lifter(FIRST_BASE, simd='c')
    call = lifter.lift(words[1], pc)
    assert call.kind == 'bl' and call.target == table.page + 0xeb0
    wide_index, wide_words, wide_expected = next(alt for alt in alts if table.sets[alt[0]][0] == 'wide_aspect')
    assert table.sets[wide_index] == ('wide_aspect', 'NX2NAVI_PATCH_WIDE_ASPECT', 0)
    assert wide_expected == original and wide_words[0] == original
    wide_call = lifter.lift(wide_words[1], pc)
    assert wide_call.kind == 'bl' and wide_call.target == table.page + 0xe40

    class Camera:
        name = 'main'

        def __init__(self):
            self.lifter = lifter

        def lift(self, addr):
            return self.lifter.lift(original if addr == pc else 0xd65f03c0, addr)

    emitter = Emitter(Camera(), {pc}, {pc: [(index, words)]}, reg_locals=0)
    lines, _ = emitter.function(pc, pc + 8)
    patched = next(line for line in lines if f'if (rt_patch_on[{index}])' in line)
    on, off = patched.split(' else ')
    displacement = lifter.lift(original, pc).code
    assert displacement in on and displacement in off
    assert 'rt_dispatch(c)' in on and 'rt_dispatch(c)' not in off
    print('fov_check: FOV and Vert+ share the lens site; both callbacks keep the load and target their page slots')


if __name__ == '__main__':
    main()
