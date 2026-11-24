# OutRunners Amiga M10

M10 is a single-player build for a full 68060 with Fast RAM, AmigaOS 3.x and AHI. Choose a PAL AGA display or a supported 16-bit RTG screen. M10 includes the sky sampling fix, automatic AHI setup and THEA500/CD32-compatible pad calibration.

## Play

Extract the update ZIP in the game drawer beside your prepared `assets` directory. The play launchers use the streamed music and effects banks. First-time users can generate these from their World ROM set with the [extraction and audio preparation instructions](../README.md). Initialising the upstream submodules with `git submodule update --init --recursive` supplies the extraction tools and native build dependencies.

Double-click **Play Pad4 AGA** or **Play Pad4 RTG**. Both set the stack automatically. At first launch connect a CD32-compatible controller to Amiga port 2 and follow the button prompts. Both launchers read the same saved mapping. X inserts a credit, the assigned centre button starts play, A accelerates, B brakes, and the shoulders shift in Manual. Escape returns to Workbench. For keyboard play with an existing prepared asset drawer, use AmigaShell:

```
Stack 65536
M9-Pad4-060 RTG MENU30 FXGUARD
```

Use AGA in place of RTG for the native display. The M9-Pad4-060 filename is the executable in the tested M10 archive.

## Build the update

Use CMake, Ninja, Python 3 and the m68k-amigaos GCC cross compiler with its Amiga NDK and vasmm68k_mot on PATH. The release was checked with GCC 16.2. From the repository root run

```sh
cmake -S . -B build/m10 -G Ninja -DORUNNERS_AMIGA=ON -DCMAKE_TOOLCHAIN_FILE=ports/amiga/toolchain-060.cmake
cmake --build build/m10 --target OutRunners Pad4Audio Pad4Setup
python3 ports/amiga/m10/package.py build/bin OutRunners-Amiga-M10-060.zip
```

The update contains the game, launchers, controls and notices. Players supply their own game assets. This tag uses the original M10 graphics loader.
