import re

_NUM = re.compile(r'^\s*v?(\d+(?:\.\d+)*)(?:-([0-9A-Za-z.-]+))?(?:\+([0-9A-Za-z.-]+))?\s*$')


def _pre_key(pre):
    return tuple((0, int(p), '') if p.isdigit() else (1, 0, p) for p in pre.split('.'))


def parse(text):
    m = _NUM.match(text or '')
    if not m:
        raise ValueError(f'not a version: {text!r}')
    nums = tuple(int(x) for x in m.group(1).split('.'))
    nums = nums + (0,) * (4 - len(nums)) if len(nums) < 4 else nums
    pre = m.group(2)
    return nums, (1,) if pre is None else (0, _pre_key(pre))


def newer(a, b):
    return parse(a) > parse(b)


def display(text):
    return (text or '').split('+', 1)[0].strip()
