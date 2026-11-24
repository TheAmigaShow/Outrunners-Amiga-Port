# OutRunners Arcade on AmigaOS 3

Use a 68030 or newer with Fast RAM, AmigaOS 3.x and AHI v4 or newer.
Set up AHI unit 0 for stereo playback. The AGA mode uses a native PAL
320×256 screen. RTG uses a 16-bit Picasso96 or CyberGraphX mode of at least
320×256. AGA opens a private custom screen. Both backends hide the Amiga
mouse pointer over the game window. RTG centres the 320×224 game image
in a full-screen window with black margins.
The arcade simulation still targets its native 60 Hz. The PAL display
scans at 50 Hz and may skip presentations when no back buffer is safe.

## Install and run

On a desktop computer, use Python 3.10 or newer to extract your World
`orunners` ROM set from the base repository:

```sh
python3 extract_assets.py /path/to/orunners.zip
```

Copy the resulting `assets` directory beside the Amiga executable. The
extracted directory retains the width information the big-endian asset
loader needs. ROMs and extracted game assets are supplied by the player.

Run from the Amiga Shell:

```
Stack 65536
OutRunners AGA
OutRunners RTG
OutRunners P96
```

Startup lists the loading stages and shows the sprite sheet number while
preparing graphics. Both Shell and Workbench game launches open a separate
window titled **OutRunners - Loading**. If the window cannot open, an
interactive Shell shows the progress there instead. Shell redirection also
captures the progress messages. The window closes when the game starts. A Workbench launch
that fails during loading shows the error and waits for Return. A redirected
Shell launch also writes the startup error to its log. Use the 65536-byte
Shell stack shown above, or set the Workbench tool icon stack to 65536. The first
game screen opens after the game assets have loaded.

The solo Amiga build closes race entries as soon as it registers the local player.
Car and transmission selection retain their normal input timeouts.

AGA is the default. A second argument selects a different asset directory.
Use the arrow keys to steer, accelerate and brake. **5** inserts a coin,
**Return** starts, **S** and **D** shift, **M** changes music mode,
**comma** and **period** change station, and **Esc** quits.

For a quick display check, run `OutRunners AGATEST` or `OutRunners P96TEST`.
Each opens the display before loading any assets, shows red, green, blue and
white bars, checks pixel readback and returns to the Shell. A screen-open
failure reports the mode ID and the Intuition error when available.

`RTG` tries native Picasso96 first and falls back to CyberGraphX if it cannot
open a Picasso96 screen. `P96` specifically requests Picasso96API.library
v2 and an enabled 16-bit 565 screen mode of at least 320×256. Larger modes
centre the game at its original resolution. Frame rendering takes place in
Fast RAM. Picasso96 receives the completed rows using p96WritePixelArray.

### Adaptive pacing in the 68060 HUD prototype

The default launchers use the original M8 scheduler with MENU30 and FXGUARD.
The optional ADAPTIVE setting uses the 60 Hz game clock. A late tick lets a following
tick omit screen drawing while still updating controls, game state, sprite
banks and audio. After four consecutive timing skips, ordinary gameplay
draws again. Fades and an explicitly selected `MENU30` use their existing
audio recovery policies. A PAL 50 Hz display can show up to 50 distinct
pictures per second. A 60 Hz display can show 60 if the hardware keeps up.

`AUDIOSTATS` reports average game ticks/s and presented frames/s on exit.
`FIXEDPACE` selects the earlier scheduler. A scripted ten-frame renderer
benchmark still requests all ten pictures and reports elapsed times for
sprites, screen drawing, end-of-tick audio service and presentation.

## Stereo radio and live effects

With the supplied prepared banks, put `music.ima` and `pcm22.bin` in
`assets/sound/` beside the extracted sound data. The left cabinet's radio
selects the recording. Engines, tyres, speech and other effects respond to
the game. Both sources pass through the linked peak compressor before AHI
receives signed 8-bit stereo at 22,050 Hz. The prepared effects bank lowers
the off-road tyre loop by 12 dB so the radio stays audible on the verge.

To prepare the banks from your own extracted World assets, install CMake, a
native C compiler, Python 3.10 or newer, and FFmpeg with the IMA WAV encoder.
Run the following on the desktop from the patched repository root, adjusting
the last executable path if you configured a different output directory.

```sh
python3 ports/amiga/prepare_pcm22.py assets
cmake -S . -B build/music-host -DCMAKE_BUILD_TYPE=Release
cmake --build build/music-host --target orunners_amiga_music
python3 ports/amiga/prepare_music.py assets build/bin/orunners_amiga_music
```

The preparation tool runs the original sound program one playlist at a time
and records its actual one-shot end or loop point. The cabinet routes each
player's music to one side. The desktop tool spreads instruments across the
two speakers for this version. The prepared bank contains 38 distinct tracks
for the radio, jingles and stage music. Its 1,024-byte stereo IMA blocks
hold 1,017 sample pairs. The World bank occupies about 68.4 MiB on disk.
The original extracted samples and the 2 MiB half-rate effects bank stay in
Fast RAM.

A separate AmigaDOS process services AHI and reads the next 8 KiB compressed
page while decoding the current page. Its two read pages use 16 KiB Fast RAM.
The effects queue, decoded block, mix block and IMA tables use about another
17 KiB, and the process requests a 16 KiB stack. The two AHI output buffers
use 2 KiB Chip RAM in total.

The AHI streaming process normally runs at priority +5. If the game is
launched at a higher priority, the port requests one step above it, up to
Exec's priority limit. During normal play the game renders at emulated VBlank,
queues each frame's live effects before presentation, and uses its usual
60 Hz pacing deadline. AGA may skip a presentation while waiting for a safe
back buffer.

The stereo music follows AHI's clock. If live effects run short, the mixer
fades them out over 32 samples (about 1.45 ms). It normally collects 768
effects samples (about 35 ms of audio) before fading them back in.
Sustained overload can still interrupt effects or slow the game.

The game automatically uses the banks it finds. Use `LIVE` to compare live
music synthesis with streaming, `PCM22` to require the half-rate bank, or
`PCM16` for the original 16-bit output. The optional `HALF` setting samples
the scenery tilemaps at 160×112 and expands them. Sprites, the HUD, text and
bitmap overlays retain their native resolution. The default uses full-resolution
scenery on both processors. Add `FINE` to sample every vertical row of large
roadside sprites. Removing the switch restores the faster default.

```
OutRunners AGA assets
OutRunners RTG assets HALF
OutRunners RTG assets FINE
OutRunners AGA assets LIVE PCM22
```

## Build

Use the AmigaPorts `m68k-amigaos-gcc` SDK with its NDK, CyberGraphX and
AHI headers and `vasmm68k_mot` on PATH. The port was cross-built with
AmigaPorts GCC 16.2-rc25 and CMake using:

```sh
cmake -S . -B build/amiga -DORUNNERS_AMIGA=ON \
  -DCMAKE_TOOLCHAIN_FILE=ports/amiga/toolchain.cmake
cmake --build build/amiga --target OutRunners
```

For a full 68060 with its integrated FPU enabled, build in a separate directory.
Use AmigaOS with support for the installed 68060 accelerator.

```sh
cmake -S . -B build/amiga060 -DORUNNERS_AMIGA=ON \
  -DCMAKE_TOOLCHAIN_FILE=ports/amiga/toolchain-060.cmake \
  -DORUNNERS_BIN_DIR="$PWD/build/bin060"
cmake --build build/amiga060 --target OutRunners
```

The 68060 target uses hardware floating point and Kalms's 1:1 eight-plane
`c2p1x1_8_c5_040`, which the author designed for both the 040 and 060. On a
68030 the default toolchain uses software floating point and `c2p1x1_8_c5_030`.

The full 68060 is a separate optimisation target. The compiler flags are
`-m68060 -mtune=68060 -mhard-float`. For measured hot loops, use
68060-specific C or assembly where its instruction pairing, cache behaviour
or integrated FPU offers an advantage. Select the implementation at build
time and retain the 68030 implementation for that binary. Report timings
from the 68040-compatible emulator as estimates. Use a physical 68060 to
verify gains that depend on its pipelines or caches.

## Timed replay on an Amiga

Run either command with the supplied replay script and extracted assets.
The optional script argument enables a ten-frame gameplay benchmark.

```
OutRunners AGA assets race_left.txt
OutRunners RTG assets race_left.txt
OutRunners AGA assets race_left.txt 7291 PCM22
```

The number after the script selects the first measured frame. The default is
2401. With the solo entry change, the race begins earlier in this replay.
Match the race event when comparing timings against older builds. The game advances the simulation and sound chips, warms up the
renderer for seven frames, and reports ten frame times in microseconds using timer.device's E-clock. Measurements include the game,
renderer, display submission and AHI queueing. Intentional frame pacing is
disabled. The last two lines report how often both AHI buffers had already drained
and how many frames the display accepted. A drained queue indicates a possible
audible gap. The counter excludes startup and preroll.

### Audio dropouts during play

With streamed music, `FXGUARD` checks the live effects ring before drawing on
the start grid and throughout driving. With fewer than 512
sample frames queued it can skip a draw and its display upload. During ordinary racing it draws the following tick. During a screen fade,
the guard also checks how late the last tick finished and can skip up to five
consecutive draws. Game logic, sprite updates and sound continue on each tick.
Car selection with `MENU30` uses its own pacing. `AUDIOSTATS` or `CHIMETRACE` reports skipped and
eligible draws in each phase. The option applies to interactive play.
For a comparison use the same car, display and audio settings on both runs.

For the opening sequence on the 68060, use `OPENINGTRACE` with the usual
streamed music and PCM22 effects. The capture starts before the audio worker
and stores the first 1292 submitted stereo blocks (30 seconds) in Fast RAM.
Press Esc to write the report. Quitting earlier writes the partial capture.
Both tables use microseconds from the same origin. An `fx_published` value of
zero identifies initial blocks submitted before the game produced effects.
`output_peak8` is the largest absolute signed 8-bit sample sent in the block.
A zero peak means that block contained silence. `prepare_us` covers music
reads, decode and mixing. `queue_us` includes waiting for a previous AHI write.
The game table times the draw callback, sprite callback, end-of-tick sound
work and display upload. These elapsed times include task preemption.
`fade_active` records the screen fade flag and `draw_skipped` records the draw decision.
Game modes 2, 3 and 4 identify the title, car presentation and demo race.

For a race-start trace in streamed PCM22 mode, run `Stack 65536` followed by
`OutRunners RTG MENU30 CHIMETRACE >RAM:chime.txt`. Play a solo race and
drive for at least five seconds of game time before pressing Esc. The flag
enables the usual audio statistics. It also captures the opening cues from
race setup through the first 300 driving ticks. The two tasks store separate
records in Fast RAM and the game writes the report after audio shutdown.

`fx_sample` is the generated 22,050 Hz effects timeline. On a mixer row,
the fresh samples begin at `fx_first` and span `fx_used` frames. A source
event in that interval reaches output frame `out_first + fx_sample - fx_first`.
`out_first` is relative to the first captured AHI block. `first_gap_out`
marks where the mixer begins filling a shortage. The sound command and the
MultiPCM key-on can occur at different samples. Registers 1, 2/3, 4 and 5
record sample selection, pitch, key and level respectively. The last line
counts discarded records if the capture filled.

Launch with `OutRunners RTG AUDIOSTATS >RAM:audio.txt`, play through car
select and into the race, then press Escape. Read the report with
`Type RAM:audio.txt`. The game accumulates counters in memory and prints after
play. AGA and P96 accept the same option.

Add `MENU30` to limit car-select drawing to at most every other game tick.
During interactive streamed playback, a late tick carries up to four ticks
of timing debt. If the previous tick finished more than a quarter tick late,
the menu can skip extra draws while the game and effects producer catch up.
The first menu frame is drawn, and at most five successive menu draws are
skipped. Race presentation resumes its usual cadence when selection ends.
Input, simulation, sprite-buffer updates and PCM generation run on every tick.
`FIXEDMENU` selects the previous fixed alternating cadence for comparison.
Live audio and scripted benchmarks keep the fixed cadence when using `MENU30`.

`AUDIOSTATS` reports separate mean durations for ticks that drew and skipped
car select. Work includes processing, audio publication and any display wait.
The second duration also includes intentional frame pacing. Compare the mean
effects delivery interval and the percentage of output needing gap handling
across equivalent visits to car select. A machine that cannot process even a
skipped tick in real time can still run short of effects.

Each row counts writes submitted during car selection (including transmission),
the starting grid, driving, or other scenes. Initial audio setup and shutdown
are excluded. Compare rows with nonzero writes. `ahi_drains` counts times the
previous AHI request had already completed at the driver's queue check. This
signals a possible playback gap. The host audio device has its own buffering.

For streamed audio, `fx_missing_frames` counts effect sample frames unavailable
when the worker mixed a block. `fx_held_frames` counts additional available
frames it deferred to rebuild the effects queue. Their sum divided by
`output_frames` is the fraction of output that needed effects gap handling.
`fx_short_blocks` counts affected writes. With live audio the effects-ring
columns read `n/a`.

`FX512` lets streamed effects resume once 512 sample frames are queued after
a shortage. The default threshold is 768. For a comparison with the same
assets, display, mixer gain and AHI settings, launch each run in turn:

```
OutRunners RTG MENU30 AUDIOSTATS >RAM:levels768.txt
OutRunners RTG MENU30 AUDIOSTATS FX512 >RAM:levels512.txt
```

Both commands use the assets folder beside the executable. Spend a similar
time at car select, choose the same car and station and drive for ten seconds
before pressing Escape. Record the same scene at the same emulator and
recording volume to compare it with the meter.

`peak_dBFS` measures the highest absolute sample across both channels as
submitted to AHI. `rms_dBFS` measures the unweighted RMS across both channels
over the whole scene, including silence. Full scale is the magnitude of the
most negative PCM value. Silent scenes report `-inf`. Scenes with no writes
report `n/a`. Compare against sample peak and unweighted RMS for the same
section of the recording.

For the automatic gain comparison, use streamed music and the normal
768-frame recovery threshold in both runs:

```
OutRunners RTG MENU30 AUDIOSTATS >RAM:gainoff.txt
OutRunners RTG MENU30 AUDIOSTATS AGC >RAM:gainon.txt
```

`AGC` applies stereo-linked makeup gain before the peak compressor. It starts
at unity and adds up to 3 dB. Once per 512-frame block it measures the mean
absolute stereo output. Gain can rise by one Q8 step per block and fall by
four. Upward changes pause during near-silence, an active music fade or an
effects shortage. The meters still measure the PCM actually submitted to AHI.
AGC requires the streamed music and PCM22 effects banks.

With streamed audio, `fx_intervals` counts time intervals between consecutive
nonempty effects deliveries within the same scene. `fx_mean_ms` and `fx_max_ms`
give their mean and longest duration. `fx_over_block` counts intervals longer
than one AHI output block (512 frames at 22,050 Hz, about 23.2 ms). The effects
queue can absorb a long interval if enough samples were already waiting.

## Memory and display

CMake writes the Hunk executable to `build/bin/OutRunners` by default.
The Amiga target
builds the arcade runtime and omits the LAN code and second display.

The game owns an Exec Fast RAM pool for assets, renderer and mixer allocations,
and cooperative task stacks. Returning to the launcher stack after Esc lets
the port close the audio and display backends and delete the entire pool.
The C runtime owns its own stdio buffers until process exit.
The game checks the locations of its startup stack, code and static data.
The renderer uses a 64 KiB colour lookup table in Fast RAM. AGA uses an
approximately 70 KiB packed output surface and 16-bit RTG uses 140 KiB.
At graphics startup the renderer builds an 8 KiB tile opacity table in Fast RAM.
Full-resolution scrolling layers use it to clear transparent spans or copy opaque
pens without testing every pixel. A tile whose opacity changes only between rows
needs one pen test per span. Other mixed tiles keep their per-pixel transparency.
In single-screen full-resolution racing scenes with two unclipped tilemaps and
empty text and bitmap layers, the renderer uses a 640-byte scratch row for the
road, samples the sky where visible, and writes the winning road, sky or sprite
colour into the packed Fast RAM surface. Sprites retain their arcade double
buffers and priority. For a band of repeating opaque sky at the top of the picture,
the renderer averages each source scanline's colours in the current arcade
palette and reuses that shade across the exposed sky. This removes horizontal
dither. Vertical scrolling, zoom and palette changes still move or recolour the
gradient. RTG uses RGB565 for the shade, and AGA uses the fixed RGB332 palette.
The renderer switches back to textured sampling when the tilemap varies across
the band, contains transparency or has high-contrast detail. It resolves road
markings, foreground silhouettes and sprite priority at individual pixels. An ordinary shadow halves
each channel of the underlying arcade colour before the output conversion.
Other layer layouts, blending and special palette effects use the general mixer.

With the default full-resolution setting, trees, rocks and plants with at least
48 visible pixels across and 32 visible scanlines can use 2×2 pixel sampling.
The `FINE` switch restores the previous horizontal pairs with independent scanlines.
The renderer checks the original sprite frame against the World scenery sheets.
Vehicles, the HUD and unknown frames keep their original sampling. The scenery
list occupies 808 bytes of static data. Both display modes keep all 224 game
scanlines and use the existing Fast RAM surfaces.
In dense foreground scenes, the road renderer skips pairs covered by opaque
higher-priority sprites and copies runs of exposed opaque road pixels. Two
small tables on the Fast RAM stack (about 8 KiB total, including the scratch
row) carry packed colours for the frame. The sprite renderer also reserves
up to 32 KiB on the Fast RAM heap to reuse decoded 4-bit ROM rows across
frames, scales and palettes. If allocation fails it reads the ROM directly.

AGA uses RGB332 followed by the CPU-specific Kalms converter into the inactive
Chip RAM bitplanes. When native screen buffers are unavailable, Intuition owns
the PAL screen and graphics.library blits the completed Kalms bitplanes to it.
On CyberGraphX, the renderer packs RGB565 or BGR565 in the
framebuffer's native byte order, and the backend copies completed scanlines
while the bitmap is locked. Other CyberGraphX layouts use the RGB upload path.
For unobstructed road and grass pixels, the renderer resolves each tile colour
directly. Scaled sprites reuse column mappings and read a contiguous run of
ROM words when the asset block covers the whole row. A bank wrap or gap
uses the ordinary word reader. The single-screen display defers clears for
the hidden cabinet until it is shown again.

Kalms's unmodified public-domain assembler and original attribution
are in this folder. The source is from Kalmalyzer/kalms-c2p at
`d8ecf79a3325615305dd800ae7704b518e0d9dda`.

The target has been cross-built. AGA and RTG display, AHI playback,
minimum Fast RAM and gameplay speed on an actual 68030 require a run
with the player's assets on an Amiga or a suitably configured emulator.
