#!/bin/sh
# Downloads the MAME sources used as hardware documentation into reference/mame (not committed).
cd "$(dirname "$0")/.." && mkdir -p reference/mame && cd reference/mame || exit 1
for f in src/mame/williams/seattle.cpp src/mame/williams/midwayic.cpp src/mame/williams/midwayic.h \
         src/devices/machine/gt64xxx.cpp src/devices/machine/gt64xxx.h src/devices/machine/smc91c9x.cpp \
         src/devices/video/voodoo.cpp src/devices/video/voodoo.h src/devices/video/voodoo_regs.h \
         src/devices/video/voodoo_render.cpp src/devices/video/voodoo_render.h src/devices/video/voodoo_pci.cpp \
         src/devices/cpu/adsp2100/adsp2100.cpp src/devices/cpu/adsp2100/adsp2100.h \
         src/devices/cpu/adsp2100/2100ops.hxx src/mame/shared/dcs.cpp src/mame/shared/dcs.h src/emu/video/poly.h; do
    curl -sfL -o "$(basename "$f")" "https://raw.githubusercontent.com/mamedev/mame/master/$f" || echo "failed: $f"
done
