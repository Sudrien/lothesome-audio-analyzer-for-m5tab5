# Lothesome Audio Analyzer for M5Tab5

The M5Stack Tab5's two built-in microphones as a live spectrum: 1024
samples at 48 kHz, Hamming-windowed, an FFT, and 64 log-spaced bars from
50 Hz to 24 kHz on a dBFS scale, with peak markers. Only the rows that
changed are redrawn each frame.

A C port of the Arduino sketch in
[m5tab5_spectrum_analyzer](https://github.com/Sudrien/m5tab5_spectrum_analyzer),
kept unchanged in `original/` for reference. It runs on plain ESP-IDF
with no M5Unified and no Arduino core, on three libraries pulled out of
[Defeatist Music Player for M5Tab5](https://github.com/Sudrien/defeatist-music-player-for-m5tab5):

| Library | For |
|---|---|
| [feckless-drivers-for-tab5](https://github.com/Sudrien/feckless-drivers-for-tab5) | the I2C bus and IO expanders, the ES7210 microphone capture |
| [feckless-graphics-handler-for-tab5](https://github.com/Sudrien/feckless-graphics-handler-for-tab5) | the panel, the framebuffer, text |
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

Capture sets the ceiling: 1024 samples at 48 kHz take 21.3 ms to arrive,
so about 47 frames a second is the most there can be.

## The scale

0 dBFS is digital full scale. It needs no calibration and is not dB SPL:
turning it into acoustic level needs an acoustic reference, which the
original README's calibration section explains.

## Tuning

`main/spectrum.h` has the numbers: `SPECTRUM_MIN_DBFS` / `MAX_DBFS` for
the vertical range, `ATTACK_TAU` and `RELEASE_TAU` for how fast bars
rise and fall, `PEAK_FALL_PER_SEC`, `SPECTRUM_FFT_SIZE` (512 for more
headroom, 2048 for finer bass), `SPECTRUM_BARS`. `main/analyzer.c` has
the rotation, the backlight and an optional frame cap.

## Tests

```
make -C test
```

`main/spectrum.h` on the host: dBFS scaling against a direct DFT, the
band table, the ballistics' independence from framerate.

## Licence

MIT. See `LICENSE`.
