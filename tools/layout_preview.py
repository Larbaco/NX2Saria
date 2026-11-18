"""Draw the game's interface layouts as SVG at a picture shape, to design menu edits without running the game.

    layout_preview.py <archive.blarc | romfs folder> --layout PlusMenu_00 [--layout ...] --aspect 21:9
                      [--aspect 32:9 ...] [--spec spec.json] [--anim [LAYOUT/]NAME:FRAME ...] [--hud] [--png]
                      --out build/wide-menus/preview/

Each pane is a rectangle at its place on the 3840 x 2160 grid (every ancestor's translate, scale and anchor applied),
coloured by kind and labelled with its name; a part pane holds the layout it instantiates, drawn at the part's place and
scale. Panes hidden by their own flag or by --anim are drawn faint. The picture is 2160 * aspect grid units wide around
the grid's centre (3840 at 16:9, 5120 at 21:9, 7680 at 32:9); its outline is solid, the middle 16:9 (x = +-1920)
dashed, and what lies outside the picture is shaded.

--anim applies an animation at a frame (Hermite interpolation of its keys), e.g. EquipToItem:28 to show the pause
menu scrolled to the item page; LAYOUT/ limits it to one layout (a part's among them).

--spec applies edit operations first and draws "before | after" side by side. The spec is JSON,
{"<Layout>": [operation, ...]}, where out = 1080 * aspect - 1920 grid units (0 at 16:9, 640 at 21:9, 1920 at 32:9):

    {"op": "move",   "pane": P, "f": F}     translate x += F * out
    {"op": "grow",   "pane": P, "g": G}     width += 2 * G * out
    {"op": "widen",  "pane": P, "g": G}     scale x *= 1 + G * out / 1920
    {"op": "narrow", "pane": P}             scale x *= 1920 / (1920 + out)
    {"op": "keys", "anim": A, "pane": P, "prop": "translate" | "scale" | "size", ["component": "x" | "y",]
     "map": [{"value": V, "f": F}, ...]}    the track's keys and constants equal to V become V + F * out

Each operation may state the value it expects ("x", "y", "w", "h", "sx", "sy" of the pane): the tool stops with an
error when the layout holds another, as an edit of the game's files would. A spec may edit a part's layout too (the
part is drawn edited in "after").

--hud outlines, over each "after" panel, the layout as the game draws it today (no edit), to compare. Layouts the game
keeps over the whole picture (widescreen.c wide_init's keep list) are drawn stretched to it, as the game does.

Writes <out>/<Layout>_<aspect>.svg for each layout and shape, and <out>/index.html linking them; --png also renders
each SVG to PNG with rsvg-convert or ImageMagick's convert, whichever exists. Standard library only.
"""
import argparse
import html
import json
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import layout_dump as ld  # noqa: E402

# Layouts whose canvas the game keeps over the whole picture (widescreen.c wide_init).
FULL_WIDTH = {'ShortCutBG_00', 'MenuBG_00', 'SystemBG_00', 'EventFade_00', 'LoadingFade_00'}
COLOURS = {'null': '#8a8a8a', 'picture': '#2f6fd6', 'text': '#d9822b', 'alignment': '#8e44ad', 'scissor': '#c0392b',
           'capture': '#1e9e5a', 'part': '#0f9ba8', 'slide list': '#8b5a2b'}
MARGIN = 480      # grid units shown beyond each side of the picture
TITLE = 150       # grid units above the panels for their titles
GAP = 240         # between panels
LABEL = 30        # label size in grid units


def rgba(colour, alpha):
    """A colour with its opacity in one value: renderers that ignore fill-opacity (ImageMagick's own) read rgba()."""
    if colour == 'none':
        return colour
    r, g, b = (int(colour[i:i + 2], 16) for i in (1, 3, 5)) if len(colour) == 7 else \
        (int(colour[i] * 2, 16) for i in (1, 2, 3))
    return f'rgba({r},{g},{b},{alpha:.3f})'


class Panel:
    def __init__(self, x0, view_w, title):
        self.x0, self.view_w, self.title = x0, view_w, title   # x0: the panel's left edge in the SVG
        self.items = []

    def sx(self, x):
        return self.x0 + x + self.view_w / 2

    def sy(self, y):
        return TITLE + MARGIN + 1080 - y


def clip(panel, left, right, bottom, top):
    """The box limited to the panel, or None when none of it is in."""
    half = panel.view_w / 2
    lim_t = 1080 + MARGIN
    l, r = max(left, -half), min(right, half)
    b, t = max(bottom, -lim_t), min(top, lim_t)
    return (l, r, b, t) if l <= r and b <= t else None


def hidden_of(layout, state):
    """{pane index} hidden by its flag (bit 0 clear), a visibility key at 0 or an alpha at 0, or by an ancestor."""
    out = set()
    for p in layout.panes:
        s = state.get(p.index, {})
        vis = s.get((ld.VISIBILITY, 0), 1 if p.flags & 1 else 0)
        alpha = s.get((ld.ALPHA, 0), 1)
        if p.parent in out or vis < 0.5 or alpha <= 0.001:
            out.add(p.index)
    return out


def draw_layout(panel, layout, layouts, anims, at=(0.0, 0.0), scale=(1.0, 1.0), depth=0, max_depth=4,
                hidden=False, mark=frozenset(), outline=None, stretch=1.0, label_depth=1):
    """Add a layout's panes to the panel; part panes recurse into their layouts (to max_depth; panes deeper than
    label_depth go unlabelled). outline: draw only outlines in that colour (the --hud overlay). stretch: the x factor
    of a layout drawn over the whole picture. mark: pane names an edit names, drawn in red at any depth."""
    state = {}
    for lname, aname, frame in anims:
        if lname in (None, layout.name) and any(a.name == aname for a in layout.animations):
            for k, v in ld.animated_state(layout, aname, frame).items():
                state.setdefault(k, {}).update(v)
    boxes = ld.place(layout, state, at, scale)
    hid = hidden_of(layout, state)
    for p, b in zip(layout.panes, boxes):
        if p.parent < 0:
            continue
        faint = hidden or p.index in hid
        kind = p.kind_name
        left, right = b.left * stretch, b.right * stretch
        marked = (layout.name, p.name) in mark
        box = clip(panel, left, right, b.bottom, b.top)
        if box:
            panel.items.append(('box', box, kind, faint, marked, outline, b.rotated, depth))
            if ((kind != 'null' and depth <= label_depth) or marked) and not outline:
                panel.items.append(('label', (left, b.top), p.name, faint, marked))
        if kind == 'part' and depth < max_depth:
            target = layouts.get(layout.parts.get(p.name, ''))
            if target is not None:
                draw_layout(panel, target, layouts, anims, (b.px * stretch, b.py), (b.gsx * stretch, b.gsy),
                            depth + 1, max_depth, faint, mark, outline, 1.0, label_depth)


def frame_items(panel, visible):
    half = visible / 2
    return [('shade', (-panel.view_w / 2, -half)), ('shade', (half, panel.view_w / 2)),
            ('rect', (-half, half, -1080, 1080), '#222', 6, None), ('rect', (-1920, 1920, -1080, 1080), '#222', 4,
                                                                       '40 30')]


def svg_of(panels, visible, heading):
    total_w = panels[-1].x0 + panels[-1].view_w
    total_h = TITLE + 2 * MARGIN + 2160
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {total_w:.0f} {total_h:.0f}" '
           f'width="{total_w / 4:.0f}" height="{total_h / 4:.0f}" font-family="sans-serif">',
           f'<title>{html.escape(heading)}</title>',
           f'<rect x="0" y="0" width="{total_w:.0f}" height="{total_h:.0f}" fill="#ffffff"/>']
    for panel in panels:
        out.append(f'<text x="{panel.x0 + 20:.0f}" y="{TITLE - 50}" font-size="80" fill="#111">'
                   f'{html.escape(panel.title)}</text>')
        top = TITLE
        out.append(f'<rect x="{panel.x0:.0f}" y="{top}" width="{panel.view_w:.0f}" height="{2160 + 2 * MARGIN}" '
                   f'fill="none" stroke="#bbb" stroke-width="3"/>')
        for item in panel.items:
            if item[0] == 'box':
                _, (l, r, b, t), kind, faint, marked, outline, rotated, depth = item
                colour = outline or COLOURS.get(kind, '#000')
                fill = 'none' if outline or kind in ('null', 'part', 'scissor') else colour
                op = (0.25 if faint else 1.0) * (0.6 if depth > 1 else 1.0)
                dash = ' stroke-dasharray="14 10"' if outline or kind in ('part', 'scissor') or rotated else ''
                width = 8 if marked else (3 if kind != 'null' else 1.5)
                stroke = '#e0004d' if marked else colour
                out.append(f'<rect x="{panel.sx(l):.1f}" y="{panel.sy(t):.1f}" width="{r - l:.1f}" '
                           f'height="{t - b:.1f}" fill="{rgba(fill, 0.07 * op)}" stroke="{rgba(stroke, op)}" '
                           f'stroke-width="{width}"{dash}/>')
            elif item[0] == 'label':
                _, (x, y), name, faint, marked = item
                half = panel.view_w / 2
                if not (-half <= x <= half - 100 and -1080 - MARGIN <= y <= 1080 + MARGIN):
                    x = min(max(x, -half), half - 300)
                    if not (-1080 - MARGIN <= y <= 1080 + MARGIN):
                        continue
                colour = '#e0004d' if marked else '#111'
                out.append(f'<text x="{panel.sx(x) + 6:.1f}" y="{panel.sy(y) + LABEL + 2:.1f}" font-size="{LABEL}" '
                           f'fill="{rgba(colour, 0.35 if faint else 0.9)}">{html.escape(name)}</text>')
        for kind, *rest in frame_items(panel, visible):
            if kind == 'shade':
                a, b = rest[0]
                if b > a:
                    out.append(f'<rect x="{panel.sx(a):.1f}" y="{TITLE}" width="{b - a:.1f}" '
                               f'height="{2160 + 2 * MARGIN}" fill="{rgba('#9aa0a8', 0.45)}"/>')
            else:
                (l, r, b, t), colour, width, dash = rest
                d = f' stroke-dasharray="{dash}"' if dash else ''
                out.append(f'<rect x="{panel.sx(l):.1f}" y="{panel.sy(t):.1f}" width="{r - l:.1f}" '
                           f'height="{t - b:.1f}" fill="none" stroke="{colour}" stroke-width="{width}"{d}/>')
    out.append('</svg>')
    return '\n'.join(out) + '\n'


def parse_anim(text):
    try:
        who, frame = text.rsplit(':', 1)
        lname, _, aname = who.rpartition('/')
        return (lname or None, aname, float(frame))
    except ValueError:
        raise SystemExit(f'layout_preview: --anim takes [LAYOUT/]NAME:FRAME, not {text!r}')


def aspect_tag(text):
    return text.replace(':', 'x').replace('/', '_')


def render_png(svg_path):
    exe = shutil.which('rsvg-convert')
    png = svg_path.with_suffix('.png')
    if exe:
        cmd = [exe, '-w', '2400', '-o', str(png), str(svg_path)]
    elif shutil.which('convert'):
        # the SVG's width attribute is a quarter of its grid units; the density gives a PNG about 2400 pixels wide
        width = float(svg_path.read_text().split('width="', 2)[1].split('"')[0])
        cmd = [shutil.which('convert'), '-density', f'{96 * 2400 / width:.0f}', str(svg_path), str(png)]
    else:
        raise SystemExit('layout_preview: --png needs rsvg-convert or ImageMagick convert')
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode or not png.exists():
        raise SystemExit(f'layout_preview: {Path(cmd[0]).name} failed on {svg_path}: '
                         f'{r.stderr.decode(errors="replace").strip()}')
    return png


def build(layouts, names, aspects, spec, anims, hud, out_dir, png=False, max_depth=4, label_depth=1, log=print):
    """Write the SVGs (and PNGs) and index.html; returns [(layout, aspect text, svg path, [edit lines])]."""
    out_dir.mkdir(parents=True, exist_ok=True)
    for lname, aname, _f in anims:
        if lname is not None and lname not in layouts:
            raise ld.SpecError(f'--anim names layout {lname}, which the archive does not hold')
        holders = [n for n in layouts if lname in (None, n) and any(a.name == aname for a in layouts[n].animations)]
        if not holders:
            raise ld.SpecError(f'no layout has an animation {aname}')
    for lname in spec:
        if lname not in layouts:
            raise ld.SpecError(f'the spec edits {lname}, which the archive does not hold')
    results = []
    for aspect_text in aspects:
        aspect = ld.parse_aspect(aspect_text)
        visible, out = 2160 * aspect, ld.out_units(aspect)
        edited, lines = dict(layouts), {}
        for lname, ops in spec.items():
            edited[lname], lines[lname] = ld.apply_ops(layouts[lname], ops, out)
            for line in lines[lname]:
                log(f'{aspect_text} {lname}: {line}')
        view_w = visible + 2 * MARGIN
        for name in names:
            stretch = visible / 3840 if name in FULL_WIDTH else 1.0
            mark = frozenset((lname, op.get('pane')) for lname, ops in spec.items() for op in ops)
            panels = []
            if spec:
                before = Panel(0, view_w, f'{name} {aspect_text}: before (the game today)')
                after = Panel(view_w + GAP, view_w, f'after (out = {out:g} units)')
                common = dict(max_depth=max_depth, label_depth=label_depth, stretch=stretch)
                draw_layout(before, layouts[name], layouts, anims, **common)
                draw_layout(after, edited[name], edited, anims, mark=mark, **common)
                if hud:
                    draw_layout(after, layouts[name], layouts, anims, outline='#e67e22', **common)
                panels = [before, after]
            else:
                one = Panel(0, view_w, f'{name} {aspect_text} (out = {out:g} units)')
                draw_layout(one, layouts[name], layouts, anims, max_depth=max_depth, label_depth=label_depth,
                            stretch=stretch)
                panels = [one]
            heading = f'{name} at {aspect_text}'
            svg = out_dir / f'{name}_{aspect_tag(aspect_text)}.svg'
            svg.write_text(svg_of(panels, visible, heading))
            if png:
                render_png(svg)
            results.append((name, aspect_text, svg, [ln for lname in spec for ln in lines.get(lname, [])]))
    rows = []
    for name in names:
        cells = ''.join(f'<td><a href="{html.escape(svg.name)}">{html.escape(a)}</a></td>'
                        for n, a, svg, _ in results if n == name)
        rows.append(f'<tr><th>{html.escape(name)}</th>{cells}</tr>')
    figs = ''.join(f'<figure><figcaption>{html.escape(n)} at {html.escape(a)}</figcaption>'
                   f'<img src="{html.escape(svg.name)}" alt="{html.escape(n)} at {html.escape(a)}"></figure>'
                   for n, a, svg, _ in results)
    edits = ''.join(f'<li>{html.escape(ln)}</li>' for _n, _a, _s, lines_ in results[:1] for ln in lines_)
    (out_dir / 'index.html').write_text(
        '<!doctype html><meta charset="utf-8"><title>Layout previews</title>'
        '<style>body{font-family:sans-serif;margin:16px;background:#fff;color:#111}img{max-width:100%;'
        'border:1px solid #ccc}figure{margin:24px 0}td,th{padding:4px 10px;text-align:left}</style>'
        f'<h1>Layout previews</h1><table>{"".join(rows)}</table>'
        + (f'<h2>Edits (first shape)</h2><ul>{edits}</ul>' if edits else '') + figs + '\n')
    return results


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('source', type=Path, help='a decompressed layout archive (.blarc) or a RomFS folder')
    ap.add_argument('--layout', action='append', required=True, help='a layout to draw (repeatable)')
    ap.add_argument('--aspect', action='append', help='picture shape, as 21:9, 3440x1440 or 2.39 (repeatable; '
                                                      'default 16:9, 21:9 and 32:9)')
    ap.add_argument('--spec', type=Path, help='JSON edit operations, drawn before | after')
    ap.add_argument('--anim', action='append', default=[], help='[LAYOUT/]NAME:FRAME (repeatable)')
    ap.add_argument('--hud', action='store_true', help="outline the layout as the game draws it today over 'after'")
    ap.add_argument('--png', action='store_true', help='also render each SVG to PNG')
    ap.add_argument('--depth', type=int, default=4, help='how deep parts are drawn (default 4)')
    ap.add_argument('--label-depth', type=int, default=1, help='label panes down to this part depth (default 1)')
    ap.add_argument('--out', type=Path, default=ld.ROOT / 'build' / 'wide-menus' / 'preview')
    args = ap.parse_args(argv)
    try:
        layouts = ld.load(args.source)
        missing = [n for n in args.layout if n not in layouts]
        if missing:
            raise ld.SpecError(f'no layout {", ".join(missing)}')
        spec = json.loads(args.spec.read_text()) if args.spec else {}
        if not isinstance(spec, dict) or not all(isinstance(v, list) for v in spec.values()):
            raise ld.SpecError('the spec is {"<Layout>": [operation, ...]}')
        results = build(layouts, args.layout, args.aspect or ['16:9', '21:9', '32:9'], spec,
                        [parse_anim(a) for a in args.anim], args.hud, args.out, args.png, args.depth,
                        args.label_depth)
    except (ld.FormatError, ld.SpecError, OSError, ValueError, KeyError) as e:
        sys.exit(f'layout_preview: {e}')
    for name, aspect, svg, _ in results:
        print(f'{name} {aspect}: {svg}')
    print(f'index: {args.out / "index.html"}')


if __name__ == '__main__':
    main()
