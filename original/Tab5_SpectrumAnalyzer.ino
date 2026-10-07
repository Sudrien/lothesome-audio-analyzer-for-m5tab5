/*
  M5Stack Tab5 — Microphone Spectrum Analyzer
  ===========================================
  Real-time audio spectrum analyzer for the M5Stack Tab5 (ESP32-P4).
  Captures from the onboard dual-mic array, runs a windowed FFT, and
  renders a 64-band logarithmic bar graph on the 1280x720 display.

  FEATURES
    - Full audible range: 48 kHz sampling, 20 Hz - 24 kHz displayed.
    - Logarithmic frequency bands, so bass gets fine resolution and
      treble is grouped, matching how hearing actually works.
    - Sub-bin interpolation at the low end, where a band can be
      narrower than the FFT's own resolution.
    - dB grid with labeled axis, labeled frequency axis, peak-hold
      markers with decay.
    - Attack/release ballistics with real time constants, so the
      display looks identical at any framerate.
    - Delta rendering: only screen rows that actually changed are
      repainted each frame. Runs at ~47 fps, which is the ceiling
      imposed by mic capture time, not by drawing.

  HARDWARE
    M5Stack Tab5 (ESP32-P4 + ESP32-C6, 16 MB flash / 32 MB PSRAM).
    Display is rotated 180 degrees to match the orientation of the
    printing on the back of the case.

  LIBRARIES
    M5Unified  >= 0.2.17
    M5GFX      >= 0.2.23   (ST7121 / ST7123 / ILI9881C auto-detect)
    arduinoFFT >= 2.0      (kosme/arduinoFFT)

  BUILD
    The Tab5 has no dedicated board entry in the Espressif core; the
    generic ESP32-P4 entry works, since M5Unified/M5GFX handle the
    Tab5-specific hardware at runtime. PSRAM must be enabled — the
    display needs it.

      arduino-cli compile \
        --fqbn "esp32:esp32:esp32p4:FlashSize=16M,PSRAM=enabled,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc" .

      arduino-cli upload \
        --fqbn "esp32:esp32:esp32p4:FlashSize=16M,PSRAM=enabled,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc" \
        --port /dev/ttyACM0 .

  TUNING
    MIN_DBFS / MAX_DBFS  vertical range in dBFS. 0 = digital full scale
                         (top); MIN_DBFS is the floor. Lower MIN_DBFS
                         (e.g. -110) to see quieter detail; raise it
                         (e.g. -70) to zoom the loud end.
    ATTACK_TAU           rise speed. Keep short so transients snap.
    RELEASE_TAU          fall speed. Raise if jumpy, lower if mushy.
    PEAK_FALL_PER_SEC    how fast the peak markers sink.
    FFT_SIZE             512 for more headroom, 2048 for finer bass
                         resolution at the cost of framerate.

  The axis is dBFS (0 = full scale), the conventional analyzer scale and
  the one FrequenSee uses. It needs no calibration — it comes from the
  sample format. It is NOT dB SPL; converting to acoustic dB needs a real
  reference, which the (retained) calibration sketches explore and which
  turned out to need a fixture this project does not have. See README.
*/

#include <M5Unified.h>
#include <arduinoFFT.h>

// ---------- Audio / FFT ----------
constexpr int SAMPLE_RATE = 48000;   // Nyquist 24 kHz -> full audible range
constexpr int FFT_SIZE    = 1024;    // power of 2; ~46.9 Hz per bin
constexpr int NUM_BARS    = 64;

// ---------- Frequency axis ----------
constexpr float LOG_MIN_FREQ = 50.0f;
constexpr float LOG_MAX_FREQ = (float)SAMPLE_RATE / 2.0f;

// ---------- Level scaling (dBFS) ----------
// The axis is dBFS: 0 dB = digital full scale (the loudest the ADC can
// represent), everything real below it. This is the same scale FrequenSee
// and most analyzers use, and unlike raw FFT magnitude it needs no
// calibration — it is defined by the sample format alone. It is NOT dB
// SPL; the offset to true acoustic dB depends on mic sensitivity and gain
// and would need a real acoustic reference to find.
//
// MIN_DBFS is the floor of the display (quietest shown); MAX_DBFS is the
// top and should stay at 0 unless you want headroom above full scale.
float MIN_DBFS = -90.0f;
float MAX_DBFS =   0.0f;

// Full-scale reference for the FFT magnitude, so a full-scale sine reads
// 0 dBFS. Three factors:
//   - Sample full scale: 16-bit signed peaks at 32768.
//   - FFT scaling: arduinoFFT's magnitude for a real tone of amplitude A
//     comes out ~ A * N / 2, so divide by N/2.
//   - Window coherent gain: a Hamming window sums to ~0.54 of a rectangular
//     one, attenuating the tone's bin by that factor; divide it back out.
// Combined, a full-scale windowed sine lands at magnitude FULL_SCALE_MAG,
// which maps to 0 dBFS.
constexpr float SAMPLE_FULL_SCALE = 32768.0f;
constexpr float HAMMING_COHERENT_GAIN = 0.54f;
constexpr float FULL_SCALE_MAG =
    SAMPLE_FULL_SCALE * (FFT_SIZE / 2.0f) * HAMMING_COHERENT_GAIN;

// ---------- Ballistics ----------
// Exponential time constants in seconds. Because these are expressed in
// time rather than per-frame steps, behavior is framerate-independent.
constexpr float ATTACK_TAU        = 0.020f;  // ~20 ms rise
constexpr float RELEASE_TAU       = 0.150f;  // ~150 ms fall
constexpr float PEAK_FALL_PER_SEC = 0.55f;   // peak marker sink rate

// Optional frame cap. 0 = unlimited. Set only to reduce power draw.
constexpr int FRAME_LIMIT_HZ = 0;

float   vReal[FFT_SIZE];
float   vImag[FFT_SIZE];
int16_t rawSamples[FFT_SIZE];

ArduinoFFT<float> FFT(vReal, vImag, FFT_SIZE, (float)SAMPLE_RATE);

// ---------- Display layout ----------
int screenW, screenH;
int graphLeft, graphRight, graphTop, graphBottom, graphW, graphH, barW;

constexpr int MAX_GRAPH_H = 800;      // safety bound for the row tables
uint16_t rowColor[MAX_GRAPH_H + 1];   // color per row, indexed by height above baseline
bool     rowIsGrid[MAX_GRAPH_H + 1];  // true where a dB gridline sits

uint16_t COL_BLACK, COL_GRID, COL_PEAK, COL_BASE;

// ---------- Per-bar state ----------
float smoothFrac[NUM_BARS];   // smoothed level 0..1, this is what gets drawn
float barPeaks[NUM_BARS];     // peak-hold 0..1
int   prevBarH[NUM_BARS];     // previous bar height, px
int   prevPeakY[NUM_BARS];    // previous peak marker, screen y

// ---------- Per-bar FFT band, precomputed ----------
bool  bandNarrow[NUM_BARS];   // band spans < 1 bin, so interpolate
float bandCenterBin[NUM_BARS];
int   bandLoBin[NUM_BARS], bandHiBin[NUM_BARS];

// ---------- dB ticks ----------
int dbTickY[24];
int numDbTicks = 0;

// ---------- Timing / FPS ----------
uint32_t lastFrameUs = 0;
uint32_t fpsFrameCount = 0, fpsLastReportMs = 0;
float    currentFps = 0.0f;
uint32_t tCaptureUs = 0, tFftUs = 0, tDrawUs = 0;

// Vertical gradient: green low, yellow mid, red top.
// Color depends on row height, never on a bar's current level — that
// invariant is what makes delta rendering correct.
static uint16_t heightColor(float frac) {
  frac = constrain(frac, 0.0f, 1.0f);
  uint8_t r, g;
  if (frac < 0.5f) { r = (uint8_t)(255 * (frac / 0.5f)); g = 255; }
  else             { r = 255; g = (uint8_t)(255 * (1.0f - (frac - 0.5f) / 0.5f)); }
  return M5.Display.color565(r, g, 0);
}

static String freqLabel(float hz) {
  char buf[8];
  if (hz >= 1000.0f) {
    float k = hz / 1000.0f;
    if (k >= 10.0f) snprintf(buf, sizeof(buf), "%.0fk", k);
    else            snprintf(buf, sizeof(buf), "%.1fk", k);
    return String(buf);
  }
  return String((int)hz);
}

static int freqToX(float hz) {
  hz = constrain(hz, LOG_MIN_FREQ, LOG_MAX_FREQ);
  float t = logf(hz / LOG_MIN_FREQ) / logf(LOG_MAX_FREQ / LOG_MIN_FREQ);
  return (int)(t * graphW);
}

void setup() {
  Serial.begin(115200);
  auto cfg = M5.config();
  M5.begin(cfg);

  auto mic_cfg = M5.Mic.config();
  mic_cfg.sample_rate = SAMPLE_RATE;
  mic_cfg.stereo      = false;
  M5.Mic.config(mic_cfg);
  M5.Mic.begin();

  M5.Display.setRotation(3);   // landscape, flipped to match case printing
  screenW = M5.Display.width();
  screenH = M5.Display.height();

  graphLeft   = 100;           // margin for the dB axis labels
  graphRight  = screenW - 10;
  graphTop    = 70;            // clearance below the title
  graphBottom = screenH - 46;  // room for the frequency labels
  graphW = graphRight - graphLeft;
  graphH = graphBottom - graphTop;
  if (graphH > MAX_GRAPH_H) graphH = MAX_GRAPH_H;
  graphBottom = graphTop + graphH;
  barW = graphW / NUM_BARS;

  COL_BLACK = TFT_BLACK;
  COL_GRID  = M5.Display.color565(45, 45, 45);
  COL_PEAK  = TFT_WHITE;
  COL_BASE  = TFT_DARKGREY;

  M5.Display.fillScreen(COL_BLACK);

  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextSize(4);
  M5.Display.drawString("Tab5 Spectrum Analyzer", 10, 4);

  // Row lookup tables.
  for (int h = 0; h <= graphH; h++) {
    rowColor[h]  = heightColor((float)h / (float)graphH);
    rowIsGrid[h] = false;
  }

  // dBFS ticks: flag gridline rows and draw the left-hand labels. Ticks
  // run from MIN_DBFS up to MAX_DBFS in 10 dB steps, so labels read like
  // -90, -80, ... 0 up the axis.
  numDbTicks = 0;
  int firstTick = ((int)MIN_DBFS / 10) * 10;
  if (firstTick < MIN_DBFS) firstTick += 10;
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_LIGHTGREY);
  for (int db = firstTick; db <= (int)MAX_DBFS && numDbTicks < 24; db += 10) {
    float frac = (db - MIN_DBFS) / (MAX_DBFS - MIN_DBFS);
    int h = (int)(frac * graphH);
    if (h < 0 || h > graphH) continue;
    rowIsGrid[h] = true;
    int y = graphBottom - h;
    dbTickY[numDbTicks++] = y;

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", db);   // e.g. -60; unit shown in axis title
    int tw = M5.Display.textWidth(buf);
    M5.Display.setCursor(graphLeft - tw - 8, y - 8);
    M5.Display.print(buf);
  }

  // dBFS unit marker at the top of the axis, so the negative numbers read
  // clearly as dB below full scale.
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_LIGHTGREY);
  M5.Display.setCursor(10, graphTop - 22);
  M5.Display.print("dBFS");

  // Frequency labels, positioned along the log axis.
  M5.Display.setTextSize(3);
  M5.Display.setTextColor(TFT_LIGHTGREY);
  static const float labelFreqs[] = {50, 100, 500, 1000, 5000, 10000, 20000};
  for (float f : labelFreqs) {
    if (f < LOG_MIN_FREQ || f > LOG_MAX_FREQ) continue;
    int x = constrain(freqToX(f), 0, graphW - 60);
    M5.Display.setCursor(graphLeft + x, graphBottom + 10);
    M5.Display.print(freqLabel(f));
  }

  // Empty graph background, painted once.
  for (int i = 0; i < numDbTicks; i++)
    M5.Display.drawFastHLine(graphLeft, dbTickY[i], graphW, COL_GRID);
  M5.Display.drawFastHLine(graphLeft, graphBottom, graphW, COL_BASE);

  // Precompute each bar's FFT bin range, keeping powf() out of the hot loop.
  const float hzPerBin   = (float)SAMPLE_RATE / (float)FFT_SIZE;
  const int   usableBins = FFT_SIZE / 2;
  const float ratio      = LOG_MAX_FREQ / LOG_MIN_FREQ;
  for (int b = 0; b < NUM_BARS; b++) {
    float loF = LOG_MIN_FREQ * powf(ratio, (float)b       / NUM_BARS);
    float hiF = LOG_MIN_FREQ * powf(ratio, (float)(b + 1) / NUM_BARS);
    float loB = loF / hzPerBin;
    float hiB = hiF / hzPerBin;

    if (hiB - loB < 1.0f) {
      // Narrower than the FFT can resolve: interpolate between neighbors
      // so adjacent low-frequency bars don't read the identical bin.
      bandNarrow[b]    = true;
      bandCenterBin[b] = constrain((loB + hiB) * 0.5f, 1.0f, (float)(usableBins - 2));
    } else {
      bandNarrow[b] = false;
      bandLoBin[b]  = max(1, (int)loB);
      bandHiBin[b]  = min(usableBins - 1, (int)hiB);
      if (bandHiBin[b] < bandLoBin[b]) bandHiBin[b] = bandLoBin[b];
    }
  }

  for (int b = 0; b < NUM_BARS; b++) {
    smoothFrac[b] = 0.0f;
    barPeaks[b]   = 0.0f;
    prevBarH[b]   = 0;
    prevPeakY[b]  = graphBottom;
  }

  fpsLastReportMs = millis();
  lastFrameUs     = micros();
}

// Repaint rows [yTop..yBot] of one bar column with correct current content.
// Called only on rows that actually changed since the last frame.
static inline void repaintRows(int xs, int w, int yTop, int yBot,
                               int barTopY, int peakY) {
  if (yTop < graphTop)        yTop = graphTop;
  if (yBot > graphBottom - 1) yBot = graphBottom - 1;
  for (int y = yTop; y <= yBot; y++) {
    uint16_t c;
    if (y == peakY)                      c = COL_PEAK;
    else if (y >= barTopY)               c = rowColor[graphBottom - y];
    else if (rowIsGrid[graphBottom - y]) c = COL_GRID;
    else                                 c = COL_BLACK;
    M5.Display.drawFastHLine(xs, y, w, c);
  }
}

void loop() {
  M5.update();

  // ---------- capture ----------
  uint32_t t0 = micros();
  if (!M5.Mic.record(rawSamples, FFT_SIZE, SAMPLE_RATE)) { delay(2); return; }
  uint32_t t1 = micros();

  // ---------- FFT ----------
  for (int i = 0; i < FFT_SIZE; i++) { vReal[i] = (float)rawSamples[i]; vImag[i] = 0.0f; }
  FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();
  uint32_t t2 = micros();

  // ---------- frame delta, for framerate-independent ballistics ----------
  uint32_t nowUs = micros();
  float dt = (nowUs - lastFrameUs) * 1e-6f;
  lastFrameUs = nowUs;
  dt = constrain(dt, 0.0005f, 0.25f);   // guard against stalls and first frame

  const float aAttack  = 1.0f - expf(-dt / ATTACK_TAU);
  const float aRelease = 1.0f - expf(-dt / RELEASE_TAU);
  const float peakDrop = PEAK_FALL_PER_SEC * dt;

  // ---------- render ----------
  const float dbSpan = MAX_DBFS - MIN_DBFS;
  M5.Display.startWrite();

  for (int b = 0; b < NUM_BARS; b++) {
    float mag;
    if (bandNarrow[b]) {
      float f  = bandCenterBin[b];
      int   lo = (int)f;
      float t  = f - lo;
      mag = vReal[lo] * (1.0f - t) + vReal[lo + 1] * t;
    } else {
      float sum = 0.0f;
      for (int i = bandLoBin[b]; i <= bandHiBin[b]; i++) sum += vReal[i];
      mag = sum / (float)(bandHiBin[b] - bandLoBin[b] + 1);
    }

    float db  = 20.0f * log10f(mag / FULL_SCALE_MAG + 1e-9f);  // dBFS, 0 = full scale
    float raw = constrain((db - MIN_DBFS) / dbSpan, 0.0f, 1.0f);

    // Rise quickly, fall gently.
    float s = smoothFrac[b];
    s += (raw - s) * (raw > s ? aAttack : aRelease);
    smoothFrac[b] = s;

    if (s > barPeaks[b]) barPeaks[b] = s;
    else                 barPeaks[b] = max(0.0f, barPeaks[b] - peakDrop);

    int newH    = (int)(s * graphH);
    int barTopY = graphBottom - newH;
    int peakY   = graphBottom - (int)(barPeaks[b] * graphH);
    if (peakY < graphTop)        peakY = graphTop;
    if (peakY > graphBottom - 1) peakY = graphBottom - 1;

    int oldTopY  = graphBottom - prevBarH[b];
    int oldPeakY = prevPeakY[b];

    // Only rows spanned by the old and new state can have changed.
    int yTop = min(min(barTopY, oldTopY), min(peakY, oldPeakY));
    int yBot = max(max(barTopY, oldTopY), max(peakY, oldPeakY));

    if (yTop <= yBot)
      repaintRows(graphLeft + b * barW + 1, barW - 2, yTop, yBot, barTopY, peakY);

    prevBarH[b]  = newH;
    prevPeakY[b] = peakY;
  }

  M5.Display.endWrite();
  uint32_t t3 = micros();

  tCaptureUs += (t1 - t0);
  tFftUs     += (t2 - t1);
  tDrawUs    += (t3 - t2);
  fpsFrameCount++;

  // ---------- FPS and stage timings, once per second ----------
  uint32_t nowMs = millis();
  if (nowMs - fpsLastReportMs >= 1000) {
    uint32_t n = fpsFrameCount ? fpsFrameCount : 1;
    currentFps = fpsFrameCount * 1000.0f / (nowMs - fpsLastReportMs);
    Serial.printf("FPS: %.1f | capture %.1fms  fft %.1fms  draw %.1fms\n",
                  currentFps, tCaptureUs / 1000.0f / n,
                  tFftUs / 1000.0f / n, tDrawUs / 1000.0f / n);

    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_WHITE, COL_BLACK);
    char buf[16];
    snprintf(buf, sizeof(buf), "%5.1f fps", currentFps);
    M5.Display.setCursor(screenW - 170, 14);
    M5.Display.print(buf);

    fpsFrameCount = 0;
    fpsLastReportMs = nowMs;
    tCaptureUs = tFftUs = tDrawUs = 0;
  }

  // ---------- optional frame cap ----------
  if (FRAME_LIMIT_HZ > 0) {
    const uint32_t targetUs = 1000000UL / FRAME_LIMIT_HZ;
    uint32_t spentUs = micros() - nowUs;
    if (spentUs < targetUs) delayMicroseconds(targetUs - spentUs);
  }
}
