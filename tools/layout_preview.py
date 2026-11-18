"""Draw the game's interface layouts as SVG boxes or textured PNGs for one or more picture shapes.

    layout_preview.py <archive.blarc | romfs folder> --layout PlusMenu_00 [--layout ...] --aspect 21:9
                      [--aspect 32:9 ...] [--spec spec.json] [--anim [LAYOUT[/PART...]/]NAME:FRAME ...] [--hud] [--png]
                      --out build/wide-menus/preview/

Each pane is a rectangle at its place on the 3840 x 2160 grid (every ancestor's translate, scale and anchor applied),
coloured by kind and labelled with its name; a part pane holds the layout it instantiates, drawn at the part's place and
scale. Panes hidden by their own flag or by --anim are drawn faint. The picture is 2160 * aspect grid units wide around
the grid's centre (3840 at 16:9, 5120 at 21:9, 7680 at 32:9); its outline is solid, the middle 16:9 (x = +-1920)
dashed, and what lies outside the picture is shaded.

--anim applies an animation at a frame (Hermite interpolation of its keys), e.g. EquipToItem:28 to show the pause
menu scrolled to the item page. LAYOUT/ limits it to one layout. LAYOUT/PART/.../ addresses one part instance.

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
each SVG to PNG with rsvg-convert or ImageMagick's convert, whichever exists. SVG uses the standard library.

--stack A,B draws the named layouts together, from back to front. --textures draws directly to an RGBA PNG using
the textures from the RomFS folder (--romfs is required if the layout source is an archive). Each panel is clipped
to the picture. --scale gives the fraction of the 2160-pixel picture height (default 0.25), and --boxes adds pane
outlines and, with Pillow, labels. Text is shown as faint outlines. Pictures use the first available static texture
of their material, including single-texture window fills. Multi-texture windows and live captures are omitted.
Custom material effects and live layout state are approximated by the stored textures and animation tracks.
Use --dump-texture LAYOUT/PANE for a pane's source texture at its native size.

Texture mode needs NumPy and zstd. Pillow is optional. For uncached ASTC textures it builds the standalone CPU
decoder with a C compiler and the runtime headers. Set NX2SARIA_VMA_INCLUDE if vk_mem_alloc.h is outside the
usual toolchain include paths. Textured output and the decoded texture cache stay under build/.
"""
import argparse
import html
import json
import math
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


def state_for(layout, anims, instance, initial=(), *, material=False):
    state = {}
    available = {a.name for a in layout.animations}
    frames = [*initial, *((aname, frame) for scope, aname, frame in anims
                          if scope in (None, layout.name, instance))]
    for aname, frame in frames:
        if aname in available:
            for k, v in ld.animated_state(layout, aname, frame, material=material).items():
                state.setdefault(k, {}).update(v)
    return state


def draw_layout(panel, layout, layouts, anims, at=(0.0, 0.0), scale=(1.0, 1.0), depth=0, max_depth=4,
                hidden=False, mark=frozenset(), outline=None, stretch=1.0, label_depth=1, instance=None):
    """Add a layout's panes to the panel; part panes recurse into their layouts (to max_depth; panes deeper than
    label_depth go unlabelled). outline: draw only outlines in that colour (the --hud overlay). stretch: the x factor
    of a layout drawn over the whole picture. mark: pane names an edit names, drawn in red at any depth."""
    instance = instance or layout.name
    state = state_for(layout, anims, instance)
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
                            depth + 1, max_depth, faint, mark, outline, 1.0, label_depth, f'{instance}/{p.name}')


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
        frame = float(frame)
        if not math.isfinite(frame):
            raise ValueError
        return (lname or None, aname, frame)
    except ValueError:
        raise SystemExit(f'layout_preview: --anim takes [LAYOUT[/PART...]/]NAME:FRAME, not {text!r}')


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


def textured_png(path, group, layouts, edited, visible, scale, store, anims, box_overlay, marks, hud,
                 max_depth, label_depth):
    import layout_textures as lt

    def state(layout, instance, initial, *, material=False):
        return state_for(layout, anims, instance, initial, material=material)

    panels = []
    for level in (layouts, edited) if edited is not None else (layouts,):
        panel = lt.Renderer(visible, scale, store, COLOURS)
        for name in group:
            stretch = visible / 3840 if name in FULL_WIDTH else 1.0
            panel.draw(level[name], level, state, stretch, max_depth, label_depth, box_overlay,
                       marks if level is edited else frozenset())
        if hud and level is edited:
            for name in group:
                stretch = visible / 3840 if name in FULL_WIDTH else 1.0
                panel.draw(layouts[name], layouts, state, stretch, max_depth, label_depth, hud=True)
        panels.append(panel.finish())
    lt.save_panels(path, panels, round(GAP * scale))


def build(layouts, names, aspects, spec, anims, hud, out_dir, png=False, max_depth=4, label_depth=1, log=print,
          *, stacks=(), texture_store=None, output_scale=0.25, boxes=False):
    """Write previews and index.html; returns [(layout or stack, aspect text, image path, edit lines)]."""
    out_dir.mkdir(parents=True, exist_ok=True)
    for scope, aname, _f in anims:
        candidates = layouts.values()
        if scope is not None:
            root, *parts = scope.split('/')
            target = layouts.get(root)
            for part in parts:
                target = layouts.get(target.parts.get(part)) if target is not None else None
            if target is None:
                raise ld.SpecError(f'--anim names unknown layout or part {scope}')
            candidates = (target,)
        if not any(a.name == aname for layout in candidates for a in layout.animations):
            raise ld.SpecError(f'no layout has an animation {aname}')
    for lname in spec:
        if lname not in layouts:
            raise ld.SpecError(f'the spec edits {lname}, which the archive does not hold')
    groups = {name: (name,) for name in names}
    groups.update(('+'.join(stack), stack) for stack in stacks)
    mark = frozenset((lname, op.get('pane')) for lname, ops in spec.items() for op in ops)
    results = []
    for aspect_text in aspects:
        try:
            aspect = ld.parse_aspect(aspect_text)
        except (ValueError, ZeroDivisionError) as e:
            raise ld.SpecError(f'invalid picture shape {aspect_text!r}') from e
        if not math.isfinite(aspect) or aspect <= 0:
            raise ld.SpecError(f'invalid picture shape {aspect_text!r}')
        visible, out = 2160 * aspect, ld.out_units(aspect)
        edited, lines = dict(layouts), {}
        for lname, ops in spec.items():
            edited[lname], lines[lname] = ld.apply_ops(layouts[lname], ops, out)
            for line in lines[lname]:
                log(f'{aspect_text} {lname}: {line}')
        view_w = visible + 2 * MARGIN
        for name, group in groups.items():
            basename = f'{name}_{aspect_tag(aspect_text)}'
            edit_lines = [ln for lname in spec for ln in lines[lname]]
            if texture_store is not None:
                path = out_dir / f'{basename}.png'
                textured_png(path, group, layouts, edited if spec else None, visible, output_scale, texture_store,
                             anims, boxes, mark, hud, max_depth, label_depth)
                results.append((name, aspect_text, path, edit_lines))
                continue
            if spec:
                before = Panel(0, view_w, f'{name} {aspect_text}: before (the game today)')
                after = Panel(view_w + GAP, view_w, f'after (out = {out:g} units)')
                panels = [before, after]
            else:
                panels = [Panel(0, view_w, f'{name} {aspect_text} (out = {out:g} units)')]
            for lname in group:
                stretch = visible / 3840 if lname in FULL_WIDTH else 1.0
                common = dict(max_depth=max_depth, label_depth=label_depth, stretch=stretch)
                draw_layout(panels[0], layouts[lname], layouts, anims, **common)
                if spec:
                    draw_layout(panels[1], edited[lname], edited, anims, mark=mark, **common)
                    if hud:
                        draw_layout(panels[1], layouts[lname], layouts, anims, outline='#e67e22', **common)
            heading = f'{name} at {aspect_text}'
            svg = out_dir / f'{basename}.svg'
            svg.write_text(svg_of(panels, visible, heading))
            if png:
                render_png(svg)
            results.append((name, aspect_text, svg, edit_lines))
    rows = []
    for name in groups:
        cells = ''.join(f'<td><a href="{html.escape(svg.name)}">{html.escape(a)}</a></td>'
                        for n, a, svg, _ in results if n == name)
        rows.append(f'<tr><th>{html.escape(name)}</th>{cells}</tr>')
    comparison = ' · Before | after' if texture_store is not None and spec else ''
    figs = ''.join(f'<figure><figcaption>{html.escape(n)} at {html.escape(a)}{comparison}</figcaption>'
                   f'<img src="{html.escape(svg.name)}" alt="{html.escape(n)} at {html.escape(a)}"></figure>'
                   for n, a, svg, _ in results)
    edits = ''.join(f'<li>{html.escape(ln)}</li>' for _n, _a, _s, lines_ in results[:1] for ln in lines_)
    matte = 'background:#0d0c0b;' if texture_store is not None else ''
    (out_dir / 'index.html').write_text(
        '<!doctype html><meta charset="utf-8"><title>Layout previews</title>'
        '<style>body{font-family:sans-serif;margin:16px;background:#fff;color:#111}img{max-width:100%;'
        f'{matte}border:1px solid #ccc}}figure{{margin:24px 0}}td,th{{padding:4px 10px;text-align:left}}</style>'
        f'<h1>Layout previews</h1><table>{"".join(rows)}</table>'
        + (f'<h2>Edits (first shape)</h2><ul>{edits}</ul>' if edits else '') + figs + '\n')
    return results


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('source', type=Path, help='a decompressed layout archive (.blarc) or a RomFS folder')
    ap.add_argument('--layout', action='append', default=[], help='a layout to draw (repeatable)')
    ap.add_argument('--stack', action='append', default=[], help='comma-separated layouts, back to front (repeatable)')
    ap.add_argument('--aspect', action='append', help='picture shape, as 21:9, 3440x1440 or 2.39 (repeatable; '
                                                      'default 16:9, 21:9 and 32:9)')
    ap.add_argument('--spec', type=Path, help='JSON edit operations, drawn before | after')
    ap.add_argument('--anim', action='append', default=[], help='[LAYOUT[/PART...]/]NAME:FRAME (repeatable)')
    ap.add_argument('--hud', action='store_true', help="outline the layout as the game draws it today over 'after'")
    ap.add_argument('--png', action='store_true', help='also render each SVG to PNG')
    ap.add_argument('--textures', action='store_true', help='draw static picture textures directly to PNG')
    ap.add_argument('--romfs', type=Path, help='RomFS folder for textures when the source is a layout archive')
    ap.add_argument('--scale', type=float, default=0.25, help='textured PNG resolution relative to 2160p')
    ap.add_argument('--boxes', action='store_true', help='overlay pane boxes on the textured PNG')
    ap.add_argument('--dump-texture', metavar='LAYOUT/PANE', help="write the pane's source texture as a standalone PNG")
    ap.add_argument('--depth', type=int, default=4, help='how deep parts are drawn (default 4)')
    ap.add_argument('--label-depth', type=int, default=1, help='label panes down to this part depth (default 1)')
    ap.add_argument('--out', type=Path, help='output folder under build/ for textured PNGs')
    args = ap.parse_args(argv)
    stacks = [tuple(name.strip() for name in stack.split(',')) for stack in args.stack]
    if not (args.layout or stacks or args.dump_texture):
        ap.error('provide --layout, --stack or --dump-texture')
    if args.depth < 0:
        ap.error('--depth must be nonnegative')
    texture_mode = args.textures or args.dump_texture
    out = args.out or ld.ROOT / 'build' / 'wide-menus' / ('textured' if texture_mode else 'preview')
    try:
        store = None
        if texture_mode:
            if not math.isfinite(args.scale) or args.scale <= 0:
                raise ld.SpecError('--scale must be a finite positive number')
            if not out.resolve().is_relative_to((ld.ROOT / 'build').resolve()):
                raise ld.SpecError('textured output must stay under build/')
            romfs = args.romfs or (args.source if args.source.is_dir() else None)
            if romfs is None:
                raise ld.SpecError('pass --romfs ROMFS when the source is a .blarc')
            try:
                import layout_textures as lt
            except ModuleNotFoundError as e:
                if e.name != 'numpy':
                    raise
                raise ld.SpecError('texture mode needs a Python environment with NumPy') from e
            store = lt.TextureStore(romfs)
        layouts = ld.load(args.source)
        missing = [n for group in [args.layout, *stacks] for n in group if n not in layouts]
        if missing:
            raise ld.SpecError(f'no layout {", ".join(missing)}')
        if args.dump_texture:
            name, sep, pane = args.dump_texture.partition('/')
            if not sep or name not in layouts or not pane:
                raise ld.SpecError('--dump-texture needs LAYOUT/PANE from the layout archive')
            out.mkdir(parents=True, exist_ok=True)
            path, texture = lt.dump_texture(layouts[name], pane, store, out)
            print(f'{name}/{pane} ({texture}): {path}')
            if not (args.layout or stacks):
                return
        spec = json.loads(args.spec.read_text()) if args.spec else {}
        if not isinstance(spec, dict) or not all(isinstance(v, list) for v in spec.values()):
            raise ld.SpecError('the spec is {"<Layout>": [operation, ...]}')
        results = build(layouts, args.layout, args.aspect or ['16:9', '21:9', '32:9'], spec,
                        [parse_anim(a) for a in args.anim], args.hud, out, args.png, args.depth,
                        args.label_depth, stacks=stacks, texture_store=store, output_scale=args.scale, boxes=args.boxes)
    except (ld.FormatError, ld.SpecError, OSError, ValueError, KeyError) as e:
        sys.exit(f'layout_preview: {e}')
    for name, aspect, path, _ in results:
        print(f'{name} {aspect}: {path}')
    print(f'index: {out / "index.html"}')


if __name__ == '__main__':
    main()
