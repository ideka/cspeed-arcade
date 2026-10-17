# MIPS IV (R5000, little-endian) instruction decoder for the recompiler.
#
# decode(word) returns an Insn with the mnemonic in .op and the raw fields; .kind says how it affects
# control flow: None, 'branch' (conditional, possibly likely), 'jump' (j/jal), 'jr', 'jalr'.

class Insn:
    __slots__ = ('word', 'op', 'rs', 'rt', 'rd', 'sa', 'imm', 'simm', 'target', 'fmt', 'ft', 'fs', 'fd',
                 'kind', 'likely', 'link', 'cc')

    def __init__(self, word):
        self.word = word
        self.rs = (word >> 21) & 31
        self.rt = (word >> 16) & 31
        self.rd = (word >> 11) & 31
        self.sa = (word >> 6) & 31
        self.imm = word & 0xFFFF
        self.simm = self.imm - 0x10000 if self.imm & 0x8000 else self.imm
        self.target = (word & 0x3FFFFFF) << 2
        self.fmt = self.rs
        self.ft = self.rt
        self.fs = self.rd
        self.fd = self.sa
        self.kind = None
        self.likely = False
        self.link = False
        self.cc = 0
        self.op = None

SPECIAL = {0: 'sll', 2: 'srl', 3: 'sra', 4: 'sllv', 6: 'srlv', 7: 'srav', 8: 'jr', 9: 'jalr', 10: 'movz',
           11: 'movn', 12: 'syscall', 13: 'break', 15: 'sync', 16: 'mfhi', 17: 'mthi', 18: 'mflo', 19: 'mtlo',
           20: 'dsllv', 22: 'dsrlv', 23: 'dsrav', 24: 'mult', 25: 'multu', 26: 'div', 27: 'divu',
           28: 'dmult', 29: 'dmultu', 30: 'ddiv', 31: 'ddivu', 32: 'add', 33: 'addu', 34: 'sub', 35: 'subu',
           36: 'and', 37: 'or', 38: 'xor', 39: 'nor', 42: 'slt', 43: 'sltu', 44: 'dadd', 45: 'daddu',
           46: 'dsub', 47: 'dsubu', 48: 'tge', 49: 'tgeu', 50: 'tlt', 51: 'tltu', 52: 'teq', 54: 'tne',
           56: 'dsll', 58: 'dsrl', 59: 'dsra', 60: 'dsll32', 62: 'dsrl32', 63: 'dsra32'}
REGIMM = {0: 'bltz', 1: 'bgez', 2: 'bltzl', 3: 'bgezl', 8: 'tgei', 9: 'tgeiu', 10: 'tlti', 11: 'tltiu',
          12: 'teqi', 14: 'tnei', 16: 'bltzal', 17: 'bgezal', 18: 'bltzall', 19: 'bgezall'}
PRIMARY = {2: 'j', 3: 'jal', 4: 'beq', 5: 'bne', 6: 'blez', 7: 'bgtz', 8: 'addi', 9: 'addiu', 10: 'slti',
           11: 'sltiu', 12: 'andi', 13: 'ori', 14: 'xori', 15: 'lui', 20: 'beql', 21: 'bnel', 22: 'blezl',
           23: 'bgtzl', 24: 'daddi', 25: 'daddiu', 26: 'ldl', 27: 'ldr', 32: 'lb', 33: 'lh', 34: 'lwl',
           35: 'lw', 36: 'lbu', 37: 'lhu', 38: 'lwr', 39: 'lwu', 40: 'sb', 41: 'sh', 42: 'swl', 43: 'sw',
           44: 'sdl', 45: 'sdr', 46: 'swr', 47: 'cache', 48: 'll', 49: 'lwc1', 51: 'pref', 52: 'lld',
           53: 'ldc1', 55: 'ld', 56: 'sc', 57: 'swc1', 60: 'scd', 61: 'sdc1', 63: 'sd'}
COP1X = {0: 'lwxc1', 1: 'ldxc1', 8: 'swxc1', 9: 'sdxc1', 15: 'prefx', 32: 'madd.s', 33: 'madd.d',
         40: 'msub.s', 41: 'msub.d', 48: 'nmadd.s', 49: 'nmadd.d', 56: 'nmsub.s', 57: 'nmsub.d'}
FPU = {0: 'add', 1: 'sub', 2: 'mul', 3: 'div', 4: 'sqrt', 5: 'abs', 6: 'mov', 7: 'neg', 8: 'round.l',
       9: 'trunc.l', 10: 'ceil.l', 11: 'floor.l', 12: 'round.w', 13: 'trunc.w', 14: 'ceil.w', 15: 'floor.w',
       17: 'movcf', 18: 'movz', 19: 'movn', 21: 'recip', 22: 'rsqrt', 32: 'cvt.s', 33: 'cvt.d', 36: 'cvt.w',
       37: 'cvt.l'}
FMT = {16: 's', 17: 'd', 20: 'w', 21: 'l'}

def decode(word):
    i = Insn(word)
    op = word >> 26
    if op == 0:
        fn = word & 63
        if fn == 1:
            i.op = 'movt' if word & 0x10000 else 'movf'
            i.cc = (word >> 18) & 7
        else:
            i.op = SPECIAL.get(fn)
        if i.op == 'jr':
            i.kind = 'jr'
        elif i.op == 'jalr':
            i.kind = 'jalr'
            i.link = True
    elif op == 1:
        i.op = REGIMM.get(i.rt)
        if i.op and i.op[0] == 'b':
            i.kind = 'branch'
            i.likely = i.op.endswith('l') and i.op not in ('bgezal', 'bltzal')
            i.link = 'al' in i.op
    elif op == 16:
        if i.rs & 16:
            i.op = {1: 'tlbr', 2: 'tlbwi', 6: 'tlbwr', 8: 'tlbp', 24: 'eret', 32: 'wait'}.get(word & 63)
        else:
            i.op = {0: 'mfc0', 1: 'dmfc0', 4: 'mtc0', 5: 'dmtc0'}.get(i.rs)
    elif op == 17:
        if i.rs == 8:
            i.cc = (word >> 18) & 7
            i.op = ['bc1f', 'bc1t', 'bc1fl', 'bc1tl'][(word >> 16) & 3]
            i.kind = 'branch'
            i.likely = (word >> 17) & 1 == 1
        elif i.rs < 16:
            i.op = {0: 'mfc1', 1: 'dmfc1', 2: 'cfc1', 4: 'mtc1', 5: 'dmtc1', 6: 'ctc1'}.get(i.rs)
        elif i.rs in FMT:
            fn = word & 63
            f = FMT[i.rs]
            if fn >= 48:
                i.op = 'c.' + f
                i.cc = (word >> 8) & 7
            elif fn in FPU:
                i.op = FPU[fn] + '.' + f
                if fn == 17:
                    i.op = ('movt.' if word & 0x10000 else 'movf.') + f
                    i.cc = (word >> 18) & 7
    elif op == 19:
        i.op = COP1X.get(word & 63)
    else:
        i.op = PRIMARY.get(op)
        if op in (2, 3):
            i.kind = 'jump'
            i.link = op == 3
        elif op in (4, 5, 6, 7, 20, 21, 22, 23):
            i.kind = 'branch'
            i.likely = op >= 20
    return i
