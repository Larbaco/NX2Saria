import argparse
import re
import sys

LINE = re.compile(r'perf: frame (\d+): interval avg ([0-9.]+) ms .*replay avg ([0-9.]+) ms; ab (\d+)/(\d+); world (-?[0-9.]+) s')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--skip', type=float, default=10, help='seconds of world clock left out after the load')
    a = ap.parse_args()
    side = {0: [], 1: []}
    for line in open(a.log, encoding='utf-8', errors='replace'):
        m = LINE.search(line)
        if not m or float(m.group(6)) < a.skip:
            continue
        b, n = int(m.group(4)), int(m.group(5))
        if b and b != n:
            continue
        side[1 if b else 0].append((n, float(m.group(2)), float(m.group(3))))
    if not side[0] or not side[1]:
        print('no whole A and B windows (NX2SARIA_AB unset, or the run too short)')
        return 1
    def means(w):
        frames = sum(x[0] for x in w)
        return sum(x[0] * x[1] for x in w) / frames, sum(x[0] * x[2] for x in w) / frames, sum(x[0] * x[1] for x in w) / 1000
    for k, name in ((0, 'A'), (1, 'B')):
        iv, rp, secs = means(side[k])
        print(f'{name}: {len(side[k]):3d} windows ({secs:5.1f} s), interval {iv:6.2f} ms ({1000 / iv:5.1f} fps), replay {rp:5.2f} ms')
    (ia, ra, _), (ib, rb, _) = means(side[0]), means(side[1])
    print(f'B - A: interval {ib - ia:+.2f} ms ({100 * (ib - ia) / ia:+.1f}%), replay {rb - ra:+.2f} ms ({100 * (rb - ra) / ra:+.1f}%)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
