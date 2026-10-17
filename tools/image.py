# Loads vmunix (the arcade game's MIPS ELF): its single loadable segment and its symbol table.
import struct

class Image:
    def __init__(self, path):
        d = open(path, 'rb').read()
        e = struct.unpack_from('<16sHHIIIIIHHHHHH', d, 0)
        self.entry = e[4]
        phoff, shoff, shnum = e[5], e[6], e[12]
        p = struct.unpack_from('<IIIIIIII', d, phoff)
        self.base, self.data, self.memsz = p[2], d[p[1]:p[1] + p[4]], p[5]
        secs = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * 40) for i in range(shnum)]
        self.funcs = {}    # address -> (name, size)
        self.objects = {}  # address -> (name, size)
        for s in secs:
            if s[1] != 2:
                continue
            strtab = secs[s[6]][4]
            for off in range(s[4], s[4] + s[5], 16):
                name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', d, off)
                n = d[strtab + name:d.index(b'\0', strtab + name)].decode()
                if info & 15 == 2:
                    if value not in self.funcs or not self.funcs[value][0].startswith('_'):
                        self.funcs[value] = (n, size)
                elif info & 15 == 1:
                    self.objects[value] = (n, size)
        self.text_end = self.base + len(self.data)

    def word(self, addr):
        return struct.unpack_from('<I', self.data, addr - self.base)[0]

    def in_text(self, addr):
        return self.base <= addr < self.text_end
