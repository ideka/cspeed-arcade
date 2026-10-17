# Side-by-side software / GPU frames from ARC_RENDERER=both dumps, with the difference:
#   compare_frames.py <dump dir> <frame numbers...> -o out.png
import argparse, os
from PIL import Image, ImageChops

ap = argparse.ArgumentParser()
ap.add_argument('dir')
ap.add_argument('frames', nargs='*', type=int)
ap.add_argument('-o', default='compare.png')
a = ap.parse_args()
frames = a.frames or sorted(int(f[3:8]) for f in os.listdir(a.dir) if f.startswith('gpu'))[::10]
rows = []
for n in frames:
    sw = Image.open(os.path.join(a.dir, 'frame%05d.png' % n)).convert('RGB')
    gpu = Image.open(os.path.join(a.dir, 'gpu%05d.png' % n)).convert('RGB').resize(sw.size)
    diff = ImageChops.difference(sw, gpu).point(lambda v: min(255, v * 4))
    row = Image.new('RGB', (sw.width * 3, sw.height))
    row.paste(sw, (0, 0))
    row.paste(gpu, (sw.width, 0))
    row.paste(diff, (sw.width * 2, 0))
    rows.append(row)
    print(n, 'mean diff', sum(ImageChops.difference(sw, gpu).convert('L').getdata()) / (sw.width * sw.height))
out = Image.new('RGB', (rows[0].width, sum(r.height for r in rows)))
y = 0
for r in rows:
    out.paste(r, (0, y))
    y += r.height
out.save(a.o)
