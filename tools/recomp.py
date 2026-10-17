# Static recompiler: translates vmunix (little-endian MIPS IV) into C.
#
#   recomp.py <vmunix> <config.toml> <output dir>
#
# Every function becomes `void f_XXXXXXXX(Ctx *c)`. Functions are found by following control flow from
# the symbol table's functions (static functions are not in it): call targets become functions, and a
# jump or branch out of a function becomes a tail call. A function's region is its symbol's extent, or
# for a function found by the traversal the space up to the next function; a call into the middle of a
# function becomes a second entry into the same region (the code is emitted again for it).
#
# The config lists functions to skip (implemented natively by the runtime), extra entry points and
# hooks (C code inserted before an instruction).
import bisect, collections, os, sys, tomllib
from image import Image
from mips import decode

GPR = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
       's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']
FUNCS_PER_FILE = 150

class Func:
    def __init__(self, entry, lo, hi):
        self.entry, self.lo, self.hi = entry, lo, hi
        self.pcs = set()        # instructions reached (not counting delay slots of their own)
        self.labels = set()     # branch / jump-table targets inside the region
        self.tables = {}        # pc of `jr reg` -> sorted targets
        self.falls_out = set()  # region-end addresses reached by falling through

class Recompiler:
    def __init__(self, img, cfg):
        self.img, self.cfg = img, cfg
        self.skip = {int(a, 16) if isinstance(a, str) else a for a in cfg.get('skip', [])}
        self.hooks = collections.defaultdict(list)
        for h in cfg.get('hook', []):
            self.hooks[h['at']].append(h['code'])
        self.sized = {}  # symbol functions with a size: start -> end
        for a, (n, s) in img.funcs.items():
            if s:
                self.sized[a] = a + s
        self.sized_starts = sorted(self.sized)
        self.entries = set(img.funcs) | set(cfg.get('entries', []))
        self.ptr_words = None

    def name(self, a):
        return self.img.funcs[a][0] if a in self.img.funcs else None

    def sized_owner(self, a):
        i = bisect.bisect_right(self.sized_starts, a) - 1
        if i >= 0 and a < self.sized[self.sized_starts[i]]:
            return self.sized_starts[i]
        return None

    def region(self, entry, starts):
        o = self.sized_owner(entry)
        if o is not None:
            return o, self.sized[o]
        # Found by the traversal (or unsized): up to the next function start that is not inside it.
        i = bisect.bisect_right(starts, entry)
        hi = starts[i] if i < len(starts) else self.img.text_end
        return entry, hi

    def words_in(self, lo, hi):
        """Words anywhere in the image whose value lies in [lo, hi): jump table candidates."""
        if self.ptr_words is None:
            d = self.img.data
            self.ptr_words = sorted({int.from_bytes(d[i:i + 4], 'little') for i in range(0, len(d), 4)})
        i = bisect.bisect_left(self.ptr_words, lo)
        out = []
        while i < len(self.ptr_words) and self.ptr_words[i] < hi:
            if self.ptr_words[i] & 3 == 0:
                out.append(self.ptr_words[i])
            i += 1
        return out

    def const_before(self, pc, reg):
        lo = None
        for back in range(4, 24, 4):
            i = decode(self.img.word(pc - back))
            if i.op == 'addiu' and i.rt == reg and i.rs == reg and lo is None:
                lo = i.simm
            elif i.op == 'lui' and i.rt == reg:
                return ((i.imm << 16) + (lo or 0)) & 0xFFFFFFFF
            elif reg in (i.rt, i.rd) and i.op not in ('sw', 'sh', 'sb', 'sd'):
                return None
        return None

    def explore(self, f, new_entries):
        img = self.img
        todo = [f.entry]
        f.labels.add(f.entry)
        while todo:
            pc = todo.pop()
            while True:
                if pc in f.pcs:
                    break
                if not (f.lo <= pc < f.hi):
                    f.falls_out.add(pc)
                    new_entries.add(pc)
                    break
                f.pcs.add(pc)
                i = decode(img.word(pc))
                if i.op is None:
                    raise Exception('unknown instruction %08x at %08x (function %08x)' % (i.word, pc, f.entry))
                if i.kind is None:
                    pc += 4
                    continue
                if i.kind == 'jump':
                    t = (pc & 0xF0000000) | i.target
                    if i.link:
                        new_entries.add(t)
                        pc += 8
                        continue
                    if f.lo <= t < f.hi:
                        f.labels.add(t)
                        todo.append(t)
                    else:
                        new_entries.add(t)
                    break
                if i.kind == 'branch':
                    t = (pc + 4 + (i.simm << 2)) & 0xFFFFFFFF
                    always = always_taken(i)
                    if i.link:
                        new_entries.add(t)
                    elif f.lo <= t < f.hi:
                        f.labels.add(t)
                        todo.append(t)
                    else:
                        new_entries.add(t)
                    if always and not i.link:
                        break
                    pc += 8
                    continue
                if i.kind == 'jalr':
                    pc += 8
                    continue
                if i.kind == 'jr':
                    if i.rs != 31:
                        targets = self.words_in(f.lo, f.hi)
                        # A constant built right before the jump (lui + addiu into the same register).
                        k = self.const_before(pc, i.rs)
                        if k is not None and f.lo <= k < f.hi:
                            targets = sorted(set(targets) | {k})
                        f.tables[pc] = targets
                        for t in targets:
                            f.labels.add(t)
                            todo.append(t)
                    break

    def pointer_candidates(self):
        """Addresses of code that nothing explored reaches: built with lui + addiu/ori in explored code,
        or stored as words in data. Only kept if they explore cleanly."""
        img = self.img
        covered = set()
        for f in self.funcs.values():
            covered |= f.pcs
        cands = set()
        for f in self.funcs.values():
            hi = {}
            for pc in sorted(f.pcs):
                i = decode(img.word(pc))
                if i.op == 'lui':
                    hi[i.rt] = i.imm << 16
                elif i.op in ('addiu', 'ori') and i.rs in hi:
                    v = (hi[i.rs] + i.simm) & 0xFFFFFFFF if i.op == 'addiu' else hi[i.rs] | i.imm
                    cands.add(v)
        d = img.data
        for o in range(0, len(d), 4):
            v = int.from_bytes(d[o:o + 4], 'little')
            if img.base + o not in covered:
                cands.add(v)
        out = set()
        starts = sorted(self.entries)
        for v in cands:
            if v & 3 or not img.in_text(v) or v in covered or v in self.entries or v in self.skip:
                continue
            # A function start follows the end of something: a return's delay slot, a nop, or data.
            lo, hi_ = self.region(v, starts)
            try:
                f = Func(v, lo, hi_)
                reached = set()
                self.explore(f, reached)
                if any(not img.in_text(t) for t in reached):
                    continue
            except Exception:
                continue
            first = decode(img.word(v))
            if first.op == 'addiu' and first.rt == 29 and first.rs == 29 and first.simm < 0:
                out.add(v)
            else:
                # Skip alignment padding back to the previous function's return.
                p = v - 4
                while p >= img.base and img.word(p) == 0 and p not in covered:
                    p -= 4
                ends = lambda i: (i.op == 'jr' and i.rs == 31) or i.op == 'eret'
                if ends(decode(img.word(p - 4))) or ends(decode(img.word(v - 8))):
                    out.add(v)
        return out

    def run(self):
        # Iterate until no new entry points appear.
        while True:
            self.traverse()
            extra = self.pointer_candidates()
            if not extra:
                break
            self.entries |= extra
        return self.funcs

    def traverse(self):
        while True:
            starts = sorted(self.entries)
            self.funcs = {}
            new = set()
            for e in starts:
                if e in self.skip:
                    continue
                lo, hi = self.region(e, starts)
                f = Func(e, lo, hi)
                self.explore(f, new)
                self.funcs[e] = f
            new -= self.entries
            if not new:
                break
            self.entries |= new

    # ---- code generation ----

    def r(self, n):
        return '0' if n == 0 else 'c->r[%d]' % n

    def r32(self, n):
        return '0' if n == 0 else '(uint32_t)c->r[%d]' % n

    def set(self, n, expr):
        if n == 0:
            return '(void)(%s);' % expr if ('rd' in expr) else ''
        return 'c->r[%d] = %s;' % (n, expr)

    def ea(self, i):
        return '(uint32_t)(%s + %d)' % (self.r32(i.rs), i.simm) if i.rs else '(uint32_t)%d' % i.simm

    def call(self, t):
        if t in self.skip or t in self.funcs:
            return 'f_%08x(c);' % t
        raise Exception('call to unknown function %08x' % t)

    def insn(self, i, pc):
        """C for a non-control-flow instruction."""
        op, rs, rt, rd, sa = i.op, i.rs, i.rt, i.rd, i.sa
        R, R32, S = self.r, self.r32, self.set
        if op == 'sll':
            if i.word == 0:
                return ''
            return S(rd, 'S32(%s << %d)' % (R32(rt), sa))
        simple = {
            'srl': lambda: S(rd, 'S32(%s >> %d)' % (R32(rt), sa)),
            'sra': lambda: S(rd, 'S32((int32_t)%s >> %d)' % (R32(rt), sa)),
            'sllv': lambda: S(rd, 'S32(%s << (%s & 31))' % (R32(rt), R(rs))),
            'srlv': lambda: S(rd, 'S32(%s >> (%s & 31))' % (R32(rt), R(rs))),
            'srav': lambda: S(rd, 'S32((int32_t)%s >> (%s & 31))' % (R32(rt), R(rs))),
            'dsll': lambda: S(rd, '%s << %d' % (R(rt), sa)),
            'dsrl': lambda: S(rd, '%s >> %d' % (R(rt), sa)),
            'dsra': lambda: S(rd, '(uint64_t)((int64_t)%s >> %d)' % (R(rt), sa)),
            'dsll32': lambda: S(rd, '%s << %d' % (R(rt), sa + 32)),
            'dsrl32': lambda: S(rd, '%s >> %d' % (R(rt), sa + 32)),
            'dsra32': lambda: S(rd, '(uint64_t)((int64_t)%s >> %d)' % (R(rt), sa + 32)),
            'dsllv': lambda: S(rd, '%s << (%s & 63)' % (R(rt), R(rs))),
            'dsrlv': lambda: S(rd, '%s >> (%s & 63)' % (R(rt), R(rs))),
            'dsrav': lambda: S(rd, '(uint64_t)((int64_t)%s >> (%s & 63))' % (R(rt), R(rs))),
            'movz': lambda: 'if (%s == 0) { %s }' % (R(rt), S(rd, R(rs))),
            'movn': lambda: 'if (%s != 0) { %s }' % (R(rt), S(rd, R(rs))),
            'movf': lambda: 'if (!FCC(%d)) { %s }' % (i.cc, S(rd, R(rs))),
            'movt': lambda: 'if (FCC(%d)) { %s }' % (i.cc, S(rd, R(rs))),
            'syscall': lambda: 'rt_syscall(c, 0x%08x);' % pc,
            'break': lambda: 'rt_break(c, 0x%08x, %d);' % (pc, (i.word >> 6) & 0xFFFFF),
            'sync': lambda: '',
            'mfhi': lambda: S(rd, 'c->hi'),
            'mflo': lambda: S(rd, 'c->lo'),
            'mthi': lambda: 'c->hi = %s;' % R(rs),
            'mtlo': lambda: 'c->lo = %s;' % R(rs),
            'mult': lambda: 'MULT(c, %s, %s);' % (R(rs), R(rt)),
            'multu': lambda: 'MULTU(c, %s, %s);' % (R(rs), R(rt)),
            'div': lambda: 'DIV(c, %s, %s);' % (R(rs), R(rt)),
            'divu': lambda: 'DIVU(c, %s, %s);' % (R(rs), R(rt)),
            'dmult': lambda: 'rt_dmult(c, %s, %s);' % (R(rs), R(rt)),
            'dmultu': lambda: 'rt_dmultu(c, %s, %s);' % (R(rs), R(rt)),
            'ddiv': lambda: 'rt_ddiv(c, %s, %s);' % (R(rs), R(rt)),
            'ddivu': lambda: 'rt_ddivu(c, %s, %s);' % (R(rs), R(rt)),
            'add': lambda: S(rd, 'S32(%s + %s)' % (R32(rs), R32(rt))),
            'addu': lambda: S(rd, 'S32(%s + %s)' % (R32(rs), R32(rt))),
            'sub': lambda: S(rd, 'S32(%s - %s)' % (R32(rs), R32(rt))),
            'subu': lambda: S(rd, 'S32(%s - %s)' % (R32(rs), R32(rt))),
            'and': lambda: S(rd, '%s & %s' % (R(rs), R(rt))),
            'or': lambda: S(rd, '%s | %s' % (R(rs), R(rt))),
            'xor': lambda: S(rd, '%s ^ %s' % (R(rs), R(rt))),
            'nor': lambda: S(rd, '~(%s | %s)' % (R(rs), R(rt))),
            'slt': lambda: S(rd, '(int64_t)%s < (int64_t)%s' % (R(rs), R(rt))),
            'sltu': lambda: S(rd, '(uint64_t)%s < (uint64_t)%s' % (R(rs), R(rt))),
            'dadd': lambda: S(rd, '%s + %s' % (R(rs), R(rt))),
            'daddu': lambda: S(rd, '%s + %s' % (R(rs), R(rt))),
            'dsub': lambda: S(rd, '%s - %s' % (R(rs), R(rt))),
            'dsubu': lambda: S(rd, '%s - %s' % (R(rs), R(rt))),
            'tge': lambda: 'if ((int64_t)%s >= (int64_t)%s) rt_trap(c, 0x%08x);' % (R(rs), R(rt), pc),
            'tgeu': lambda: 'if ((uint64_t)%s >= (uint64_t)%s) rt_trap(c, 0x%08x);' % (R(rs), R(rt), pc),
            'tlt': lambda: 'if ((int64_t)%s < (int64_t)%s) rt_trap(c, 0x%08x);' % (R(rs), R(rt), pc),
            'tltu': lambda: 'if ((uint64_t)%s < (uint64_t)%s) rt_trap(c, 0x%08x);' % (R(rs), R(rt), pc),
            'teq': lambda: 'if (%s == %s) rt_trap(c, 0x%08x);' % (R(rs), R(rt), pc),
            'tne': lambda: 'if (%s != %s) rt_trap(c, 0x%08x);' % (R(rs), R(rt), pc),
            'tgei': lambda: 'if ((int64_t)%s >= %d) rt_trap(c, 0x%08x);' % (R(rs), i.simm, pc),
            'tgeiu': lambda: 'if ((uint64_t)%s >= (uint64_t)(int64_t)%d) rt_trap(c, 0x%08x);' % (R(rs), i.simm, pc),
            'tlti': lambda: 'if ((int64_t)%s < %d) rt_trap(c, 0x%08x);' % (R(rs), i.simm, pc),
            'tltiu': lambda: 'if ((uint64_t)%s < (uint64_t)(int64_t)%d) rt_trap(c, 0x%08x);' % (R(rs), i.simm, pc),
            'teqi': lambda: 'if ((int64_t)%s == %d) rt_trap(c, 0x%08x);' % (R(rs), i.simm, pc),
            'tnei': lambda: 'if ((int64_t)%s != %d) rt_trap(c, 0x%08x);' % (R(rs), i.simm, pc),
            'addi': lambda: S(rt, 'S32(%s + %d)' % (R32(rs), i.simm)),
            'addiu': lambda: S(rt, 'S32(%s + %d)' % (R32(rs), i.simm)),
            'daddi': lambda: S(rt, '%s + (int64_t)%d' % (R(rs), i.simm)),
            'daddiu': lambda: S(rt, '%s + (int64_t)%d' % (R(rs), i.simm)),
            'slti': lambda: S(rt, '(int64_t)%s < %d' % (R(rs), i.simm)),
            'sltiu': lambda: S(rt, '(uint64_t)%s < (uint64_t)(int64_t)%d' % (R(rs), i.simm)),
            'andi': lambda: S(rt, '%s & 0x%x' % (R(rs), i.imm)),
            'ori': lambda: S(rt, '%s | 0x%x' % (R(rs), i.imm)),
            'xori': lambda: S(rt, '%s ^ 0x%x' % (R(rs), i.imm)),
            'lui': lambda: S(rt, 'S32(0x%xu)' % (i.imm << 16)),
            'cache': lambda: '',
            'pref': lambda: '',
            'prefx': lambda: '',
        }
        if op in simple:
            return simple[op]()
        ea = self.ea(i)
        loads = {'lb': 'S8(rd8(%s))', 'lbu': 'rd8(%s)', 'lh': 'S16(rd16(%s))', 'lhu': 'rd16(%s)',
                 'lw': 'S32(rd32(%s))', 'lwu': 'rd32(%s)', 'ld': 'rd64(%s)', 'll': 'S32(rd32(%s))',
                 'lld': 'rd64(%s)'}
        if op in loads:
            return S(rt, loads[op] % ea)
        stores = {'sb': 'wr8(%s, (uint8_t)%s);', 'sh': 'wr16(%s, (uint16_t)%s);', 'sw': 'wr32(%s, (uint32_t)%s);',
                  'sd': 'wr64(%s, %s);'}
        if op in stores:
            return stores[op] % (ea, R(rt))
        if op == 'sc':
            return 'wr32(%s, (uint32_t)%s); %s' % (ea, R(rt), S(rt, '1'))
        if op == 'scd':
            return 'wr64(%s, %s); %s' % (ea, R(rt), S(rt, '1'))
        if op in ('lwl', 'lwr', 'ldl', 'ldr'):
            return S(rt, 'rt_%s(%s, %s)' % (op, ea, R(rt)))
        if op in ('swl', 'swr', 'sdl', 'sdr'):
            return 'rt_%s(%s, %s);' % (op, ea, R(rt))
        if op == 'lwc1':
            return 'c->f[%d].w = rd32(%s);' % (rt, ea)
        if op == 'ldc1':
            return 'c->f[%d].u = rd64(%s);' % (rt, ea)
        if op == 'swc1':
            return 'wr32(%s, c->f[%d].w);' % (ea, rt)
        if op == 'sdc1':
            return 'wr64(%s, c->f[%d].u);' % (ea, rt)
        xea = '(uint32_t)(%s + %s)' % (R32(rs), R32(rt))
        if op == 'lwxc1':
            return 'c->f[%d].w = rd32(%s);' % (sa, xea)
        if op == 'ldxc1':
            return 'c->f[%d].u = rd64(%s);' % (sa, xea)
        if op == 'swxc1':
            return 'wr32(%s, c->f[%d].w);' % (xea, rd)
        if op == 'sdxc1':
            return 'wr64(%s, c->f[%d].u);' % (xea, rd)
        if op in ('mfc0', 'dmfc0'):
            return S(rt, 'S32(rt_mfc0(c, %d))' % rd)
        if op in ('mtc0', 'dmtc0'):
            return 'rt_mtc0(c, %d, %s);' % (rd, R32(rt))
        if op in ('tlbr', 'tlbwi', 'tlbwr', 'tlbp'):
            return 'rt_tlb(c, 0x%08x);' % pc
        if op == 'eret':
            return 'rt_eret(c); return;'
        if op == 'wait':
            return 'rt_wait(c);'
        if op == 'mfc1':
            return S(rt, 'S32(c->f[%d].w)' % rd)
        if op == 'dmfc1':
            return S(rt, 'c->f[%d].u' % rd)
        if op == 'mtc1':
            return 'c->f[%d].w = %s;' % (rd, R32(rt))
        if op == 'dmtc1':
            return 'c->f[%d].u = %s;' % (rd, R(rt))
        if op == 'cfc1':
            return S(rt, 'S32(rt_cfc1(c, %d))' % rd)
        if op == 'ctc1':
            return 'rt_ctc1(c, %d, %s);' % (rd, R32(rt))
        if '.' in op:
            return self.fpu(i, pc)
        raise Exception('no translation for %s at %08x' % (op, pc))

    def fpu(self, i, pc):
        name, f = i.op.rsplit('.', 1)
        fd, fs, ft = i.fd, i.fs, i.ft
        v = {'s': 'f', 'd': 'd', 'w': 'i', 'l': 'l'}[f]
        F = lambda n: 'c->f[%d].%s' % (n, v)
        if name in ('add', 'sub', 'mul', 'div'):
            o = {'add': '+', 'sub': '-', 'mul': '*', 'div': '/'}[name]
            return '%s = %s %s %s;' % (F(fd), F(fs), o, F(ft))
        if name == 'sqrt':
            return '%s = %s(%s);' % (F(fd), 'sqrtf' if f == 's' else 'sqrt', F(fs))
        if name == 'abs':
            return '%s = %s(%s);' % (F(fd), 'fabsf' if f == 's' else 'fabs', F(fs))
        if name == 'neg':
            return '%s = -%s;' % (F(fd), F(fs))
        if name == 'mov':
            return 'c->f[%d].u = c->f[%d].u;' % (fd, fs)
        if name == 'recip':
            return '%s = %s / %s;' % (F(fd), '1.0f' if f == 's' else '1.0', F(fs))
        if name == 'rsqrt':
            return '%s = %s / %s(%s);' % (F(fd), '1.0f' if f == 's' else '1.0', 'sqrtf' if f == 's' else 'sqrt', F(fs))
        if name in ('movf', 'movt'):
            return 'if (%sFCC(%d)) c->f[%d].u = c->f[%d].u;' % ('!' if name == 'movf' else '', i.cc, fd, fs)
        if name in ('movz', 'movn'):
            return 'if (%s %s 0) c->f[%d].u = c->f[%d].u;' % (self.r(ft), '==' if name == 'movz' else '!=', fd, fs)
        if name.startswith('cvt.') or name.split('.')[0] in ('round', 'trunc', 'ceil', 'floor'):
            kind, to = name.split('.')
            dst = {'s': 'f', 'd': 'd', 'w': 'i', 'l': 'l'}[to]
            if kind == 'cvt' and to in 'sd':
                return 'c->f[%d].%s = (%s)%s;' % (fd, dst, 'float' if to == 's' else 'double', F(fs))
            fn = {'cvt': 'rt_rint', 'round': 'rt_round', 'trunc': 'rt_trunc', 'ceil': 'rt_ceil', 'floor': 'rt_floor'}[kind]
            return 'c->f[%d].%s = %s_%s(c, (double)%s);' % (fd, dst, fn, to, F(fs))
        if name in ('madd', 'msub', 'nmadd', 'nmsub'):
            fr = i.rs
            t = '(%s * %s)' % (F(fs), F(ft))
            e = {'madd': '%s + %s', 'msub': '%s - %s', 'nmadd': '-(%s + %s)', 'nmsub': '-(%s - %s)'}[name] % (t, F(fr))
            return '%s = %s;' % (F(fd), e)
        if name == 'c':
            cond = i.word & 15
            parts = []
            if cond & 1:
                parts.append('UNORD(%s, %s)' % (F(fs), F(ft)))
            if cond & 2:
                parts.append('%s == %s' % (F(fs), F(ft)))
            if cond & 4:
                parts.append('%s < %s' % (F(fs), F(ft)))
            return 'SET_FCC(c, %d, %s);' % (i.cc, ' || '.join(parts) if parts else '0')
        raise Exception('no translation for %s at %08x' % (i.op, pc))

    def cond(self, i):
        R = self.r
        op = i.op
        if op in ('beq', 'beql'):
            return '%s == %s' % (R(i.rs), R(i.rt))
        if op in ('bne', 'bnel'):
            return '%s != %s' % (R(i.rs), R(i.rt))
        if op in ('blez', 'blezl'):
            return '(int64_t)%s <= 0' % R(i.rs)
        if op in ('bgtz', 'bgtzl'):
            return '(int64_t)%s > 0' % R(i.rs)
        if op in ('bltz', 'bltzl', 'bltzal', 'bltzall'):
            return '(int64_t)%s < 0' % R(i.rs)
        if op in ('bgez', 'bgezl', 'bgezal', 'bgezall'):
            return '(int64_t)%s >= 0' % R(i.rs)
        if op in ('bc1f', 'bc1fl'):
            return '!FCC(%d)' % i.cc
        if op in ('bc1t', 'bc1tl'):
            return 'FCC(%d)' % i.cc
        raise Exception(op)

    def emit_func(self, f, out):
        img = self.img
        n = self.name(f.entry)
        out.append('// %s%s' % (n or 'f_%08x' % f.entry, '' if f.entry == f.lo else ' (inside %s)' % (self.name(f.lo) or '%08x' % f.lo)))
        out.append('void f_%08x(Ctx *c) {' % f.entry)
        out.append('    TRACE_ENTRY(c, 0x%08x);' % f.entry)
        if f.entry != min(f.pcs):
            out.append('    goto L_%08x;' % f.entry)
        pcs = sorted(f.pcs)
        delay_slots = {pc + 4 for pc in pcs if decode(img.word(pc)).kind is not None}
        for pc in delay_slots:
            if pc in f.labels:
                f.labels.add(pc + 4)  # the standalone copy of the delay slot jumps over to here
        for pc in pcs:
            if pc in delay_slots:
                continue
            i = decode(img.word(pc))
            if pc in f.labels:
                out.append('L_%08x:' % pc)
            for h in self.hooks.get(pc, []):
                out.append('    ' + h)
            if i.kind is None:
                s = self.insn(i, pc)
                if s:
                    out.append('    ' + s)
                if pc + 4 not in f.pcs and pc + 4 >= f.hi:
                    out.append('    ' + self.call(pc + 4) + ' return;')
                continue
            # Control flow with its delay slot.
            d = decode(img.word(pc + 4))
            if d.kind is not None:
                raise Exception('branch in delay slot at %08x' % pc)
            ds = self.insn(d, pc + 4)
            after = pc + 8
            if i.kind == 'jump':
                t = (pc & 0xF0000000) | i.target
                if i.link:
                    out.append('    c->r[31] = 0x%08x; %s %s' % (S32str(after), ds, self.call(t)))
                    self.fallthrough(f, after, out)
                elif f.lo <= t < f.hi:
                    out.append('    %s %sgoto L_%08x;' % (ds, 'CHECK(c, 0x%08x); ' % pc if t <= pc else '', t))
                else:
                    out.append('    %s %s return;' % (ds, self.call(t)))
            elif i.kind == 'branch':
                t = (pc + 4 + (i.simm << 2)) & 0xFFFFFFFF
                cnd = self.cond(i)
                if i.link:
                    link = 'c->r[31] = 0x%08x; ' % S32str(after)
                    if i.likely:
                        out.append('    %sif (%s) { %s %s }' % (link, cnd, ds, self.call(t)))
                    else:
                        out.append('    { int t_ = %s; %s%s if (t_) { %s } }' % (cnd, link, ds, self.call(t)))
                    self.fallthrough(f, after, out)
                    continue
                if f.lo <= t < f.hi:
                    take = '%sgoto L_%08x;' % ('CHECK(c, 0x%08x); ' % pc if t <= pc else '', t)
                else:
                    take = '%s return;' % self.call(t)
                if always_taken(i):
                    out.append('    %s %s' % (ds, take))
                elif i.likely:
                    out.append('    if (%s) { %s %s }' % (cnd, ds, take))
                else:
                    out.append('    { int t_ = %s; %s if (t_) { %s } }' % (cnd, ds, take))
                if not always_taken(i):
                    self.fallthrough(f, after, out)
            elif i.kind == 'jalr':
                out.append('    { uint32_t t_ = (uint32_t)%s; c->r[%d] = 0x%08x; %s rt_call(c, t_); }' % (self.r(i.rs), i.rd, S32str(after), ds))
                self.fallthrough(f, after, out)
            elif i.kind == 'jr':
                if i.rs == 31:
                    out.append('    %s return;' % ds)
                else:
                    cases = ' '.join('case 0x%08x: goto L_%08x;' % (t, t) for t in f.tables[pc])
                    out.append('    { uint32_t t_ = (uint32_t)%s; %s CHECK(c, 0x%08x); switch (t_) { %s default: rt_jump(c, t_, 0x%08x); return; } }' % (self.r(i.rs), ds, pc, cases, pc))
            if pc + 4 in f.labels:
                # Something branches to the delay slot: emit it on its own as well.
                if after in f.pcs:
                    out.append('    goto L_%08x;' % after)
                out.append('L_%08x:' % (pc + 4))
                out.append('    ' + ds)
                self.fallthrough(f, after, out)
        out.append('}')
        out.append('')

    def fallthrough(self, f, after, out):
        if after not in f.pcs:
            if after >= f.hi or after < f.lo:
                out.append('    ' + self.call(after) + ' return;')
            else:
                out.append('    rt_unreachable(c, 0x%08x); return;' % after)

def always_taken(i):
    return i.op in ('beq', 'beql', 'bgez', 'bgezl', 'bgezal', 'bgezall') and i.rs == 0 and (not i.op.startswith('beq') or i.rt == 0)

def S32str(v):
    return v & 0xFFFFFFFF

def main():
    img = Image(sys.argv[1])
    cfg = tomllib.load(open(sys.argv[2], 'rb'))
    outdir = sys.argv[3]
    os.makedirs(outdir, exist_ok=True)
    rc = Recompiler(img, cfg)
    funcs = rc.run()
    allf = sorted(set(funcs) | rc.skip)
    hdr = ['#pragma once', '#include "recomp.h"', '#ifdef __cplusplus', 'extern "C" {', '#endif', '']
    hdr += ['void f_%08x(Ctx *c);%s' % (a, ' // native' if a in rc.skip else '') for a in allf]
    hdr += ['', '#ifdef __cplusplus', '}', '#endif']
    open(os.path.join(outdir, 'funcs.h'), 'w').write('\n'.join(hdr) + '\n')
    entries = sorted(funcs)
    nfiles = 0
    for k in range(0, len(entries), FUNCS_PER_FILE):
        out = ['#include "funcs.h"', '']
        for e in entries[k:k + FUNCS_PER_FILE]:
            rc.emit_func(funcs[e], out)
        open(os.path.join(outdir, 'funcs_%02d.c' % nfiles), 'w').write('\n'.join(out) + '\n')
        nfiles += 1
    tab = ['#include "funcs.h"', '', 'const FuncEntry rt_func_table[] = {']
    for a in allf:
        tab.append('    { 0x%08x, f_%08x, "%s" },' % (a, a, rc.name(a) or ''))
    tab += ['};', 'const int rt_func_count = %d;' % len(allf), '']
    open(os.path.join(outdir, 'table.c'), 'w').write('\n'.join(tab))
    # Remove stale files from a previous run with more functions.
    for name in os.listdir(outdir):
        if name.startswith('funcs_') and int(name[6:8]) >= nfiles:
            os.remove(os.path.join(outdir, name))
    found = [e for e in entries if e not in img.funcs]
    print('%d functions (%d not in the symbol table), %d files' % (len(entries), len(found), nfiles))

if __name__ == '__main__':
    main()
