# Lothesome Audio Analyzer *for M5Tab5*

The M5Stack Tab5's two built-in microphones as a live spectrum: 1024
samples at 48 kHz, Hamming-windowed, an FFT, and a log-spaced spectrum from
20 Hz to 24 kHz, a bar per pixel column, on a dBFS scale, with peak markers. Only the rows that
changed are redrawn each frame.

![The analyzer's screen, rendered on a host](docs/mockup.png)

*Not a photo: `analyzer.c`'s drawing code run on a PC against a stand-in
for the display, with a made-up spectrum, and read back through the same
landscape mapping the board uses. The layout, labels and colours are
the firmware's; the bars are not a real measurement.*

A C port of the Arduino sketch in
[m5tab5_spectrum_analyzer](https://github.com/Sudrien/m5tab5_spectrum_analyzer),
kept unchanged in `original/` for reference. It runs on plain ESP-IDF
with no M5Unified and no Arduino core, on three libraries pulled out of
[Defeatist Music Player for M5Tab5](https://github.com/Sudrien/defeatist-music-player-for-m5tab5):

| Library | For |
|---|---|
| [feckless-drivers-for-m5tab5](https://github.com/Sudrien/feckless-drivers-for-m5tab5) | the I2C bus and IO expanders, the ES7210 microphone capture |
| [feckless-graphics-handler-for-m5tab5](https://github.com/Sudrien/feckless-graphics-handler-for-m5tab5) | the panel, the framebuffer, text |
| [esp-dsp](https://components.espressif.com/components/espressif/esp-dsp) | the FFT |

## Building

ESP-IDF 5.5 or later:

```
idf.py set-target esp32p4
idf.py build flash monitor
```

Rev 2 Tabs (ST7121 panel) only; see feckless-graphics-handler.

Once a second the log prints the framerate and where each frame's time
went, as `FPS: ... | capture ...ms  fft ...ms  draw ...ms`.

The microphones are read by a task of their own into a ring, and each
frame takes the newest 1024 samples from it, so drawing sets the
framerate and the window is always the latest 21.3 ms of sound.

## Where the sound comes from

In order of preference:

1. **A USB microphone** on the USB-A port -- a UAC headset or a USB mic.
   It runs at its own sample rate, and the axis runs to that rate's
   Nyquist: a 16 kHz headset shows 20 Hz to 8 kHz.
2. **A headset on the 3.5 mm jack.**
3. **The two built-in microphones.**

The source is named at the top of the screen. The jack senses a plug,
not a microphone, so plain headphones give an empty spectrum. Plug in
directly: full-speed USB audio devices do not work behind a hub on the
Tab5's port.

## The bass

Below 1.2 kHz the spectrum comes from a second FFT of the sound
low-passed and decimated to 3 kHz: 2.9 Hz bins instead of 46.9, over a
341 ms window, so the bass is finer and slower than the treble. The
part of the axis a source cannot hear is greyed: below 80 Hz for the
built-in microphones and the jack, nothing for a USB microphone.

## The views

Three buttons, top right: **BARS** (a bar per pixel column), **1/3 OCT**
and **FALL**.

**1/3 OCT** is a real-time analyzer: the 31 standard bands from 20 Hz to
20 kHz, each the power in its band, so pink noise reads flat when the
sound is flat. **FAST/SLOW** under the buttons picks the time weighting,
125 ms or 1 s; Slow is the steadier one for reading pink noise. The
20 kHz band is grey when the source's Nyquist cuts it short (a 44.1 kHz
microphone), and the bottom two bands are coarse: 20 and 25 Hz bands
are narrower than two bins of the bass FFT.

## The waterfall

**FALL**, shrinks the bars to a strip and scrolls the
last ten seconds down under them, newest at the top, on the same
frequency axis: 40 rows a second, coloured black through purple and
orange to pale yellow by level. Left of 1.2 kHz the picture comes from the bass FFT and is
smeared over a third of a second, which is the price of its resolution.

## The meter

Under the title, on every sample rather than the FFT: **LAeq** (A
weighted, the energy average since the last reset), **LAFmax** (the
highest A-weighted Fast reading, 125 ms) and **LCpeak** (the highest C
weighted sample), with the time since the reset. **Hold a finger on the
screen for half a second to reset**; a tap does not, so a stray touch
cannot wipe a long measurement. Changing source resets it too. **CLIP**
means a sample reached full scale, so the readings are low.

The levels are dB re a full-scale sine, which reads 0 on all three --
not dB SPL (below). The weighting is within IEC 61672-1 class 1 limits
at 44.1 and 48 kHz, but runs up to 0.85 dB high around 8 kHz and 3 dB
low at 16 kHz; below 32 kHz the top of it is approximate.

## The scale

0 dBFS is digital full scale. It needs no calibration and is not dB SPL:
turning it into acoustic level needs an acoustic reference, which the
original README's calibration section explains.

## Tuning

`main/spectrum.h` has the numbers: `SPECTRUM_MIN_DBFS` / `MAX_DBFS` for
the vertical range, `ATTACK_TAU` and `RELEASE_TAU` for how fast bars
rise and fall, `PEAK_FALL_PER_SEC`, `SPECTRUM_FFT_SIZE` (512 for more
headroom, 2048 for finer bass). `main/analyzer.c` has
the rotation, the backlight and an optional frame cap.

## Tests

```
make -C test
```

`main/spectrum.h` on the host: dBFS scaling against a direct DFT, the
band table, the ballistics' independence from framerate.

## Pictures without a board

```
make -C test/render
```

Runs `main/analyzer.c`'s own drawing on a PC, against stand-ins for the
hardware, on ten seconds of a made-up signal, and writes each view as a
picture of the screen (`bars.ppm`, `oct.ppm`, `fall.ppm`, and the same
for the built-in microphones). For checking layout before flashing; the
font comes from `managed_components`, so it needs one `idf.py build`
first.

## Licence

MIT. See `LICENSE`.
