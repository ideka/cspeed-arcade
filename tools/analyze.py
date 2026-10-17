# Sanity checks before recompiling: function sizes, unknown instructions, calls into the middle of
# functions, jumps out of functions and indirect jumps.  analyze.py <vmunix>
import collections, sys
from image import Image
from mips import decode

img = Image(sys.argv[1])
starts = sorted(img.funcs)
ends = {}
for i, a in enumerate(starts):
    name, size = img.funcs[a]
    nxt = starts[i + 1] if i + 1 < len(starts) else img.text_end
    ends[a] = a + size if size else nxt
    if a + size > nxt:
        print('overlap: %s %08x+%x > next %08x' % (name, a, size, nxt))

def owner(addr):
    import bisect
    i = bisect.bisect_right(starts, addr) - 1
    return starts[i] if i >= 0 and addr < ends[starts[i]] else None

unknown = collections.Counter()
mid_calls = collections.Counter()
outside = collections.Counter()
jr = []
zero = [img.funcs[a][0] for a in starts if img.funcs[a][1] == 0]
for a in starts:
    for pc in range(a, ends[a], 4):
        i = decode(img.word(pc))
        if i.op is None:
            unknown[(img.funcs[a][0], hex(img.word(pc)))] += 1
            continue
        if i.kind == 'jump':
            t = (pc & 0xF0000000) | i.target
            if i.link:
                if t not in img.funcs:
                    mid_calls[(t, img.funcs.get(owner(t), ('?',))[0])] += 1
            elif not (a <= t < ends[a]):
                outside[(img.funcs[a][0], hex(t), img.funcs.get(t, ('mid',))[0])] += 1
        elif i.kind == 'branch':
            t = pc + 4 + (i.simm << 2)
            if not (a <= t < ends[a]):
                outside[(img.funcs[a][0], hex(t), img.funcs.get(t, ('mid',))[0])] += 1
        elif i.kind == 'jr' and i.rs != 31:
            jr.append((img.funcs[a][0], hex(pc), i.rs))
print('functions:', len(starts), 'size-0:', zero)
print('unknown instructions:', sum(unknown.values()))
for k, v in unknown.most_common(40): print('  ', k, v)
print('calls into the middle of functions:')
for (t, o), v in sorted(mid_calls.items()): print('   %08x in %s x%d' % (t, o, v))
print('jumps/branches out of their function:')
for k, v in sorted(outside.items()): print('  ', k, v)
print('jr non-ra:', len(jr))
for k in jr: print('  ', k)
