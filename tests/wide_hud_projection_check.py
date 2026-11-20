"""Check the full-height exceptions in the 1.1.1 wide_hud projection patch."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'recomp'))
from layout import FIRST_BASE  # noqa: E402
from patches import load  # noqa: E402


def main():
    table = load(ROOT / 'recomp/patches-v111.txt', [('main', FIRST_BASE, 0x4ea0000)])
    index = next(i for i, row in enumerate(table.sets) if row[0] == 'wide_hud')
    site = table.sites['main'][0x774368]
    words, expected = next((words, expected) for set_index, words, expected in site if set_index == index)
    assert expected == 0xad401041

    # Projection match builds v17. The screen lookup then removes the four full-picture vtables from that mask,
    # duplicates it across lanes, and only then may the Y row use the vertical squeeze factor.
    projection_mask = words.index(0x4e040631)
    full_screen_exclusion = words.index(0x0e721e31)
    vertical_scale = words.index(0x3dc02233)
    vertical_multiply = words.index(0x6e33dc93)
    vertical_select = words.index(0x6eb11e64)
    horizontal_scale = words.index(0x3dc02e33)
    assert projection_mask < full_screen_exclusion < vertical_scale < vertical_multiply < vertical_select < horizontal_scale
    assert words[vertical_scale:vertical_select + 1] == [0x3dc02233, 0x6e33dc93, 0x6eb11e64]

    # After `ldr d16, [x16]` (0xfd400210) x16 holds the canvas or its screen, no longer the patch page: every 128-bit
    # load of a page value from there on reads through x17, which the site sets to the page again. Reading the vertical
    # squeeze through x16 (0.20.0-rc.1) took a word of the screen object and hid the whole HUD.
    repurposed = words.index(0xfd400210)
    for w in words[repurposed + 1:]:
        if isinstance(w, int) and w & 0xffc00000 == 0x3dc00000:   # LDR (immediate, unsigned offset), 128-bit
            assert (w >> 5) & 31 == 17, f'{w:08x} reads the page through x{(w >> 5) & 31}, not x17'

    # 16:9 and wider ratios supply 1.0 in page +128, so the extra Y path leaves those screens unchanged.
    source = (ROOT / 'runtime/widescreen.c').read_text()
    assert 'memcpy(page + 128, vertical4, 16);' in source
    assert 'float y_squeeze = (float)wide_ui_vertical_scale(ratio);' in source
    assert 'float movie_margin_y = taller ? 0.0f : margin_y;' in source
    assert 'if ((center && wider) || taller) wide_bars_init(margin_x, taller ? 0.0 : margin_y);' in source
    print('wide_hud_projection_check: ordinary UI compresses vertically; all four full-picture vtables keep full height')


if __name__ == '__main__':
    main()
