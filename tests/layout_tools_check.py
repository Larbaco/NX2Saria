"""Check tools/layout_dump.py and tools/layout_preview.py on a layout archive the test makes itself. No game files.

The archive is a SARC with two BFFH layouts: Main (a group pane that is scaled and moved, a picture in it, a text with a
left origin anchored to its parent's right edge, a part that instantiates Sub) and Sub (one picture). Main has one
animation with a curve of the group's translate x and a constant of the picture's scale x. The checks: the tables,
panes, parts and tracks read back; the panes' extents on the grid; the curve's value between keys; the dump's text,
JSON and --find-animated output; each edit operation of a spec and the span of a curve whose keys move; a spec whose
expected value differs is refused and changes nothing; the preview's SVG files and index.

Usage: python3 -I tests/layout_tools_check.py
"""
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import layout_dump as ld  # noqa: E402
import layout_preview as lp  # noqa: E402

FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print(f'FAIL: {what}')


# ---- a BFFH writer: the records the reader follows, at offsets it finds through the header
class Bffh:
    def __init__(self):
        self.b = bytearray(b'BFFH' + bytes(0x300 - 4))
        struct.pack_into('<ff', self.b, ld.CANVAS, 3840.0, 2160.0)

    def put(self, data, align=8):
        while len(self.b) % align:
            self.b.append(0)
        at = len(self.b)
        self.b += data
        return at

    def string(self, s):
        return self.put(s.encode() + b'\0', 1)

    def table(self, slot, entries):
        names = self.put(b''.join(struct.pack('<Q', self.string(n)) for n, _ in entries) or bytes(8))
        records = self.put(b''.join(struct.pack('<Q', r) for _, r in entries) or bytes(8))
        struct.pack_into('<QQQI', self.b, slot, names, records, 0, len(entries))

    def pane(self, x, y, w, h, parent, kind, sx=1.0, sy=1.0, origin=(2, 5), anchor=(2, 5), visible=True):
        rec = bytearray(ld.PANE_SIZE)
        flags = (1 if visible else 0) | kind << 7 | origin[0] << 11 | origin[1] << 14 | anchor[0] << 17 | \
            anchor[1] << 20
        struct.pack_into('<fff', rec, ld.PANE_SCALE_X, sx, sy, 0.0)
        struct.pack_into('<ff', rec, ld.PANE_X, x, y)
        struct.pack_into('<iff', rec, ld.PANE_PARENT, parent, w, h)
        struct.pack_into('<II', rec, ld.PANE_END, 0, flags)
        return self.put(bytes(rec))

    def part(self, layout, pane_index):
        rec = bytearray(0x48)
        struct.pack_into('<Q', rec, 0, self.string(layout))
        struct.pack_into('<I', rec, 0x40, pane_index)
        return self.put(bytes(rec))

    def code(self, pane, group, prop):
        v = 0x7f | 7 << 8 | (pane & 0xf) << 11 | (pane >> 4) << 16 | group << 22 | prop << 24
        return self.put(v.to_bytes(6, 'little'), 1)

    def curve(self, keys):
        """keys: [(frame, value, slope)]"""
        frames = self.put(b''.join(struct.pack('<f', f) for f, _, _ in keys))
        floats = self.put(b''.join(struct.pack('<ff', v, s) for _, v, s in keys))
        fr = self.put(struct.pack('<ffQII', keys[0][0], keys[-1][0], frames, len(keys), 4))
        va = self.put(struct.pack('<fIQII', keys[-1][1] - keys[0][1], 0, floats, 2 * len(keys), 4))
        return self.put(struct.pack('<QQQ', fr, va, 0))

    def animation(self, name, length, tracks):
        """tracks: [(pane, group, prop, [(component, keys)], [(component, value)])]"""
        curves, ccomp, consts, kcomp, entries = [], [], [], [], []
        for pane, group, prop, tcurves, tconsts in tracks:
            first = len(curves) if tcurves else 0xffff
            cfirst = len(consts) if tconsts else 0xffff
            for comp, keys in tcurves:
                curves.append(self.curve(keys))
                ccomp.append(comp)
            for comp, value in tconsts:
                consts.append(value)
                kcomp.append(comp)
            entries.append(self.put(struct.pack('<QHHBB', self.code(pane, group, prop), first, cfirst,
                                                len(tcurves), len(tconsts)) + bytes(2)))
        lists = [(self.put(b''.join(struct.pack('<Q', e) for e in entries)), len(entries)),
                 (self.put(b''.join(struct.pack('<Q', c) for c in curves) or bytes(8)), len(curves)),
                 (self.put(bytes(ccomp) or bytes(1)), len(ccomp)),
                 (self.put(b''.join(struct.pack('<f', v) for v in consts) or bytes(4)), len(consts)),
                 (self.put(bytes(kcomp) or bytes(1)), len(kcomp))]
        body = bytearray(0x60)
        struct.pack_into('<Q', body, 0, self.string(name))
        for (at, n), off in zip(lists, (ld.BODY_TRACKS, ld.BODY_CURVES, ld.BODY_CURVE_COMPONENTS, ld.BODY_CONSTANTS,
                                        ld.BODY_CONSTANT_COMPONENTS)):
            struct.pack_into('<QI', body, off, at, n)
        body_at = self.put(bytes(body))
        rec = bytearray(0x38)
        struct.pack_into('<Q', rec, 0, body_at)
        struct.pack_into('<IIQ', rec, ld.ANIM_LENGTH, length, 0, self.string(name))
        return self.put(bytes(rec))


def main_layout():
    f = Bffh()
    panes = [('RootPane', f.pane(0, 0, 0, 0, -1, 0)),
             ('N_Group_00', f.pane(100, 50, 200, 100, 0, 0, sx=2.0, sy=2.0)),
             ('P_Pic_00', f.pane(50, 0, 40, 20, 1, 1)),
             ('T_Left_00', f.pane(10, 0, 30, 10, 1, 2, origin=(1, 5), anchor=(3, 5))),
             ('Pa_Sub_00', f.pane(-1000, 0, 200, 200, 0, 6, sx=0.5, sy=0.5)),
             ('P_Hidden_00', f.pane(0, -500, 100, 100, 0, 1, visible=False))]
    f.table(ld.TABLES['panes'], panes)
    f.table(ld.TABLES['parts'], [('Pa_Sub_00', f.part('Sub', 4))])
    slide = f.animation('Slide', 10, [(1, 1, 1, [(0, [(0, 100, 0), (10, 300, 0)])], [(4, 50.0)]),
                                      (2, 1, 0, [], [(0, 2.0)]),
                                      (5, 1, 2, [(0, [(0, 0, 0), (5, 0, 0), (5, 1, 0)])], [])])
    f.table(ld.TABLES['animations'], [('Slide', slide)])
    for name, slot in ld.TABLES.items():
        if name not in ('panes', 'parts', 'animations'):
            f.table(slot, [])
    return bytes(f.b)


def sub_layout():
    f = Bffh()
    f.table(ld.TABLES['panes'], [('RootPane', f.pane(0, 0, 0, 0, -1, 0)), ('P_Inner_00', f.pane(100, 0, 60, 60, 0, 1))])
    for name, slot in ld.TABLES.items():
        if name != 'panes':
            f.table(slot, [])
    return bytes(f.b)


def sarc(members):
    names = b''
    offs = []
    for n, _ in members:
        offs.append(len(names))
        names += n.encode() + b'\0'
        while len(names) % 4:
            names += b'\0'
    head = 0x14 + 0xc + 16 * len(members) + 8 + len(names)
    start = (head + 0xff) & ~0xff
    data, entries = b'', b''
    for (n, body), o in zip(members, offs):
        while len(data) % 8:
            data += b'\0'
        entries += struct.pack('<IIII', 0, 0x01000000 | o // 4, len(data), len(data) + len(body))
        data += body
    out = (b'SARC' + struct.pack('<HHIIHH', 0x14, 0xfeff, start + len(data), start, 0x100, 0) +
           b'SFAT' + struct.pack('<HHI', 0xc, len(members), 0x65) + entries + b'SFNT' + struct.pack('<HH', 8, 0) +
           names)
    return out + bytes(start - len(out)) + data


def run(*args):
    return subprocess.run([sys.executable, '-I', *map(str, args)], capture_output=True, text=True)


def main():
    with tempfile.TemporaryDirectory(prefix='layout-tools-check-') as tmp:
        tmp = Path(tmp)
        archive = tmp / 'Test.blarc'
        archive.write_bytes(sarc([('Main.buil', main_layout()), ('Sub.buil', sub_layout()),
                                  ('Main.buil', main_layout())]))
        layouts = ld.load(archive)
        check(sorted(layouts) == ['Main', 'Sub'], f'layouts {sorted(layouts)}')
        main_l = layouts['Main']
        check(main_l.copies == 2, f'copies {main_l.copies}')
        check(main_l.canvas == (3840.0, 2160.0), 'canvas')
        check([p.kind_name for p in main_l.panes] == ['null', 'null', 'picture', 'text', 'part', 'picture'], 'kinds')
        check(main_l.parts == {'Pa_Sub_00': 'Sub'}, f'parts {main_l.parts}')
        check(main_l.panes[1].children == [2, 3], 'children')

        # extents on the grid: the group at x 100 scaled 2; the picture 50 in it, 40 wide; the text anchored to the
        # group's right edge (100 in its space) with a left origin
        boxes = ld.place(main_l)
        check(abs(boxes[2].left - 160) < 1e-3 and abs(boxes[2].right - 240) < 1e-3,
              f'picture extent {boxes[2].left} .. {boxes[2].right}')
        check(abs(boxes[3].left - (100 + 2 * (100 + 10))) < 1e-3 and abs(boxes[3].right - 380) < 1e-3,
              f'text extent {boxes[3].left} .. {boxes[3].right}')
        check(abs(boxes[1].bottom - (50 - 100)) < 1e-3 and abs(boxes[1].top - 150) < 1e-3, 'group height')

        # tracks
        slide = main_l.animations[0]
        check(slide.name == 'Slide' and slide.length == 10 and len(slide.tracks) == 3, 'animation')
        t0 = slide.tracks[0]
        check(t0.target == ld.TARGET_PANE and t0.pane == 1 and t0.prop_name == 'translate', f'track 0 {t0}')
        check([(c.component, c.frames, [v for v, _ in c.values]) for c in t0.curves] == [(0, [0, 10], [100, 300])],
              'curve keys')
        check([(k.component, k.value) for k in t0.constants] == [(4, 50.0)], 'constant')
        check(slide.tracks[1].prop_name == 'scale' and slide.tracks[2].prop_name == 'visibility', 'properties')
        check(abs(ld.evaluate(t0.curves[0], 5) - 200) < 1e-3, 'curve at frame 5')
        check(ld.evaluate(slide.tracks[2].curves[0], 5) == 1, 'a step takes the later key on its frame')
        state = ld.animated_state(main_l, 'Slide', 10)
        check(state[1][(ld.TRANSLATE, 0)] == 300 and state[2][(ld.SCALE, 0)] == 2.0, 'animated state')
        moved = ld.place(main_l, state)
        check(abs(moved[2].left - (300 + 100 - 80)) < 1e-3, f'animated picture at {moved[2].left}')
        found = ld.find_animated(main_l)
        check(('N_Group_00', 'translate x', ['Slide (curve)']) in found and
              ('P_Pic_00', 'scale x', ['Slide (constant)']) in found and len(found) == 2, f'find-animated {found}')

        # the command line
        r = run(ROOT / 'tools' / 'layout_dump.py', archive, '--layout', 'Main')
        check(r.returncode == 0 and 'Pa_Sub_00 -> Sub' in r.stdout and 'N_Group_00 translate: x [0:100 10:300]; y =50'
              in r.stdout and '| 160 .. 240' in r.stdout, f'dump text:\n{r.stdout}{r.stderr}')
        r = run(ROOT / 'tools' / 'layout_dump.py', archive, '--json')
        try:
            js = json.loads(r.stdout)
            check([j['name'] for j in js] == ['Main', 'Sub'] and js[0]['animations'][0]['tracks'][1]['constants'] ==
                  [{'component': 'x', 'value': 2.0}], 'dump json')
        except json.JSONDecodeError:
            check(False, f'dump json does not parse: {r.stderr}')
        r = run(ROOT / 'tools' / 'layout_dump.py', archive, '--find-animated')
        check(r.stdout.splitlines() == ['Main N_Group_00 translate x: Slide (curve)',
                                        'Main P_Pic_00 scale x: Slide (constant)'], f'find-animated:\n{r.stdout}')
        r = run(ROOT / 'tools' / 'layout_dump.py', archive, '--layout', 'Nope')
        check(r.returncode != 0 and 'no layout Nope' in r.stderr, 'a missing layout is an error')

        # edits at 21:9 (out 640)
        out = ld.out_units(ld.parse_aspect('21:9'))
        check(abs(out - 640) < 1e-3 and ld.out_units(ld.parse_aspect('16:9')) == 0 and
              abs(ld.out_units(ld.parse_aspect('32:9')) - 1920) < 1e-3, f'out {out}')
        before = bytes(main_l.data)
        ops = [{'op': 'move', 'pane': 'P_Pic_00', 'f': 1, 'x': 50},
               {'op': 'grow', 'pane': 'P_Pic_00', 'g': 0.5, 'w': 40},
               {'op': 'widen', 'pane': 'N_Group_00', 'g': 1, 'sx': 2},
               {'op': 'narrow', 'pane': 'T_Left_00'},
               {'op': 'keys', 'anim': 'Slide', 'pane': 'N_Group_00', 'prop': 'translate',
                'map': [{'value': 300, 'f': 1}]},
               {'op': 'keys', 'anim': 'Slide', 'pane': 'P_Pic_00', 'prop': 'scale', 'map': [{'value': 2, 'f': 0.5}]}]
        edited, lines = ld.apply_ops(main_l, ops, out)
        p = edited.by_name
        check(abs(p['P_Pic_00'].x - 690) < 1e-3, 'move')
        check(abs(p['P_Pic_00'].w - 680) < 1e-3, 'grow')
        check(abs(p['N_Group_00'].sx - 2 * (1 + 640 / 1920)) < 1e-4, 'widen')
        check(abs(p['T_Left_00'].sx - 1920 / 2560) < 1e-4, 'narrow')
        c = edited.animations[0].tracks[0].curves[0]
        check([v for v, _ in c.values] == [100, 940] and abs(c.span - 840) < 1e-3, f'keys and span {c.values} {c.span}')
        check(edited.animations[0].tracks[0].constants[0].value == 50.0, 'y constant kept')
        check(edited.animations[0].tracks[1].constants[0].value == 322.0, 'constant key')
        check(len(lines) == 6 and lines[0] == 'P_Pic_00 x 50 -> 690', f'lines {lines}')
        check(bytes(main_l.data) == before, 'the edit leaves the original untouched')
        changed = [i for i in range(len(before)) if before[i] != edited.data[i]]
        check(len(changed) <= 6 * 4 + 4 + 4, f'{len(changed)} bytes changed')

        for bad, why in (([{'op': 'move', 'pane': 'P_Pic_00', 'f': 1, 'x': 51}], 'x is 50'),
                         ([{'op': 'grow', 'pane': 'P_Pic_00', 'g': 1, 'w': 41}], 'w is 40'),
                         ([{'op': 'move', 'pane': 'P_Nope_00', 'f': 1}], 'no pane'),
                         ([{'op': 'keys', 'anim': 'Slide', 'pane': 'N_Group_00', 'prop': 'translate',
                            'map': [{'value': 299, 'f': 1}]}], 'no key holds 299'),
                         ([{'op': 'keys', 'anim': 'Other', 'pane': 'N_Group_00', 'prop': 'translate', 'map': []}],
                          'has no translate track'),
                         ([{'op': 'spin', 'pane': 'P_Pic_00'}], 'unknown op')):
            try:
                ld.apply_ops(main_l, bad, out)
                check(False, f'refused spec {bad} was applied')
            except ld.SpecError as e:
                check(why in str(e), f'refusal says {e}')
        check(bytes(main_l.data) == before, 'a refused spec changes nothing')

        # the preview: before | after SVGs, the index, a refused spec stops it
        spec = tmp / 'spec.json'
        spec.write_text(json.dumps({'Main': ops[:2], 'Sub': [{'op': 'move', 'pane': 'P_Inner_00', 'f': -1,
                                                               'x': 100}]}))
        outdir = tmp / 'preview'
        r = run(ROOT / 'tools' / 'layout_preview.py', archive, '--layout', 'Main', '--aspect', '16:9', '--aspect',
                '21:9', '--spec', spec, '--anim', 'Slide:5', '--hud', '--out', outdir)
        svgs = sorted(x.name for x in outdir.glob('*.svg')) if outdir.exists() else []
        check(r.returncode == 0 and svgs == ['Main_16x9.svg', 'Main_21x9.svg'], f'preview: {svgs}\n{r.stderr}')
        if svgs:
            text = (outdir / 'Main_21x9.svg').read_text()
            for want, n in (('>P_Pic_00</text>', 2), ('>P_Inner_00</text>', 2), ('before (the game today)', 1),
                            ('after (out = 640 units)', 1), ('rgba(224,0,77,', 2), ('rgba(230,126,34,', 4)):
                check(text.count(want) >= n, f'preview holds {want} {n} times, not {text.count(want)}')
            check('Main_21x9.svg' in (outdir / 'index.html').read_text(), 'index links the SVGs')
            check('21:9 Sub: P_Inner_00 x 100 -> -540' in r.stdout, f'preview reports the edit:\n{r.stdout}')
        spec.write_text(json.dumps({'Main': [{'op': 'move', 'pane': 'P_Pic_00', 'f': 1, 'x': 49}]}))
        r = run(ROOT / 'tools' / 'layout_preview.py', archive, '--layout', 'Main', '--aspect', '21:9', '--spec', spec,
                '--out', tmp / 'refused')
        check(r.returncode != 0 and 'x is 50, the spec expects 49' in r.stderr and
              not (tmp / 'refused' / 'Main_21x9.svg').exists(), f'refused preview: {r.stderr}')

        # a damaged file is a format error, not a crash
        data = main_layout()
        for cut in (8, 0x120, 0x2fc, len(data) // 2, len(data) * 3 // 4):
            try:
                ld.Layout('Cut', bytearray(data[:cut]))
                check(False, f'a layout cut at {cut} was read')
            except ld.FormatError:
                pass
        check(isinstance(lp.FULL_WIDTH, set), 'preview module loads')

    if FAILS:
        print(f'layout_tools_check: {len(FAILS)} failure(s)')
        sys.exit(1)
    print('layout_tools_check: all checks passed')


if __name__ == '__main__':
    main()
