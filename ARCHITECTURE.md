# Architecture

What the code is and why it is that way. `CLAUDE.md` has the rules for
changing it.

## The program

One task, `app_main()`, in a loop:

1. **Capture** 1024 stereo frames from the two array microphones
   through `audio_out_capture_read()`, average them to mono, and
   multiply by a Hamming window.
2. **FFT** with esp-dsp's `dsps_fft2r_fc32()` and bit reversal, then the
   magnitude of each of the 512 useful bins.
3. **Render**: each of the 64 bars takes its bins (`spectrum_band()`),
   becomes dBFS and a fraction of the graph, goes through the
   ballistics, and repaints only the rows between its old state and its
   new one, straight into gfx's shadow buffer.
4. **Blit** the union of the rows any bar touched.

`main/spectrum.h` is all of the arithmetic and none of the hardware.
`main/analyzer.c` is the reverse.

## Numbering

The patches are a series from 0001. Each one's number and subject is a
heading below, and the next patch takes the next number.

### 0001 -- original/Tab5_SpectrumAnalyzer.ino, verbatim

The Arduino sketch, as the reference every later patch is read against.

### 0002 -- the port

From the sketch to ESP-IDF, on feckless-drivers v0.2.0,
feckless-graphics-handler v0.1.0 and esp-dsp. What changed, and what
could not stay the same:

**The samples are 24-bit scale.** M5Unified handed the sketch 16-bit
samples. feckless-drivers' capture hands back int32 at 24-bit scale --
16 bits shifted up, for the built-in pair -- so full scale is 2^23 and
`SPECTRUM_SAMPLE_FULL_SCALE` says so. The resolution is the same.
`test/spectrumtest.c` checks that a full-scale, bin-centred sine at that
scale reads 0 dBFS within 0.1 dB, and -20, -40 and -80 likewise.

**Mono is the average of the two microphones.** The sketch asked
M5Unified for mono and did not say how it got it. Averaging two mics a
short distance apart cancels a little at the frequencies where their
paths differ by half a wavelength; one channel alone is the alternative
if that shows.

**Capture keeps playback running.** The ES7210 and the ES8388 share an
I2S port, and capture re-clocks the playback channel rather than
opening a second one (feckless-drivers' `audio_out.h`). So the analyzer
calls `audio_out_init()` at 48 kHz and never writes to it.

**The FFT is esp-dsp's, the window is ours.** esp-dsp has Hann,
Blackman and others, but no Hamming, and the sketch's 0.54 coherent
gain is Hamming's. `s_window` is computed once at boot.
`dsps_fft2r_fc32()` is unnormalised, as arduinoFFT was, so the
full-scale magnitude formula is unchanged.

**Drawing goes into a shadow buffer, then to the glass.** M5GFX wrote
rows to the panel directly. gfx draws into a PSRAM shadow and
`gfx_blit()` copies bands of rows to the panel. The delta rendering is
the sketch's: a row's colour depends on its height, never on a bar's
level, so a growing bar paints only its new rows. The blit is of the
row range all bars together touched, full width.

**Landscape costs a transpose.** The panel is portrait. In landscape a
logical row is a panel column, so each blit is gathered through a
transpose (feckless-graphics-handler's `gfx.h`). Busy music touches most
of the graph's height every frame, which makes that transpose most of a
1280 x 600 region per frame. This is the first number to look at in the
log's `draw` figure if the framerate is under the capture ceiling.

**Rotation is a guess.** The sketch's `setRotation(3)` put it the right
way up against the case printing. `ANALYZER_ROTATION` is
`GFX_ROT_270`, which may or may not be the same landscape: if it boots
upside down, it is `GFX_ROT_90`.

**Text is Ark12.** M5GFX's 6x8 font at size n was 8n pixels; Ark12 is
12 pixels a scale step, so the title is 36 px where it was 32, and
every label is 24 px where they were 16 or 24.

### Not verified on a board

Nothing here has been built against ESP-IDF in the session that wrote
it; `analyzer.c` compiled on the host against stub headers only. The
things most likely to need a second look:

- **esp-dsp on silicon v1.3.** `dsps_fft2r_fc32()` selects its P4
  assembly when `CONFIG_DSP_OPTIMIZED`. If the FFT faults or returns
  garbage, `CONFIG_DSP_ANSI=y` in `sdkconfig.defaults` (then
  `rm sdkconfig`) takes the plain-C path and settles which it is.
- **The rotation**, above.
- **`draw` time in landscape**, above.

### 0003 -- capture in a task of its own, into a ring

The first board run of 0002 answered the three open questions. esp-dsp's
P4 FFT runs on silicon v1.3, 0.5 ms a frame. `GFX_ROT_270` is an
acceptable way up. And draw is 25-44 ms a frame, about 28 fps against a
47 fps capture ceiling: the landscape transpose, as predicted.

The same log showed a second problem: "capture 4 ms". Reading 1024 frames
at 48 kHz cannot take 4 ms unless most of them were already waiting --
and the only thing they wait in is the I2S DMA, 8 x 120 frames, 20 ms.
During a 30 ms draw it wrapped. So each window was whatever survived in
the DMA spliced to what came after: a discontinuity inside every FFT,
which smears a tone's energy into its neighbours. It did not make the
display lag -- the DMA is too small to fall behind by more than 20 ms --
it made it wrong.

A capture task now reads one DMA buffer (120 frames) at a time, for as
long as the program runs, averages to mono and appends to `monoring.h`'s
4096-sample ring. It is pinned to core 1 at priority 5, so no draw on
core 0 can hold it up. The drawing loop copies out the newest 1024
samples once at least 256 new ones have arrived (FRAME_HOP, 5.3 ms) and
draws them. The window is always contiguous and always current; frames
overlap when drawing is fast; the framerate is now set by drawing alone.

The log's "capture" figure now means the wait for FRAME_HOP new samples,
not the read, so it reads near zero while drawing is the slow part.

`monoring.h` is one writer, one reader, no lock: the writer publishes
its count with a release store after the samples, the reader acquires
it. spectrumtest checks the copy across the ring's wrap and the count's.

Expected on the board: framerate unchanged (still draw-bound, ~28),
"capture" near zero, and a pure tone narrower than before. The
landscape draw cost is the next patch's business.

### 0004 -- the axis starts at 80 Hz

`SPECTRUM_LOG_MIN_HZ` is 80, not the sketch's 50: on the board the array
microphones only start differentiating frequencies at about 80 Hz, so
the bars below it spent a sixth of the width on nothing resolvable. The
64 bars now span 80 Hz to 24 kHz, about 9.3% each instead of 10%, and
the first frequency label is 80 rather than 50. No other number moved.

### 0005 -- the boot banner

The first line app_main() prints, as the player's does:

    W (...) analyzer: === Lothesome Audio Analyzer === <version>, IDF <ver>, built <date> <time>

Everything above it in a log is the ROM, the bootloader and IDF's own
startup; everything below is this program, so it is the line to start
copying from. The version is `git describe` of the tree that was built,
with -dirty when it had changes, which is what matches a log to the
source a backtrace has to be decoded against. ESP_LOGW so it survives a
build with the info level turned down.

The player also lists, under its banner, any sdkconfig.defaults key the
build's sdkconfig disagrees with (its cmake/defaults_check.cmake). Not
here yet.

### 0006 -- landscape drawn onto the panel's own rows

With capture off the critical path (0003), draw was nearly all of
every frame: 17-23 ms in a quiet room, at GFX_ROT_270. That angle keeps
gfx's shadow buffer in landscape rows and sends each blit through a
gather-transpose, because a band of landscape rows is a band of panel
columns (feckless-graphics-handler's gfx.h calls it the expensive
angle).

So gfx stays at GFX_ROT_0 now, its shadow in the panel's own order, and
`landmap.h` says where each landscape pixel lands. The mapping is the
inverse of gfx's 270 gather -- px = ly, py = 1279 - lx -- so the picture
on the glass is the one 0002 showed, the way up already accepted on
the board. spectrumtest checks it against that gather for every pixel.

What it buys: a bar, being a run of landscape columns, is a run of
panel ROWS. Its changed landscape rows are one contiguous span of each
of them, so repaint_rows() works the colours out once and memcpy()s
them into each of the bar's 16 panel rows, and the blit of the frame's
dirty columns is a plain, contiguous band of panel rows. No transpose
anywhere.

What it costs: gfx's text calls draw upright on the panel, so this
file draws its own text from the Ark12 glyphs (land_text()), turned.
ASCII only, which is all the analyzer writes; no right-to-left, no
fullwidth, no ellipsis. gfx_fill_rect() still does the filling, through
land_fill().

The dirty range is now a range of landscape columns rather than rows:
the leftmost to the rightmost bar that changed. With every bar moving,
that is the graph's width, 1150 or so panel rows of 720 -- the same
pixel count the transposed blit moved, but copied straight.

Checked on the host by including analyzer.c in a harness with gfx
stubbed onto a plain 720 x 1280 buffer, drawing the static layout, forty
frames of a synthetic spectrum and the fps line, and reading the buffer
back through landmap.h: the landscape picture comes out whole and the
right way round. Not on a board yet. The figure to compare is the log's
"draw", against 0005's 17-18 ms in a quiet room -- and against a run
with music playing, which neither has had.

### 0007 -- a headset on the 3.5 mm jack

feckless-drivers' capture has had the jack's microphone since the
player's 5206: `AUDIO_CAPTURE_HEADSET`, the ES7210's fourth channel,
which only reaches the P4 in TDM -- the same TDM capture the array uses,
so the switch is an end and a begin, not a reconfiguration of the port.
The capture probe lines in every board log so far have shown that slot
(3) live.

The capture task now owns the source. Every 250 ms it asks
`audio_out_headphones()`; when the answer changes it ends the capture
and begins the other one, and publishes the choice. The drawing loop
names it on screen, between the title and the fps, whenever it changes.

Two things differ between the sources, and both are dealt with in the
task so spectrum.h does not change:

- **Channels.** The array is stereo and averaged to mono; the jack is
  mono already, one int32 per frame.
- **Scale.** The array arrives at 24-bit scale, the jack as 16-bit
  values. The jack's samples are multiplied by 256 on the way into the
  ring, so SPECTRUM_SAMPLE_FULL_SCALE (2^23) is the full scale of both
  and dBFS means the same thing whichever is showing.

The jack senses a plug, not a microphone. Plain headphones switch it to
the jack and show an empty spectrum labelled "headset jack", which is
what is there. If the headset capture is refused, the task goes back to
the array and says so in the log.

For a few milliseconds after a switch the 1024-sample window holds the
end of one source and the start of the other. That is one or two frames
of a seam, once per plug.
