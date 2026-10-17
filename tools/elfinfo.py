# Print the ELF header, program headers and sections of a 32-bit little-endian ELF.
import struct, sys
d = open(sys.argv[1], 'rb').read()
e = struct.unpack_from('<16sHHIIIIIHHHHHH', d, 0)
print('type %d machine %d entry %08x phoff %x shoff %x flags %08x' % (e[1], e[2], e[4], e[5], e[6], e[7]))
phoff, shoff, phnum, shnum, shstrndx = e[5], e[6], e[10], e[12], e[13]
for i in range(phnum):
    p = struct.unpack_from('<IIIIIIII', d, phoff + i * 32)
    print('PH type %x off %x vaddr %08x filesz %x memsz %x flags %x' % (p[0], p[1], p[2], p[4], p[5], p[6]))
secs = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * 40) for i in range(shnum)]
strtab = secs[shstrndx][4] if secs else 0
for s in secs:
    n = d[strtab + s[0]:d.index(b'\0', strtab + s[0])].decode()
    print('SH %-12s type %2d flags %x addr %08x off %7x size %7x' % (n, s[1], s[2], s[3], s[4], s[5]))
