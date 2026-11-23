import re
from collections import Counter

REG = re.compile(r'c->x\[(\d+)\]|c->sp\b')
WRITE = re.compile(r'c->(?:x\[(\d+)\]|(sp))\s*=(?!=)')
ESCAPE = re.compile(r'\brt_(?:svc|brk|udf)\(c\b')
SP = 31
SPILL, RELOAD, RELOAD_CALL = '\x00S', '\x00R', '\x00C'
CALLEE_SAVED = frozenset(range(19, 30)) | {SP}
LOCALS = 6


def name(r):
    return 'sp_' if r == SP else f'x{r}'


def cpu(r):
    return 'c->sp' if r == SP else f'c->x[{r}]'


def refs(text):
    return [SP if m.group(0) == 'c->sp' else int(m.group(1)) for m in REG.finditer(text)]


def written(text):
    return {SP if m.group(2) else int(m.group(1)) for m in WRITE.finditer(text)}


class Plan:
    def __init__(self, regs, spill, kept):
        self.regs = regs
        self.local = set(regs)
        self.spill = spill
        self.kept = kept

    def decl(self):
        return 'uint64_t ' + ', '.join(f'{name(r)} = {cpu(r)}' for r in self.regs) + ';' if self.regs else ''

    def resolve(self, i, text):
        def local(m):
            r = SP if m.group(0) == 'c->sp' else int(m.group(1))
            return name(r) if r in self.local else m.group(0)

        text = REG.sub(local, text)
        spill = ''.join(f'{cpu(r)} = {name(r)}; ' for r in sorted(self.spill[i]))
        reload = ''.join(f' {name(r)} = {cpu(r)};' for r in self.regs)
        reload_call = ''.join(f' {name(r)} = {cpu(r)};' for r in self.regs if r not in self.kept)
        return text.replace(SPILL, spill).replace(RELOAD, reload).replace(RELOAD_CALL, reload_call)


def plan(texts, succ, clean, n_locals=LOCALS, kept=CALLEE_SAVED):
    n = len(texts)
    uses = Counter(r for t in texts for r in refs(t))
    regs = {r for r, _ in sorted(uses.items(), key=lambda kv: (-kv[1], kv[0]))[:n_locals]}
    writes = [written(t) & regs for t in texts]
    dirty = [set() for _ in range(n)]
    seen = [False] * n
    work = [0] if n else []
    if n:
        seen[0] = True
    while work:
        i = work.pop()
        out = set() if i in clean else dirty[i] | writes[i]
        for s in succ[i]:
            if not seen[s] or not out <= dirty[s]:
                seen[s] = True
                dirty[s] |= out
                work.append(s)
    every = set().union(*writes) if writes else set()
    for i in range(n):
        if not seen[i]:
            dirty[i] = every
    return Plan(sorted(regs), [dirty[i] | writes[i] if SPILL in texts[i] else set() for i in range(n)], kept)
