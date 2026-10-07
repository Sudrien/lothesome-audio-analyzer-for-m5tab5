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
