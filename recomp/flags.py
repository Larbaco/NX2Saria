import re

KIND_DEF = re.compile(r'\b(subs|adds|logic)(32|64)\(c, ')
CARRY_DEF = re.compile(r'\b(adcs|sbcs)(32|64)\(c, ')
CARRY_USE = re.compile(r'\b(adc|sbc)(32|64)\(c, ')
COND = re.compile(r'COND_([A-Z]{2})\(c\)')
NZ_DEF = 'c->nzcv = '
NZ_USE = '(c->nzcv)'
KIND_NAME = {'subs': 'sub', 'adds': 'add', 'logic': 'logic'}

ENTRY, CALL = 'entry', 'call'


class FlagError(Exception):
    pass


def effect(code):
    reads = bool(COND.search(code) or CARRY_USE.search(code) or CARRY_DEF.search(code) or NZ_USE in code)
    if NZ_DEF in code or CARRY_DEF.search(code):
        return reads, 'nz'
    defs = KIND_DEF.findall(code)
    if len(defs) > 1:
        raise FlagError(f'several flag definitions in one instruction: {code}')
    if defs:
        op, size = defs[0]
        return reads, KIND_NAME[op] + size
    return reads, None


class Plan:

    def __init__(self, n):
        self.reader = [None] * n
        self.pack = [False] * n
        self.exit = [None] * n
        self.entry = False
        self.entry_read = False
        self.after_call = 0
        self.used = False

    def decl(self):
        return 'Flags F = {.nz = c->nzcv};' if self.entry else 'Flags F = {0};'

    def publish(self, i):
        k = self.exit[i]
        return f'c->nzcv = fnz_{k}(&F); ' if k else ''

    def rewrite(self, i, code):
        rk = self.reader[i]
        _, dk = effect(code)
        if rk:
            code = COND.sub(lambda m: f'fc_{rk}(&F, CC_{m.group(1)})', code)
            code = CARRY_DEF.sub(lambda m: f'{m.group(1)}{m.group(2)}f(&F, fc_{rk}(&F, CC_CS), ', code)
            code = CARRY_USE.sub(lambda m: f'{m.group(1)}{m.group(2)}f(fc_{rk}(&F, CC_CS), ', code)
            code = code.replace(NZ_USE, f'(fnz_{rk}(&F))')
        if dk == 'nz':
            code = code.replace(NZ_DEF, 'F.nz = ')
            code = re.sub(r'\(void\)(subs|adds)(32|64)\(c, ', lambda m: f'F.nz = nz_{KIND_NAME[m.group(1)]}{m.group(2)}(',
                          code)
        elif dk:
            code = KIND_DEF.sub(lambda m: f'f{KIND_NAME[m.group(1)]}{m.group(2)}(&F, ', code)
            if self.pack[i]:
                code += f' F.nz = fnz_{dk}(&F);'
        if 'c->nzcv' in code or COND.search(code) or KIND_DEF.search(code) or CARRY_USE.search(code):
            raise FlagError(f'flag access left after the rewrite: {code}')
        return code


def plan(effects, succ, calls, exits):
    n = len(effects)
    p = Plan(n)
    if not any(r or k for r, k in effects):
        return p
    p.used = True
    reach = [set() for _ in range(n)]
    reach[0].add(ENTRY)
    work = [0]
    while work:
        i = work.pop()
        out = {i} if effects[i][1] else ({CALL} if i in calls else reach[i])
        for s in succ[i]:
            if not out <= reach[s]:
                reach[s] |= out
                work.append(s)

    def kind(d):
        return 'nz' if d == ENTRY else effects[d][1]

    def use(r):
        kinds = {kind(d) for d in r}
        if ENTRY in r:
            p.entry = True
        if len(kinds) == 1:
            return kinds.pop()
        for d in r:
            if d != ENTRY:
                p.pack[d] = True
        return 'nz'

    for i, (reads, _) in enumerate(effects):
        if reads:
            p.after_call += CALL in reach[i]
            p.reader[i] = use(reach[i] - {CALL})
            p.entry_read |= ENTRY in reach[i]
    for i in exits:
        r = reach[i] - {CALL}
        if r and r != {ENTRY}:
            p.exit[i] = use(r)
    return p
