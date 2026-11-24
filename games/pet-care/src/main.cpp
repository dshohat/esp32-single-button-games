#include <Arduino.h>
#include <U8g2lib.h>

// One-button pet care game: watch what the pet needs, then press when the matching care icon is highlighted.

const int BUTTON_PIN = 4;   // other leg to GND
const int BUZZER_PIN = 23;  // passive buzzer
const int NOTE_GAP_MS = 30;  // long enough not to be skipped while a frame is being sent
const int FRAME_MS = 40;
const int GROUND = 45;

const int CYCLE_MS = 1000;  // how long each care icon stays highlighted
const int CALL_MS = 2600;   // how often the pet repeats its request
const int WRONG_MS = 1300;
const int HAPPY_MS = 3200;
const int MAX_REWARD = 3;   // hearts for the right care on the first try

#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);

// ---------------- Pets ----------------

enum Species { CAT, DOG, BUNNY, BEAR, MOUSE };
const char *NAMES[] = {"חתול", "כלב", "ארנב", "דוב", "עכבר"};

// Icon order on screen matches this order.
enum Need { SLEEP, FOOD, MEDICINE, PET, NEED_COUNT };

enum Face { F_OPEN, F_SLEEPY, F_SICK, F_HAPPY };

// ---------------- Sound ----------------

struct Note {
  uint16_t f;  // Hz, or REST
  uint8_t d;   // length in units
};

enum : uint16_t {
  REST = 0, nC5 = 523, nD5 = 587, nE5 = 659, nF5 = 698, nG5 = 784, nA5 = 880, nB5 = 988,
  nC6 = 1047, nD6 = 1175, nE6 = 1319, nG6 = 1568,
};

const Note THEME[] = {
  {nC5, 2}, {nE5, 2}, {nG5, 2}, {nE5, 2}, {nF5, 2}, {nA5, 2}, {nC6, 4},
  {nB5, 2}, {nG5, 2}, {nA5, 2}, {nF5, 2}, {nG5, 8},
  {nE5, 2}, {nG5, 2}, {nC6, 2}, {nG5, 2}, {nA5, 2}, {nB5, 2}, {nC6, 4},
  {nD6, 2}, {nB5, 2}, {nG5, 2}, {nD5, 2}, {nC5, 8}, {REST, 8},
};
const Note ZZZ_SFX[] = {{120, 3}, {105, 3}, {120, 3}, {105, 3}, {95, 6}};
const Note HUNGRY_SFX[] = {{600, 1}, {800, 1}, {1000, 1}, {REST, 2}, {600, 1}, {800, 1}, {1000, 1}, {REST, 2}, {700, 4}, {550, 4}};
const Note COUGH_SFX[] = {{230, 2}, {REST, 1}, {190, 2}, {REST, 4}, {230, 2}, {REST, 1}, {170, 3}};
const Note PURR_SFX[] = {{90, 2}, {75, 2}, {90, 2}, {75, 2}, {90, 2}, {75, 2}, {90, 2}, {75, 2}};
const Note WRONG_SFX[] = {{220, 3}, {160, 6}};
const Note HAPPY_SFX[] = {{nC6, 2}, {nE6, 2}, {nG6, 2}, {REST, 1}, {nE6, 2}, {nG6, 6}};

const Note *tune;
int tuneLen, tuneIdx, tuneUnit;
bool tuneLoop, sounding;
uint32_t noteEnd;

void play(const Note *t, int len, int unit, bool loop = false) {
  tune = t;
  tuneLen = len;
  tuneUnit = unit;
  tuneLoop = loop;
  tuneIdx = 0;
  noteEnd = millis();
}
#define PLAY(t, unit, ...) play(t, COUNT(t), unit, ##__VA_ARGS__)

void stopSound() {
  tune = nullptr;
  sounding = false;
  noTone(BUZZER_PIN);
}

void updateSound() {
  if (!tune) return;
  int32_t left = noteEnd - millis();
  if (left > 0) {
    if (sounding && left <= NOTE_GAP_MS) {
      noTone(BUZZER_PIN);
      sounding = false;
    }
    return;
  }
  if (tuneIdx >= tuneLen) {
    if (!tuneLoop) {
      stopSound();
      return;
    }
    tuneIdx = 0;
  }
  const Note &n = tune[tuneIdx++];
  if (n.f != REST) tone(BUZZER_PIN, n.f);
  else noTone(BUZZER_PIN);
  sounding = n.f != REST;
  noteEnd = millis() + n.d * tuneUnit;
}

void playNeed(int need) {
  switch (need) {
    case SLEEP: PLAY(ZZZ_SFX, 60); break;
    case FOOD: PLAY(HUNGRY_SFX, 50); break;
    case MEDICINE: PLAY(COUGH_SFX, 60); break;
    case PET: PLAY(PURR_SFX, 60); break;
  }
}

// ---------------- State ----------------

enum State { TITLE, NEED, WRONG, HAPPY };
State state = TITLE;
uint32_t stateAt;

int animal = -1, need = -1;
int wrongs, reward, hearts;
uint32_t callAt, cycleAt;

void setState(State s) {
  state = s;
  stateAt = millis();
}

int selected(uint32_t now) {
  return ((now - cycleAt) / CYCLE_MS) % NEED_COUNT;
}

// ---------------- Drawing ----------------

// U8g2 draws left to right, so Hebrew is reversed by code point, then centered on cx.
void drawHebrew(const char *s, int cx, int y) {
  int len = strlen(s);
  char buf[96];
  int out = len;
  buf[out] = 0;
  for (int i = 0; i < len;) {
    int n = 1;
    while (i + n < len && (s[i + n] & 0xC0) == 0x80) n++;
    out -= n;
    memcpy(buf + out, s + i, n);
    i += n;
  }
  oled.drawUTF8(cx - oled.getUTF8Width(buf) / 2, y, buf);
}

void drawHeart(int x, int y, int r) {
  oled.drawDisc(x - r, y, r);
  oled.drawDisc(x + r, y, r);
  oled.drawTriangle(x - 2 * r, y + r / 2, x + 2 * r, y + r / 2, x, y + 3 * r);
}

void drawEars(int sp, int hx, int hy) {
  switch (sp) {
    case CAT:
      oled.drawTriangle(hx - 7, hy - 3, hx - 2, hy - 6, hx - 6, hy - 12);
      oled.drawTriangle(hx + 7, hy - 3, hx + 2, hy - 6, hx + 6, hy - 12);
      break;
    case DOG:
      oled.drawFilledEllipse(hx - 7, hy + 1, 2, 5);
      oled.drawFilledEllipse(hx + 7, hy + 1, 2, 5);
      break;
    case BUNNY:
      oled.drawFilledEllipse(hx - 3, hy - 11, 2, 6);
      oled.drawFilledEllipse(hx + 3, hy - 11, 2, 6);
      break;
    case BEAR:
      oled.drawDisc(hx - 5, hy - 5, 3);
      oled.drawDisc(hx + 5, hy - 5, 3);
      break;
    case MOUSE:
      oled.drawCircle(hx - 6, hy - 6, 4);
      oled.drawCircle(hx + 6, hy - 6, 4);
      break;
  }
}

void drawHead(int sp, int hx, int hy, Face face, int tongue) {
  drawEars(sp, hx, hy);
  // Head hides the inner part of the ears.
  oled.setDrawColor(0);
  oled.drawDisc(hx, hy, 6);
  oled.setDrawColor(1);
  oled.drawCircle(hx, hy, 7);

  if (sp == CAT || sp == MOUSE) {
    oled.drawLine(hx - 8, hy + 1, hx - 12, hy);
    oled.drawLine(hx - 8, hy + 3, hx - 12, hy + 4);
    oled.drawLine(hx + 8, hy + 1, hx + 12, hy);
    oled.drawLine(hx + 8, hy + 3, hx + 12, hy + 4);
  }
  if (sp == DOG) oled.drawDisc(hx, hy + 2, 1);
  else oled.drawPixel(hx, hy + 2);

  switch (face) {
    case F_OPEN:
      oled.drawDisc(hx - 3, hy - 1, 1);
      oled.drawDisc(hx + 3, hy - 1, 1);
      break;
    case F_SLEEPY:
      oled.drawHLine(hx - 4, hy - 1, 3);
      oled.drawHLine(hx + 2, hy - 1, 3);
      break;
    case F_SICK:
      oled.drawLine(hx - 4, hy - 3, hx - 2, hy - 2);
      oled.drawLine(hx + 4, hy - 3, hx + 2, hy - 2);
      oled.drawHLine(hx - 4, hy, 3);
      oled.drawHLine(hx + 2, hy, 3);
      break;
    case F_HAPPY:
      oled.drawLine(hx - 4, hy, hx - 3, hy - 2);
      oled.drawLine(hx - 3, hy - 2, hx - 2, hy);
      oled.drawLine(hx + 2, hy, hx + 3, hy - 2);
      oled.drawLine(hx + 3, hy - 2, hx + 4, hy);
      break;
  }

  if (tongue > 0) {
    oled.drawHLine(hx - 2, hy + 4, 5);
    oled.drawRBox(hx - 2, hy + 4, 5, tongue + 3, 2);
    oled.setDrawColor(0);
    oled.drawVLine(hx, hy + 6, tongue - 1);
    oled.setDrawColor(1);
  } else if (face == F_HAPPY) {
    oled.drawPixel(hx - 2, hy + 4);
    oled.drawHLine(hx - 1, hy + 5, 3);
    oled.drawPixel(hx + 2, hy + 4);
  } else if (face == F_SICK) {
    oled.drawHLine(hx - 1, hy + 4, 3);
  } else {
    oled.drawPixel(hx - 1, hy + 4);
    oled.drawPixel(hx + 1, hy + 4);
  }
}

void drawTail(int sp, int x, int y) {
  if (sp == BUNNY || sp == BEAR) oled.drawDisc(x + 1, y, 2);
  else oled.drawLine(x, y, x + 5, y - 6);
}

void drawStanding(int sp, int cx, int hx, int hy, Face face, int tongue, int lift) {
  int by = GROUND - 7 - lift;
  for (int x = cx - 6; x <= cx + 6; x += 4) oled.drawVLine(x, by + 3, GROUND - by - 3);
  drawTail(sp, cx + 10, by - 1);
  oled.setDrawColor(0);
  oled.drawFilledEllipse(cx, by, 9, 4);
  oled.setDrawColor(1);
  oled.drawEllipse(cx, by, 10, 5);
  drawHead(sp, hx, hy - lift, face, tongue);
}

void drawLying(int sp, int cx, int hx, int hy, Face face) {
  drawTail(sp, cx + 17, GROUND - 4);
  oled.setDrawColor(0);
  oled.drawFilledEllipse(cx + 4, GROUND - 4, 12, 3);
  oled.setDrawColor(1);
  oled.drawEllipse(cx + 4, GROUND - 4, 13, 4);
  drawHead(sp, hx, hy, face, 0);
}

void drawNeedScene(uint32_t now, int dx) {
  int cx = 64 + dx;
  uint32_t t = now - callAt;
  switch (need) {
    case SLEEP: {
      int nod = (now / 700) % 2;
      drawStanding(animal, cx, cx, GROUND - 19 + nod, F_SLEEPY, 0, 0);
      oled.setFont(u8g2_font_6x13_t_hebrew);
      for (int i = 0; i < 3; i++) {
        float p = ((now + i * 700) % 2100) / 2100.0f;
        oled.drawStr(cx + 9 + p * 14, 24 - p * 14, p < 0.5f ? "z" : "Z");
      }
      break;
    }
    case FOOD: {
      int tongue = 5 + (int)(2 * sinf(now / 90.0f));
      int lift = t < 500 ? (int)(3 * sinf(PI * t / 500)) : 0;
      drawStanding(animal, cx, cx, GROUND - 20, F_OPEN, tongue, lift);
      break;
    }
    case MEDICINE: {
      bool coughing = t < 700;
      int shake = coughing ? ((now / 60) % 2 ? 1 : -1) : 0;
      int hx = cx - 10 + shake, hy = GROUND - 8 - (coughing ? 2 : 0);
      drawLying(animal, cx, hx, hy, F_SICK);
      if (coughing) {
        oled.drawCircle(hx - 10 - t / 80, hy + 3, 1 + t / 300);
        if (t > 250) oled.drawCircle(hx - 9 - (t - 250) / 80, hy + 1, 1);
      }
      break;
    }
    case PET: {
      // Head rubs up and down against the shoulder.
      float s = sinf(now * 2 * PI / 800);
      drawStanding(animal, cx, cx + 5 + (int)s, GROUND - 18 + (int)(2 * s), F_HAPPY, 0, 0);
      break;
    }
  }
  oled.drawHLine(0, GROUND, 128);
}

void drawIcon(int i, int x, int y) {
  switch (i) {
    case SLEEP:  // moon
      oled.drawDisc(x, y, 5);
      oled.setDrawColor(0);
      oled.drawDisc(x + 3, y - 2, 5);
      oled.setDrawColor(1);
      break;
    case FOOD:  // bowl with food
      oled.drawDisc(x, y - 1, 6, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      oled.drawCircle(x, y - 1, 4, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      break;
    case MEDICINE:  // first aid box
      oled.drawFrame(x - 6, y - 6, 13, 13);
      oled.drawBox(x - 1, y - 4, 3, 9);
      oled.drawBox(x - 4, y - 1, 9, 3);
      break;
    case PET:  // hand
      oled.drawRBox(x - 4, y - 1, 9, 7, 2);
      for (int f = x - 4; f <= x + 4; f += 2) oled.drawVLine(f, y - 5, 4);
      oled.drawLine(x - 4, y + 2, x - 7, y - 1);
      break;
  }
}

void drawIcons(int sel) {
  for (int i = 0; i < NEED_COUNT; i++) {
    int x = 16 + i * 32;
    drawIcon(i, x, 55);
    if (i == sel) oled.drawRFrame(x - 10, 46, 21, 18, 3);
  }
}

void drawTopBar() {
  char buf[8];
  oled.setFont(u8g2_font_6x13_t_hebrew);
  drawHeart(6, 4, 2);
  snprintf(buf, sizeof buf, "%d", hearts);
  oled.drawStr(14, 10, buf);
  drawHebrew(NAMES[animal], 106, 10);
}

void drawTitle(uint32_t now) {
  oled.setFont(u8g2_font_unifont_t_hebrew);
  drawHebrew("חיות מחמד", 64, 14);
  oled.setFont(u8g2_font_6x13_t_hebrew);
  drawHebrew("הבן מה החיה צריכה", 64, 29);
  drawHebrew("ולחץ כשהסימן מסומן", 64, 42);
  if ((now / 500) % 2) drawHebrew("לחץ להתחלה", 64, 62);
  drawHeart(10, 56, 3);
  drawHeart(118, 56, 3);
}

void drawHappy(uint32_t now) {
  uint32_t t = now - stateAt;
  oled.setFont(u8g2_font_6x13_t_hebrew);
  drawHebrew("כל הכבוד!", 64, 10);
  int lift = (int)(4 * fabsf(sinf(now / 150.0f)));
  drawStanding(animal, 64, 64, GROUND - 20, F_HAPPY, 0, lift);
  oled.drawHLine(0, GROUND, 128);
  for (int i = 0; i < reward; i++) {
    float p = ((t + i * 400) % 1200) / 1200.0f;
    int side = i % 2 ? 1 : -1;
    drawHeart(64 + side * (12 + i * 4), 26 - p * 14, 2);
  }
  int x0 = 64 - (reward - 1) * 8;
  for (int i = 0; i < reward; i++) drawHeart(x0 + i * 16, 52, 3);
}

void draw(uint32_t now) {
  oled.clearBuffer();
  switch (state) {
    case TITLE: drawTitle(now); break;
    case NEED:
      drawTopBar();
      drawNeedScene(now, 0);
      drawIcons(selected(now));
      break;
    case WRONG: {
      uint32_t t = now - stateAt;
      oled.setFont(u8g2_font_6x13_t_hebrew);
      drawHebrew("לא זה, נסה שוב", 64, 10);
      drawNeedScene(now, t < 600 ? ((now / 60) % 2 ? 3 : -3) : 0);
      drawIcons(-1);
      break;
    }
    case HAPPY: drawHappy(now); break;
  }
  oled.sendBuffer();
}

// ---------------- Game flow ----------------

void askAgain(uint32_t now) {
  cycleAt = now;
  callAt = now - CALL_MS + 400;  // repeat the request soon
  setState(NEED);
}

void nextAnimal(uint32_t now) {
  int prev = animal;
  do animal = random(COUNT(NAMES));
  while (animal == prev);
  prev = need;
  do need = random(NEED_COUNT);
  while (need == prev);
  wrongs = 0;
  stopSound();
  askAgain(now);
}

void choose(uint32_t now) {
  if (selected(now) == need) {
    reward = max(1, MAX_REWARD - wrongs);
    hearts += reward;
    setState(HAPPY);
    PLAY(HAPPY_SFX, 90);
  } else {
    wrongs++;
    setState(WRONG);
    PLAY(WRONG_SFX, 60);
  }
}

// ---------------- Button ----------------

bool pressed() {
  static bool down = false;
  static uint32_t changedAt = 0;
  bool raw = digitalRead(BUTTON_PIN) == LOW;
  uint32_t now = millis();
  if (raw != down && now - changedAt > 25) {
    down = raw;
    changedAt = now;
    return down;
  }
  return false;
}

void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  randomSeed(esp_random());
  oled.setBusClock(400000);
  oled.begin();
  PLAY(THEME, 110, true);
}

void loop() {
  static uint32_t lastFrame;
  updateSound();
  bool press = pressed();
  uint32_t now = millis();
  uint32_t inState = now - stateAt;

  switch (state) {
    case TITLE:
      if (press) {
        hearts = 0;
        nextAnimal(now);
      }
      break;
    case NEED:
      if (now - callAt >= CALL_MS) {
        callAt = now;
        playNeed(need);
      }
      if (press && inState > 300) choose(now);
      break;
    case WRONG:
      if (inState > WRONG_MS) askAgain(now);
      break;
    case HAPPY:
      if (inState > HAPPY_MS || (press && inState > 1000)) nextAnimal(now);
      break;
  }

  if (now - lastFrame >= FRAME_MS) {
    lastFrame = now;
    draw(now);
  }
}
