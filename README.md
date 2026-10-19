# California Speed (arcade): static recompilation

> [!NOTE]
> This project is vibe coded: nearly all of its code was written by an AI coding assistant (Claude),
> directed and play-tested by a human. Expect rough edges.

A native Windows version of Atari Games' 1998 arcade racer **California Speed** (version 2.1a, Midway
Seattle hardware). The game's original MIPS code is translated into C ahead of time and compiled for
the PC; the arcade hardware around it is emulated:

- **CPU:** the R5000 (MIPS IV) executable is statically recompiled by an own recompiler
  (`tools/recomp.py`), with a runtime for memory, interrupts, the TLB and COP0.
- **Graphics:** the 3dfx Voodoo 1 is emulated at the register level and drawn with OpenGL at any internal
  resolution (3x by default), with an optional 16:9 widescreen mode. A software rasterizer is kept for
  reference.
- **Sound:** the DCS2 sound board's ADSP-2115 DSP runs the original sound program.
- **Board:** the GT64010 system controller, the I/O ASIC, CMOS, the IDE hard disk (read from a CHD
  image), and the steering wheel / pedal inputs.

The game is playable: attract mode, coin-up, track and car selection, and races at the original
57 frames per second. Known issues and technical notes are in [NOTES.md](NOTES.md).

## Game data (not included)

No game code or data is included in this repository. To run it you need your own copy of the game,
placed in a `data/` folder:

| File | What it is |
|---|---|
| `calspeed.chd` | The hard disk image (MAME's `calspeed` set, version 2.1a). |
| `vmunix` | The game executable, extracted from the disk image (`fs/CODE/vmunix` in its filesystem). |
| `caspd1_2.u32` | The boot ROM. |
| `sound102.u95` | The sound board ROM. |

The game creates `cmos.bin` (settings, audits, high scores) and `hdd_writes.bin` (its writes to the
disk) in the same folder.

## Building

Windows only for now. Requirements: Visual Studio 2022 with the C++ workload, LLVM (for `clang-cl`),
CMake, and Python 3. SDL2 is downloaded by CMake.

```
git clone --recursive https://github.com/ideka/cspeed-arcade
cd cspeed-arcade
build.bat
```

`build.bat` recompiles `data\vmunix` into `generated\` (so the game data must be in place first) and
builds `build\CaliforniaSpeedArcade.exe`. Machine-specific paths (such as a non-default LLVM folder) go
in `build_env.local.bat`; see `tools\build_env.bat`.

## Running

```
build\CaliforniaSpeedArcade.exe data
```

**Controller:** left stick steers, RT/LT (or A/B) accelerate/brake, Start, Back inserts a coin, X/Y/RB
change the view, LB the radio, d-pad gears 1-4.

**Keyboard:** arrows, Enter or 1 to start, 5 inserts a coin, Q/W/E views, R radio, A/S/D/F gears,
F2 operator menu, 9 service, F5 pause, F9 widescreen, F11 or Alt+Enter fullscreen.

Fullscreen and widescreen are remembered in `settings.ini`, in SDL's per-user folder
(`%APPDATA%\cspeed-arcade\cspeed-arcade`). Debugging options (environment variables) are listed in
[NOTES.md](NOTES.md).

## Credits

- **California Speed** was developed by Atari Games and published by Midway in 1998. All rights to the
  game belong to their respective owners; this project is not affiliated with them.
- **[MAME](https://www.mamedev.org/)** and its contributors: their drivers for the Midway Seattle
  hardware (`seattle.cpp`, `midwayic.cpp`, `gt64xxx.cpp`, `voodoo.cpp`, `dcs.cpp`) were the main
  hardware documentation for this project. Code ported from MAME, under the BSD-3-Clause license, with
  its copyright notices kept:
  - the ADSP-21xx CPU core (`runtime/adsp/`) and disassembler (`tools/adspdis/2100dasm.*`), by
    Aaron Giles;
  - the Voodoo pixel pipeline, in the software rasterizer (`runtime/swrender.cpp`) and the GPU
    renderer's shader (`runtime/gpurender.cpp`), by Aaron Giles;
  - the DCS sound board logic (`runtime/dcs.cpp`), by Aaron Giles.
- **[libchdr](https://github.com/rtissera/libchdr)** by Romain Tisserand (BSD-3-Clause), with
  zstd (BSD), the LZMA SDK (public domain) and miniz (MIT), for reading the CHD disk image.
- **[SDL2](https://www.libsdl.org/)** (zlib license) for the window, OpenGL context, audio and
  controllers.

## License

This project is licensed under the GNU General Public License version 3; see [COPYING](COPYING).
The files ported from MAME keep their BSD-3-Clause license headers, and the bundled libraries keep their
own licenses.
