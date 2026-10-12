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

### 0008 -- a USB microphone

A UAC microphone on the USB-A port, through feckless-drivers' uac.c:
`uac_mic_open()`, `uac_mic_read()`, `uac_mic_gone()`, the path the
player's recorder uses (its 5207). app_main() brings the port up --
usbhost_init(), uac_init() before the port starts, as usbhost.h
requires, then usbhost_start() -- and a failure there leaves the
built-in mics and the jack working.

The capture task prefers USB over the jack over the array. When a mic
is announced it opens it, and only then ends the ES7210 capture; when
the mic is reported gone it closes it and begins the ES7210 again. A
mic the driver refuses (no 16-bit PCM alternate, say) is not retried
until uac_generation() says something was plugged or unplugged.

**The sample rate is the mic's.** uac.c takes 48 kHz if offered, else
44.1, else the highest. spectrum.h gained spectrum_band_at() and
spectrum_freq_x_at(), which take the rate; the bars span 80 Hz to that
rate's Nyquist, and frequency labels above it are not drawn.
spectrum_band() and spectrum_freq_x() are the 48 kHz case, and
spectrumtest checks that they still are, and that the bands stay in
order and reach Nyquist at 16, 22.05, 32, 44.1 and 48 kHz.

When the source changes and the rate with it, the drawing loop rebuilds
the bands and repaints the graph (layout_for_rate()), and every bar
starts again from the floor. The capture task writes the rate and the
label before the source, so the loop never sees a new source with the
old rate.

The window is still 1024 samples, so at a lower rate it is longer --
64 ms at 16 kHz -- and the bins are narrower: 15.6 Hz instead of 46.9.
FRAME_HOP is still 256 samples, which at 16 kHz is 16 ms, so a 16 kHz
mic redraws at no more than about 62 fps.

Samples are 16-bit, one or two channels; two are averaged, and both are
scaled by 256 to the 24-bit full scale, as the jack's are (0007).

The label is "USB <product>", with the rate added when it is not 48 kHz.

The registry's usb_host_uac, not the player's vendored copy with its
descriptor-parser fix (5026), is what this builds against. A headset
that the player handles and this does not is the first thing to
suspect that for.

### 0009 -- a picture in the README

`docs/mockup.png` is the screen as `analyzer.c` draws it, made off the
board: analyzer.c included into a host harness with gfx reduced to a
plain 720 x 1280 buffer and every ESP-IDF and driver call stubbed, the
static layout and the source label drawn, forty frames of a synthetic
spectrum rendered, and the buffer read back through landmap.h into a
1280 x 720 image. The same harness is how 0006's mapping was checked.

So it shows the firmware's layout, text and colours exactly, and a
spectrum that was never measured. The README says so under it. The
harness is not committed: it leans on the player's texttest stubs and
on stub headers written for the occasion, and is more scaffolding than
it is worth keeping. A photo of the board is the better picture, when
there is one.

### 0010 -- the right headset slot, and the program's own name

The first board run with a headset found two things.

**The headset was a slot of silence, and the array half the array.**
Not a wrong slot number here: the capture channel is an I2S slave on
clocks already running, and when it is enabled it can start anywhere in
the four-slot frame. The probe showed slot 0's silence at position 0 on
boot, then at 1 after the USB mic came out, then at 3 after the jack
went in. feckless-drivers v0.3.0 finds that silent slot at each capture
begin and takes every slot relative to it (its patch 0005); this patch
asks for v0.3.0. The cost is about 150 ms after each source switch
before samples arrive, which the drawing loop already waits out.

**The title said "Tab5 Spectrum Analyzer"**, the sketch's. It is
"Lothesome Audio Analyzer" now, and docs/mockup.png is redrawn with it.

And one cosmetic: USB product strings come space-padded ("C-Media USB
Headphone Set  "), so the label and the log line trim them.

### 0011 -- the libraries' new names

feckless-drivers and feckless-graphics-handler are -for-m5tab5 now,
not -for-tab5 (the TODO in b714150). The two git: URLs in
main/idf_component.yml, and the names in README.md and CLAUDE.md,
follow. Component names and tags are unchanged.

### 0012 -- a bar per pixel column

The 64 bars become 1170, one per column of the graph, with no gap
between them. `spectrum_band_n()` takes the bar count, since the count
is the graph's width; `spectrum_band_at()` keeps `SPECTRUM_BARS` for the
tests.

A wide bar is now the largest of its bins, not their mean. With 64 bars
the top bar averaged ~170 bins, so a tone there read some 45 dB under the
same tone in a one-bin bar; the largest reads a tone at its level
anywhere. Noise reads a little higher where a column spans several bins.

The display now shows everything the FFT resolves, and no more: a bin is
46.9 Hz, so below about 9.6 kHz a column is narrower than a bin and is
interpolated between two. The low end's resolution is
`SPECTRUM_FFT_SIZE`'s to give, not the display's.

### 0013 -- down to 20 Hz, on a bass FFT of its own

The axis starts at 20 Hz. A bin of the main FFT is 46.9 Hz, so the
capture task also low-passes each chunk and keeps every 16th sample
(`spectrum_decim()`, a 511-tap Blackman-windowed sinc, only the outputs
computed), into a second ring. Each frame runs the same 1024-point FFT
on that ring's newest window: 3 kHz, 2.9 Hz bins, 341 ms. A band wholly
under 1.2 kHz -- 0.8 of the decimated Nyquist -- reads it
(`spectrum_band_t.bass`).

The filter is flat to 0.05 dB below the split, and what would fold back
below it is at least 70 dB down; a bin-centred tone through it reads its
dBFS on the bass FFT within 0.1 dB. `test/spectrumtest.c` checks all
three. The bass is slower than the treble by its window, which is the
price of the resolution, not a defect.

Each source has a floor, `k_source_floor_hz`: 80 Hz for the array (0004)
and the jack, 20 Hz for USB. Below it the graph is filled grey and its
columns are not drawn, so a deaf region does not read as silence. A
source change now always rebuilds the layout, since the floor can move
without the rate.

### 0014 -- LAeq, LAFmax, LCpeak

A sound level meter in `spectrum.h`, fed by the capture task with every
mono chunk, beside the decimator: on the samples, because LCpeak needs
every one and Fast and Leq are defined on the weighted signal (IEC
61672-1), and an FFT window neither sees every sample nor weights them
evenly.

A and C weighting are biquads from the standard's analog poles by the
bilinear transform, each pole prewarped, the cascade set to 0 dB at
1 kHz: C is two, A three. `test/spectrumtest.c` holds both to class 1 at
44.1 and 48 kHz and to 0.1 dB up to 1 kHz, and runs sines through the
float filters for LAeq, LAFmax, LCpeak, Fast's 34.7 dB/s fall and Leq's
energy average. The transform's bend near Nyquist is the weak point:
+0.85 dB at 8 kHz and -3 dB at 16 kHz at 48 kHz, within class 1 but not
much inside it. A higher-order fit would do better.

Levels are dB re a full-scale sine (AES17), so a full-scale sine reads 0
on all three and agrees with the bars; LAeq sums in double, which stays
exact for days. A sample at 0.999 of full scale sets CLIP.

The capture task is the meter's only writer: it publishes the readings
after each chunk and does a reset the drawing loop asks for. The drawing
loop redraws the line four times a second, and reads touch (feckless-
drivers' `touch.h`, already in v0.3.0) at 20 Hz. A reset is a press held
half a second, not a tap: the player found this panel reporting presses
nobody made (feckless-drivers' `touch.c`, 0503). A source change
re-initialises the meter, since a reading across two gains means
neither.

The graph's top moves from 70 to 104 to make room for the line.

### 0015 -- the waterfall

A second view: the bars as a 140 px strip, and under them a waterfall
of the last 10.5 s, newest at the top, one column per bar column so the
two share the frequency axis. Each column's level is the frame's
unsmoothed one -- the ballistics are for the bars -- through a 256-entry
table of `spectrum_heat_rgb()`, an inferno-like map whose lightness
rises the whole way, so it reads without colour vision.

Rows are time: `spectrum_wf_rows()` owes 40 a second by the clock and
carries the fraction, so a slow frame paints several identical rows and
the time labels stay true at any framerate. `test/spectrumtest.c` checks
both the map and the row count at 7 to 120 fps.

Scrolling is a `memmove` along each panel row: a landscape column is a
panel row (0006), and the waterfall is a contiguous run of it, so one
row down is 1090 moves of 844 bytes in the shadow. A ring was weighed
and dropped. `gfx_blit()` sends whole panel rows and the DPI driver
takes a source exactly as wide as the rectangle, so a ring would have to
be unrolled into the shadow in order every frame -- the same bytes. And
the bars already dirty nearly every column each frame, so the waterfall
adds no blit, only the move.

The graph ends 90 px from the right in waterfall mode, for a colour key
labelled every 30 dB; the strip's dB ticks go to 30 dB steps too. The
two mode buttons take the top right, so the source label stops short of
them and the fps moves under them. Touch is turned to 270 to match
`landmap.h`; a press that lands on a button chooses that mode and
cannot also reset the meter, whose half-second hold still works
anywhere else.

### 0016 -- one-third octaves

A third view, between the bars and the waterfall: the 31 bands of IEC
61260-1, base ten, 20 Hz to 20 kHz, drawn as evenly spaced bars with the
octave centres labelled.

A band is the POWER in it, not the largest bin (0012's rule for the
bars): every bin it covers, weighted by how much of that bin's width is
inside the band, scaled by Parseval to dB re a full-scale sine like the
meter. That is what makes pink noise flat. `test/spectrumtest.c` checks
the centres, a tone's level in a band of the main FFT, and that white
noise rises exactly 1 dB a band within each FFT. Bands wholly under the
1.2 kHz split read the bass FFT, so 1 kHz is a bass band and 1.25 kHz
the first of the main one.

Each band's mean square is time-weighted Fast or Slow (IEC 61672,
125 ms or 1 s) in place of the bars' cosmetic attack and release; the
peak marker holds and sinks as the bars' does. A FAST/SLOW button under
the mode buttons switches it, and the averages carry across the switch.

The mode buttons are three of 112 px, BARS / 1/3 OCT / FALL: "1/3 OCT"
is 98 px of Ark12 at label scale. A band whose top edge is past the
source's Nyquist is drawn grey: the 20 kHz band at 44.1 kHz. A band
whose centre is under the source's floor is greyed like the columns.

`render()`'s bar painting moved into `paint_bar()`, shared by the
columns and the bands, and `repaint_rows()` takes the grey.

### 0017 -- label buffers GCC can bound

0015's time labels and 0016's dB ticks wrote `%d` into 8-byte buffers.
Neither loop has a bound GCC can see -- the time loop ends on a pixel
test, and 0016 made the tick step a runtime value -- so an `-O2` build
inlines `draw_static()` into `layout_for()` and fails on
`-Werror=format-truncation`. The host test never builds analyzer.c, so
nothing here caught it. Both buffers are 16 bytes, room for any int.
