#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_timer.h>

// RHYTHM READER: read one bar of whole / half / quarter / eighth notes, then play it on the
// button after a 4-click count-in. The buzzer is the metronome. Press on each note and hold
// half and whole notes for their length; you get a score for how close you were.

const int BUTTON_PIN = 4;   // other leg to GND
const int BUZZER_PIN = 23;  // passive buzzer

const int SCREEN_W = 128, SCREEN_H = 64;
const int FRAME_MS = 33;
const int LINE_Y = 40;  // the single rhythm-staff line
const int X0 = 24;      // x of the first eighth slot
const int SLOT = 12;    // pixels per eighth; spacing is proportional to time
const int MAX_LEVEL = 8;
const int PASS_PCT = 80;
const int CLICKS = 9;  // 4 count-in + 4 in the bar + the closing downbeat
const int MAX_PRESS = 24;
const int PERFECT_MS = 35;

#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);

// ---------------- Sprites ----------------

const char *const HEAD_FILLED[] = {"..####.", ".######", "#######", "######.", ".####.."};
const char *const HEAD_HOLLOW[] = {"..####.", ".#...##", "#.....#", "##...#.", ".####.."};
const char *const HEAD_WHOLE[] = {"..#####..", ".##...##.", "##.....##", ".##...##.", "..#####.."};
const char *const FLAG[] = {"#..", "##.", ".##", "..#", "..#", ".#."};

// ---------------- Sound ----------------

struct Note {
  uint16_t f, ms;
};

const Note SFX_PERFECT[] = {{1047, 80}, {1319, 80}, {1568, 80}, {2093, 240}};
const Note SFX_GOOD[] = {{1047, 80}, {1568, 160}};
const Note SFX_BAD[] = {{392, 120}, {330, 220}};
const Note SFX_LEVEL[] = {{784, 70}, {1047, 70}, {1319, 70}, {1568, 70}, {2093, 220}};

Note sfx[8];
int sfxLen, sfxIdx;
uint32_t sfxT;

void play(const Note *s, int len) {
  memcpy(sfx, s, len * sizeof(Note));
  sfxLen = len;
  sfxIdx = 0;
  sfxT = millis();
  ledcWriteTone(BUZZER_PIN, sfx[0].f);
}

#define SFX(s) play(s, COUNT(s))

void updateSound() {
  if (sfxIdx >= sfxLen || millis() - sfxT < sfx[sfxIdx].ms) return;
  sfxT += sfx[sfxIdx].ms;
  sfxIdx++;
  ledcWriteTone(BUZZER_PIN, sfxIdx < sfxLen ? sfx[sfxIdx].f : 0);
}

// The metronome runs on esp_timer so the clicks stay steady while the display is busy.
esp_timer_handle_t metroTimer, clickOffTimer;
volatile int clickN;

void clickOff(void *) { ledcWriteTone(BUZZER_PIN, 0); }

void metroTick(void *) {
  int n = clickN++;
  if (n >= CLICKS - 1) esp_timer_stop(metroTimer);
  ledcWriteTone(BUZZER_PIN, n % 4 == 0 ? 2093 : 1047);
  esp_timer_start_once(clickOffTimer, 30000);
}

// ---------------- Button ----------------

// Edges are timestamped in the ISR: display updates block the loop for ~25 ms.
volatile uint32_t edgeT[32];
volatile bool edgeDown[32];
volatile uint8_t edgeHead;
uint8_t edgeTail;
volatile bool isrDown;
volatile uint32_t isrT;
portMUX_TYPE btnMux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR pushEdge(uint32_t t, bool down) {
  isrDown = down;
  isrT = t;
  edgeT[edgeHead & 31] = t;
  edgeDown[edgeHead & 31] = down;
  edgeHead++;
}

void IRAM_ATTR onButton() {
  uint32_t t = millis();
  bool down = digitalRead(BUTTON_PIN) == LOW;
  portENTER_CRITICAL_ISR(&btnMux);
  if (down != isrDown && t - isrT >= 15) pushEdge(t, down);
  portEXIT_CRITICAL_ISR(&btnMux);
}

// ---------------- Game state ----------------

enum State { TITLE, HELP, READ, RUN, RESULT };
State state = TITLE;
uint32_t frame;
int stateT;

struct RNote {
  uint8_t start, dur;  // in eighths
  bool beam;           // beamed to the next note
};
RNote notes[8];
int nNotes;
uint8_t prevSig;
bool hasEighths;

int level = 1, fails;
bool leveledUp;

int32_t beatUs;
float eighthMs;
int barLen, capMs;
uint32_t barStart;

int nPress;
int32_t pressT[MAX_PRESS], relT[MAX_PRESS];  // ms relative to barStart
bool holding;

float noteScore[8];
int pct, hits, avgErr;

int bpm() { return 60 + 10 * level; }
int noteX(int slot) { return X0 + slot * SLOT; }
int msX(int32_t ms) { return constrain(X0 + 3 + lroundf(ms / eighthMs * SLOT), 0, SCREEN_W - 1); }

void setState(State s) {
  state = s;
  stateT = 0;
}

// ---------------- Patterns ----------------

void addNote(int &p, int dur, bool beam) {
  notes[nNotes++] = {(uint8_t)p, (uint8_t)dur, beam};
  p += dur;
}

void newPattern() {
  uint8_t sig;
  do {
    nNotes = 0;
    int p = 0;
    while (p < 8) {
      int r = 8 - p;
      int wW = p == 0 && level != 2 ? 1 : 0;
      int wH = r >= 4 ? 3 : 0;
      int wQ = 4;
      int wE = level >= 2 ? 2 + level / 2 : 0;
      int wS = level >= 4 && r >= 4 ? 2 : 0;  // eighth-quarter-eighth syncopation
      int roll = random(wW + wH + wQ + wE + wS);
      if ((roll -= wW) < 0) {
        addNote(p, 8, false);
      } else if ((roll -= wH) < 0) {
        addNote(p, 4, false);
      } else if ((roll -= wQ) < 0) {
        addNote(p, 2, false);
      } else if ((roll -= wE) < 0) {
        addNote(p, 1, true);
        addNote(p, 1, false);
      } else {
        addNote(p, 1, false);
        addNote(p, 2, false);
        addNote(p, 1, false);
      }
    }
    // With no rests the onsets fully describe the bar.
    sig = 0;
    for (int i = 0; i < nNotes; i++) sig |= 1 << notes[i].start;
  } while (sig == prevSig);
  prevSig = sig;
  hasEighths = false;
  for (int i = 0; i < nNotes; i++) hasEighths |= notes[i].dur == 1;
}

// ---------------- Playing ----------------

void startRun() {
  sfxLen = 0;
  ledcWriteTone(BUZZER_PIN, 0);
  beatUs = 60000000 / bpm();
  eighthMs = beatUs / 2000.0f;
  barLen = lroundf(8 * eighthMs);
  capMs = min((int)eighthMs, 250);
  nPress = 0;
  holding = false;
  clickN = 0;
  int64_t t0 = esp_timer_get_time();
  barStart = (uint32_t)((t0 + 5LL * beatUs) / 1000);  // first click is one beat after t0
  esp_timer_start_periodic(metroTimer, beatUs);
  setState(RUN);
}

void recordEdge(uint32_t t, bool down) {
  int32_t r = (int32_t)(t - barStart);
  if (down) {
    if (r < -capMs || r >= barLen || nPress >= MAX_PRESS) return;
    pressT[nPress] = r;
    relT[nPress] = r;
    nPress++;
    holding = true;
  } else if (holding) {
    relT[nPress - 1] = r;
    holding = false;
  }
}

bool readButton() {
  bool pressed = false;
  // Catch a release the ISR skipped during its debounce window.
  bool down = digitalRead(BUTTON_PIN) == LOW;
  uint32_t now = millis();
  portENTER_CRITICAL(&btnMux);
  if (down != isrDown && now - isrT >= 30) pushEdge(now, down);
  portEXIT_CRITICAL(&btnMux);

  while (edgeTail != edgeHead) {
    uint32_t t = edgeT[edgeTail & 31];
    bool d = edgeDown[edgeTail & 31];
    edgeTail++;
    if (state == RUN) recordEdge(t, d);
    else if (d) pressed = true;
  }
  return pressed;
}

float onsetScore(int err) {
  if (err <= PERFECT_MS) return 1;
  if (err >= capMs) return 0;
  return 1 - float(err - PERFECT_MS) / (capMs - PERFECT_MS);
}

float holdScore(float ratio) {
  if (ratio < 0.7f) return max(0.0f, (ratio - 0.2f) / 0.5f);
  if (ratio > 1.25f) return max(0.0f, 1 - (ratio - 1.25f) / 0.5f);
  return 1;
}

void scoreRound() {
  int best[8], bestErr[8];
  for (int j = 0; j < nNotes; j++) best[j] = -1;
  int extras = 0;
  for (int k = 0; k < nPress; k++) {
    int j = 0, err = 1 << 30;
    for (int i = 0; i < nNotes; i++) {
      int e = abs(pressT[k] - lroundf(notes[i].start * eighthMs));
      if (e < err) err = e, j = i;
    }
    if (err > capMs) {
      extras++;
    } else if (best[j] < 0 || err < bestErr[j]) {
      if (best[j] >= 0) extras++;
      best[j] = k;
      bestErr[j] = err;
    } else {
      extras++;
    }
  }

  float total = 0;
  int sumErr = 0;
  hits = 0;
  for (int j = 0; j < nNotes; j++) {
    noteScore[j] = 0;
    int k = best[j];
    if (k < 0) continue;
    hits++;
    sumErr += pressT[k] - lroundf(notes[j].start * eighthMs);
    noteScore[j] = onsetScore(bestErr[j]);
    if (notes[j].dur >= 4) {
      float ratio = (relT[k] - pressT[k]) / (notes[j].dur * eighthMs);
      noteScore[j] = 0.6f * noteScore[j] + 0.4f * holdScore(ratio);
    }
    total += noteScore[j];
  }
  pct = lroundf(100 * total / (nNotes + 0.5f * extras));
  avgErr = hits ? sumErr / hits : 0;
}

void finishRun(int32_t rel) {
  esp_timer_stop(metroTimer);
  if (holding) relT[nPress - 1] = rel;
  holding = false;
  scoreRound();
  if (pct >= 95) SFX(SFX_PERFECT);
  else if (pct >= PASS_PCT) SFX(SFX_GOOD);
  else SFX(SFX_BAD);
  setState(RESULT);
}

void nextRound() {
  leveledUp = false;
  if (pct >= PASS_PCT) {
    fails = 0;
    if (level < MAX_LEVEL) {
      level++;
      leveledUp = true;
      SFX(SFX_LEVEL);
    }
    newPattern();
  } else if (++fails >= 3) {
    fails = 0;
    newPattern();
  }
  setState(READ);
}

// ---------------- Drawing ----------------

void drawSprite(const char *const *rows, int n, int x, int y) {
  for (int r = 0; r < n; r++)
    for (int c = 0; rows[r][c]; c++)
      if (rows[r][c] == '#') oled.drawPixel(x + c, y + r, WHITE);
}

void drawNoteAt(int x, int y, int dur, bool flag) {
  if (dur >= 8) {
    drawSprite(HEAD_WHOLE, COUNT(HEAD_WHOLE), x - 1, y - 2);
    return;
  }
  drawSprite(dur >= 4 ? HEAD_HOLLOW : HEAD_FILLED, 5, x, y - 2);
  oled.drawFastVLine(x + 6, y - 17, 17, WHITE);
  if (flag) drawSprite(FLAG, COUNT(FLAG), x + 7, y - 17);
}

void drawBeam(int x1, int x2, int y) { oled.fillRect(x1 + 6, y - 17, x2 - x1 + 1, 3, WHITE); }

void drawStaff(int y) {
  oled.drawFastHLine(0, y, 127, WHITE);
  oled.fillRect(3, y - 6, 2, 13, WHITE);  // percussion clef
  oled.fillRect(7, y - 6, 2, 13, WHITE);
  oled.setCursor(13, y - 8);
  oled.print('4');
  oled.setCursor(13, y + 2);
  oled.print('4');
  oled.drawFastVLine(121, y - 8, 17, WHITE);  // final bar line
  oled.fillRect(124, y - 8, 3, 17, WHITE);
}

void drawPattern() {
  drawStaff(LINE_Y);
  for (int i = 0; i < nNotes; i++) {
    int x = noteX(notes[i].start);
    bool beamed = notes[i].beam || (i > 0 && notes[i - 1].beam);
    drawNoteAt(x, LINE_Y, notes[i].dur, notes[i].dur == 1 && !beamed);
    if (notes[i].beam) drawBeam(x, noteX(notes[i + 1].start), LINE_Y);
  }
}

void drawCounts() {
  for (int k = 0; k < 8; k++) {
    if (k & 1 && !hasEighths) continue;
    oled.setCursor(noteX(k) + 1, 46);
    oled.print(k & 1 ? '&' : (char)('1' + k / 2));
  }
}

void drawPresses(int y, int32_t now) {
  for (int k = 0; k < nPress; k++) {
    int x1 = msX(pressT[k]);
    int x2 = msX(holding && k == nPress - 1 ? now : relT[k]);
    oled.fillRect(x1, y, max(2, x2 - x1), 3, WHITE);
  }
}

void printCentered(const char *s, int y) {
  oled.setCursor(64 - strlen(s) * 3, y);
  oled.print(s);
}

void printRight(const char *s, int y) {
  oled.setCursor(SCREEN_W - strlen(s) * 6, y);
  oled.print(s);
}

void drawBigDigit(int d) {
  oled.setTextSize(2);
  oled.setCursor(116, 0);
  oled.print(d);
  oled.setTextSize(1);
}

void drawTitle() {
  oled.setTextSize(2);
  oled.setCursor(28, 2);
  oled.print("RHYTHM");
  oled.setTextSize(1);
  const int y = 44;
  drawStaff(y);
  drawNoteAt(noteX(0), y, 2, false);
  drawNoteAt(noteX(2), y, 1, false);
  drawNoteAt(noteX(3), y, 1, false);
  drawBeam(noteX(2), noteX(3), y);
  drawNoteAt(noteX(4), y, 4, false);
  if ((frame / 12) & 1) printCentered("PRESS TO START", 56);
}

void drawHelp() {
  printCentered("HOW TO PLAY", 0);
  oled.drawFastHLine(31, 9, 66, WHITE);
  printCentered("READ THE RHYTHM", 13);
  printCentered("LISTEN TO 4 CLICKS", 22);
  printCentered("PRESS ON EACH NOTE", 31);
  printCentered("HOLD HALF & WHOLE", 40);
  if ((frame / 12) & 1) printCentered("PRESS TO START", 55);
}

void drawRead() {
  char buf[22];
  if (!leveledUp || stateT > 90 || (stateT / 8) & 1) {
    oled.setCursor(0, 0);
    if (leveledUp && stateT <= 90) oled.print("LEVEL UP!");
    else oled.printf("LEVEL %d", level);
  }
  snprintf(buf, sizeof(buf), "%d BPM", bpm());
  printRight(buf, 0);
  drawPattern();
  drawCounts();
  if ((frame / 12) & 1) printCentered(fails ? "TRY AGAIN: PRESS" : "PRESS TO PLAY", 56);
}

void drawRun(int32_t rel) {
  int beatMs = beatUs / 1000;
  oled.setCursor(0, 0);
  if (rel < 0) {
    oled.print("COUNT-IN");
    int idx = (rel + 4 * beatMs) / beatMs;
    if (rel + 4 * beatMs >= 0) drawBigDigit(idx + 1);
  } else {
    oled.print("PLAY!");
    if (rel < barLen) drawBigDigit(rel / beatMs + 1);
  }
  drawPattern();
  drawCounts();
  if (rel >= 0 && rel <= barLen)
    for (int y = 20; y < 56; y += 3) oled.drawPixel(msX(rel), y, WHITE);
  drawPresses(57, rel);
}

const char *rating() {
  if (pct >= 95) return "PERFECT!";
  if (pct >= 85) return "GREAT!";
  if (pct >= 70) return "GOOD";
  if (pct >= 50) return "CLOSE";
  return "TRY AGAIN";
}

void drawResult() {
  char buf[22];
  oled.setCursor(0, 0);
  oled.printf("%d%% %s", pct, rating());
  snprintf(buf, sizeof(buf), "LV%d", level);
  printRight(buf, 0);

  for (int i = 0; i < nNotes; i++) {
    int cx = noteX(notes[i].start) + 3;
    if (noteScore[i] >= 0.9f) {
      oled.fillCircle(cx, 13, 2, WHITE);
    } else if (noteScore[i] >= 0.5f) {
      oled.drawCircle(cx, 13, 2, WHITE);
    } else {
      oled.drawLine(cx - 2, 11, cx + 2, 15, WHITE);
      oled.drawLine(cx - 2, 15, cx + 2, 11, WHITE);
    }
  }
  drawPattern();

  // Target note lengths above, your presses below.
  for (int i = 0; i < nNotes; i++) {
    int x1 = msX(lroundf(notes[i].start * eighthMs));
    int x2 = msX(lroundf((notes[i].start + notes[i].dur) * eighthMs));
    oled.drawFastVLine(x1, 45, 5, WHITE);
    oled.drawFastHLine(x1, 47, x2 - x1 - 2, WHITE);
  }
  drawPresses(51, 0);

  oled.setCursor(0, 56);
  if (!hits) oled.print("NO NOTES HIT");
  else if (abs(avgErr) <= 20) oled.print("ON TIME!");
  else oled.printf("%dms %s", abs(avgErr), avgErr < 0 ? "EARLY" : "LATE");
  snprintf(buf, sizeof(buf), "%d/%d", hits, nNotes);
  printRight(buf, 56);
}

// ---------------- Main ----------------

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  isrDown = digitalRead(BUTTON_PIN) == LOW;
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), onButton, CHANGE);
  ledcAttach(BUZZER_PIN, 2000, 10);
  ledcWriteTone(BUZZER_PIN, 0);

  esp_timer_create_args_t args = {};
  args.callback = metroTick;
  args.name = "metro";
  esp_timer_create(&args, &metroTimer);
  args.callback = clickOff;
  args.name = "click";
  esp_timer_create(&args, &clickOffTimer);

  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed");
    for (;;) delay(1000);
  }
  oled.setTextColor(WHITE);
  oled.setTextWrap(false);
  randomSeed(esp_random());
  Serial.println("RHYTHM ready");
}

void loop() {
  static uint32_t nextFrame = 0;
  updateSound();
  bool pressed = readButton();

  uint32_t now = millis();
  if ((int32_t)(now - nextFrame) < 0) {
    delay(1);
    return;
  }
  nextFrame = now - nextFrame > 100 ? now + FRAME_MS : nextFrame + FRAME_MS;
  frame++;
  stateT++;

  oled.clearDisplay();
  switch (state) {
    case TITLE:
      drawTitle();
      if (pressed) setState(HELP);
      break;
    case HELP:
      drawHelp();
      if (pressed && stateT > 10) {
        newPattern();
        setState(READ);
      }
      break;
    case READ:
      drawRead();
      if (pressed && stateT > 10) startRun();
      break;
    case RUN: {
      int32_t rel = (int32_t)(now - barStart);
      if (rel < barLen + (int)(beatUs / 2000)) {
        drawRun(rel);
      } else {
        finishRun(rel);
        drawResult();
      }
      break;
    }
    case RESULT:
      drawResult();
      if (pressed && stateT > 15) nextRound();
      break;
  }
  oled.display();
}
