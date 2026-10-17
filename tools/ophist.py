# Histogram of MIPS opcodes in vmunix's .text, to see which instruction classes the recompiler must handle.
import struct, sys, collections
d = open(sys.argv[1], 'rb').read()[0x1000:0x1000 + 0xfc890]
names = {}
prim = {0:'SPECIAL',1:'REGIMM',2:'j',3:'jal',4:'beq',5:'bne',6:'blez',7:'bgtz',8:'addi',9:'addiu',10:'slti',11:'sltiu',12:'andi',13:'ori',14:'xori',15:'lui',16:'COP0',17:'COP1',18:'COP2',19:'COP1X',20:'beql',21:'bnel',22:'blezl',23:'bgtzl',24:'daddi',25:'daddiu',26:'ldl',27:'ldr',32:'lb',33:'lh',34:'lwl',35:'lw',36:'lbu',37:'lhu',38:'lwr',39:'lwu',40:'sb',41:'sh',42:'swl',43:'sw',44:'sdl',45:'sdr',46:'swr',47:'cache',48:'ll',49:'lwc1',51:'pref',53:'ldc1',55:'ld',56:'sc',57:'swc1',61:'sdc1',63:'sd'}
spec = {0:'sll',2:'srl',3:'sra',4:'sllv',6:'srlv',7:'srav',8:'jr',9:'jalr',10:'movz',11:'movn',12:'syscall',13:'break',15:'sync',16:'mfhi',17:'mthi',18:'mflo',19:'mtlo',20:'dsllv',22:'dsrlv',23:'dsrav',24:'mult',25:'multu',26:'div',27:'divu',28:'dmult',29:'dmultu',30:'ddiv',31:'ddivu',32:'add',33:'addu',34:'sub',35:'subu',36:'and',37:'or',38:'xor',39:'nor',42:'slt',43:'sltu',44:'dadd',45:'daddu',46:'dsub',47:'dsubu',48:'tge',52:'teq',56:'dsll',58:'dsrl',59:'dsra',60:'dsll32',62:'dsrl32',63:'dsra32',1:'movci'}
c = collections.Counter()
for i in range(0, len(d), 4):
    w = struct.unpack_from('<I', d, i)[0]
    op = w >> 26
    if op == 0: n = spec.get(w & 63, 'spec%d' % (w & 63))
    elif op == 16: n = 'cop0 rs%d fn%d' % ((w >> 21) & 31, w & 63) if (w >> 21) & 16 else 'cop0 rs%d' % ((w >> 21) & 31)
    elif op == 17: n = 'cop1 fmt%d fn%d' % ((w >> 21) & 31, w & 63) if (w >> 21) & 16 else 'cop1 rs%d' % ((w >> 21) & 31)
    elif op == 19: n = 'cop1x fn%d' % (w & 63)
    elif op == 28: n = 'SPECIAL2 fn%d' % (w & 63)
    else: n = prim.get(op, 'op%d' % op)
    c[n] += 1
for n, k in sorted(c.items(), key=lambda x: -x[1]): print('%6d %s' % (k, n))
