#include <Arduino.h>
#include <U8g2lib.h>

// One-button Pokemon game: catch a wild Pokemon with a single throw, then battle a training dummy.

const int BUTTON_PIN = 4;   // other leg to GND
const int BUZZER_PIN = 23;  // passive buzzer
const int NOTE_GAP_MS = 30;  // long enough not to be skipped while a frame is being sent
const int FRAME_MS = 40;
const int GROUND = 61;

const int FLY_MS = 450;
const int WOBBLE_MS = 500;
const int RESULT_MS = 3000;
const int BATTLE_INTRO_MS = 3500;
const int DUMMY_HP = 120;
const int DUMMY_EVERY_MS = 1500;
const int HINT_AFTER_MS = 1200;  // idle time before "press fast" flashes

#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);

// ---------------- Pokemon ----------------

struct Species {
  const char *name;
  uint8_t power;  // damage per button press
  uint8_t hp;
};

const Species MONS[] = {
  {"פיקאצ'ו", 5, 90},
  {"צ'רמנדר", 6, 80},
  {"בולבזאור", 4, 110},
  {"סקווירטל", 4, 100},
  {"ג'יגליפאף", 3, 130},
};
const char *DUMMY_NAME = "בובת אימון";

// ---------------- Sound ----------------

struct Note {
  uint16_t f;  // Hz, or REST
  uint8_t d;   // length in units
};

enum : uint16_t {
  REST = 0, nG4 = 392, nC5 = 523, nD5 = 587, nE5 = 659, nF5 = 698, nG5 = 784, nA5 = 880, nB5 = 988,
  nC6 = 1047, nD6 = 1175, nE6 = 1319, nF6 = 1397, nG6 = 1568,
};

// An original adventure tune: the real Pokemon GO theme is copyrighted.
const Note THEME[] = {
  {nC5, 2}, {nE5, 2}, {nG5, 2}, {nC6, 6}, {nB5, 2}, {nA5, 2},
  {nG5, 4}, {nE5, 2}, {nG5, 2}, {nA5, 8},
  {nF5, 2}, {nA5, 2}, {nC6, 2}, {nF6, 6}, {nE6, 2}, {nD6, 2},
  {nC6, 4}, {nA5, 2}, {nB5, 2}, {nG5, 8},
  {nC5, 2}, {nE5, 2}, {nG5, 2}, {nC6, 6}, {nD6, 2}, {nE6, 2},
  {nF6, 4}, {nE6, 2}, {nD6, 2}, {nC6, 4}, {nA5, 4},
  {nB5, 2}, {nC6, 2}, {nD6, 4}, {nG5, 2}, {nA5, 2}, {nB5, 4},
  {nC6, 8}, {REST, 8},
};
const Note THROW_SFX[] = {{nG5, 1}, {nA5, 1}, {nB5, 1}, {nC6, 1}, {nD6, 1}, {nE6, 1}, {nG6, 2}};
const Note CLICK_SFX[] = {{300, 2}};
const Note CAUGHT_SFX[] = {{nC6, 2}, {nE6, 2}, {nG6, 2}, {REST, 1}, {nE6, 2}, {nG6, 6}};
const Note ESCAPE_SFX[] = {{nG5, 2}, {nE5, 2}, {nC5, 2}, {nG4, 6}};
const Note HIT_SFX[] = {{1400, 1}, {900, 1}};
const Note HURT_SFX[] = {{200, 2}, {150, 2}};
const Note WIN_SFX[] = {{nG5, 2}, {nC6, 2}, {nE6, 2}, {nG6, 4}, {nE6, 2}, {nG6, 8}};
const Note LOSE_SFX[] = {{nG5, 4}, {nF5, 4}, {nE5, 4}, {nD5, 4}, {nC5, 12}};

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

// ---------------- State ----------------

enum State { TITLE, CATCH, THROW, RESULT, BATTLE_INTRO, BATTLE, END };
State state = TITLE;
uint32_t stateAt;

int mine = 0;  // the player's Pokemon, kept until a new one is caught
int wild;
bool caught;
int wobbles, lastWobble;

int myHp, dumHp;
uint32_t myAtkAt, dumAtkAt, nextDummyAtk, lastPressAt;
bool won;

void setState(State s) {
  state = s;
  stateAt = millis();
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

void drawX(int x, int y) {
  oled.drawLine(x - 1, y - 1, x + 1, y + 1);
  oled.drawLine(x - 1, y + 1, x + 1, y - 1);
}

void drawStar(int x, int y) {
  oled.drawLine(x - 5, y, x + 5, y);
  oled.drawLine(x, y - 5, x, y + 5);
  oled.drawLine(x - 3, y - 3, x + 3, y + 3);
  oled.drawLine(x - 3, y + 3, x + 3, y - 3);
}

void drawBall(int x, int y, int r) {
  oled.drawDisc(x, y, r, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  oled.drawCircle(x, y, r);
  oled.drawHLine(x - r, y, 2 * r + 1);
  int b = r > 6 ? 3 : 1;
  oled.setDrawColor(0);
  oled.drawDisc(x, y, b + 1);
  oled.setDrawColor(1);
  oled.drawCircle(x, y, b);
}

void drawMon(int id, int cx, int cy) {
  switch (id) {
    case 0:  // long ears and a lightning tail
      oled.drawTriangle(cx - 8, cy - 4, cx - 3, cy - 8, cx - 11, cy - 17);
      oled.drawTriangle(cx + 8, cy - 4, cx + 3, cy - 8, cx + 11, cy - 17);
      oled.drawLine(cx + 8, cy + 4, cx + 14, cy);
      oled.drawLine(cx + 14, cy, cx + 11, cy - 3);
      oled.drawLine(cx + 11, cy - 3, cx + 17, cy - 9);
      break;
    case 1:  // flame on the tail
      oled.drawLine(cx + 7, cy + 6, cx + 14, cy + 2);
      oled.drawTriangle(cx + 11, cy + 1, cx + 17, cy + 1, cx + 14, cy - 7);
      break;
    case 2:  // bulb on the back
      oled.drawCircle(cx, cy - 10, 6);
      oled.drawTriangle(cx - 2, cy - 15, cx + 2, cy - 15, cx, cy - 19);
      break;
    case 3:  // shell and a curly tail
      oled.drawCircle(cx, cy + 1, 12, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      oled.drawCircle(cx, cy + 1, 11, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      oled.drawCircle(cx + 14, cy + 6, 3);
      break;
    case 4:  // hair curl and small ears
      oled.drawCircle(cx - 2, cy - 10, 3, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT | U8G2_DRAW_LOWER_RIGHT);
      oled.drawTriangle(cx - 8, cy - 5, cx - 4, cy - 8, cx - 9, cy - 11);
      oled.drawTriangle(cx + 8, cy - 5, cx + 4, cy - 8, cx + 9, cy - 11);
      break;
  }
  // Body hides whatever is drawn behind it.
  oled.setDrawColor(0);
  oled.drawDisc(cx, cy, 8);
  oled.setDrawColor(1);
  oled.drawCircle(cx, cy, 9);
  if (id == 4) {
    oled.drawCircle(cx - 3, cy - 2, 2);
    oled.drawCircle(cx + 3, cy - 2, 2);
    oled.drawPixel(cx - 3, cy - 2);
    oled.drawPixel(cx + 3, cy - 2);
  } else {
    oled.drawDisc(cx - 3, cy - 2, 1);
    oled.drawDisc(cx + 3, cy - 2, 1);
  }
  oled.drawHLine(cx - 1, cy + 3, 3);
  if (id == 0) {
    oled.drawCircle(cx - 6, cy + 3, 1);
    oled.drawCircle(cx + 6, cy + 3, 1);
  } else if (id == 2) {
    oled.drawPixel(cx - 4, cy + 5);
    oled.drawPixel(cx + 3, cy + 6);
  }
}

void drawFaintedMon(int cx) {
  oled.drawEllipse(cx, GROUND - 4, 12, 4);
  drawX(cx - 4, GROUND - 4);
  drawX(cx + 4, GROUND - 4);
}

void drawDummy(int cx, bool down) {
  if (down) {
    oled.drawRFrame(cx - 9, GROUND - 8, 18, 8, 3);
    oled.drawCircle(cx - 14, GROUND - 5, 4);
    oled.drawHLine(cx + 9, GROUND - 4, 8);
    drawX(cx - 14, GROUND - 5);
    return;
  }
  oled.drawVLine(cx, GROUND - 4, 4);
  oled.drawHLine(cx - 13, GROUND - 18, 6);
  oled.drawHLine(cx + 8, GROUND - 18, 6);
  oled.drawRFrame(cx - 7, GROUND - 22, 14, 18, 4);
  oled.drawCircle(cx, GROUND - 13, 3);
  oled.drawPixel(cx, GROUND - 13);
  oled.drawCircle(cx, GROUND - 28, 5);
  drawX(cx - 2, GROUND - 29);
  drawX(cx + 2, GROUND - 29);
}

void drawBar(int x, int hp, int maxHp) {
  oled.drawFrame(x, 14, 56, 5);
  oled.drawBox(x + 1, 15, 54 * hp / maxHp, 3);
}

int ringRadius(uint32_t now) {
  int t = now % 1200;
  int tri = t < 600 ? t : 1200 - t;
  return 11 + tri * 9 / 600;
}

void drawTitle() {
  oled.setFont(u8g2_font_unifont_t_hebrew);
  drawHebrew("פוקימון", 64, 14);
  drawBall(64, 35, 11);
  oled.setFont(u8g2_font_6x13_t_hebrew);
  drawHebrew("לחץ להתחלה", 64, 62);
}

void drawCatch(uint32_t now) {
  char buf[64];
  oled.setFont(u8g2_font_6x13_t_hebrew);
  snprintf(buf, sizeof buf, "%s פראי!", MONS[wild].name);
  drawHebrew(buf, 64, 11);
  if (state == CATCH) {
    drawMon(wild, 64, 36);
    oled.drawCircle(64, 36, ringRadius(now));
    drawBall(64, 59, 4);
    return;
  }
  int t = now - stateAt;
  if (t < FLY_MS) {
    float p = (float)t / FLY_MS;
    drawMon(wild, 64, 36);
    drawBall(64, 59 - 22 * p - 14 * sinf(PI * p), 4);
  } else {
    int phase = (t - FLY_MS) % WOBBLE_MS;
    int wobble = (t - FLY_MS) / WOBBLE_MS;
    int off = phase < 200 ? (wobble % 2 ? -2 : 2) : 0;
    drawBall(64 + off, 40, 6);
  }
}

void drawResult() {
  char buf[64];
  oled.setFont(u8g2_font_6x13_t_hebrew);
  if (caught) {
    snprintf(buf, sizeof buf, "תפסת את %s!", MONS[wild].name);
    drawHebrew(buf, 64, 11);
    drawBall(64, 36, 10);
    drawHebrew("הוא הפוקימון שלך", 64, 62);
  } else {
    snprintf(buf, sizeof buf, "%s ברח!", MONS[wild].name);
    drawHebrew(buf, 64, 11);
    drawMon(wild, 64, 36);
    snprintf(buf, sizeof buf, "נשאר לך %s", MONS[mine].name);
    drawHebrew(buf, 64, 62);
  }
}

void drawBattle(uint32_t now) {
  const int MY_X = 30, DUMMY_X = 98;
  bool myHit = now - myAtkAt < 120;
  bool dumHit = now - dumAtkAt < 150;
  int shake = (now / 30) % 2 ? 2 : -2;
  int px = MY_X + (myHit ? 8 : 0) + (dumHit ? shake : 0);
  int dx = DUMMY_X - (dumHit ? 8 : 0) + (myHit ? shake : 0);

  if (state == END) {
    oled.setFont(u8g2_font_unifont_t_hebrew);
    drawHebrew(won ? "ניצחת!" : "הפסדת...", 64, 14);
    oled.setFont(u8g2_font_6x13_t_hebrew);
    drawHebrew("לחץ להמשך", 64, 28);
    if (won) drawMon(mine, MY_X, GROUND - 10);
    else drawFaintedMon(MY_X);
    drawDummy(DUMMY_X, won);
  } else {
    oled.setFont(u8g2_font_6x13_t_hebrew);
    drawHebrew(MONS[mine].name, MY_X, 10);
    drawHebrew(DUMMY_NAME, DUMMY_X, 10);
    drawBar(2, myHp, MONS[mine].hp);
    drawBar(70, dumHp, DUMMY_HP);
    drawMon(mine, px, GROUND - 10);
    drawDummy(dx, false);
    if (myHit) drawStar(dx, GROUND - 13);
    if (dumHit) drawStar(px + 6, GROUND - 10);
    if (now - lastPressAt > HINT_AFTER_MS && (now / 300) % 2) drawHebrew("לחץ מהר!", 64, 32);
  }
  oled.drawHLine(0, GROUND, 128);
}

void drawBattleIntro() {
  oled.setFont(u8g2_font_unifont_t_hebrew);
  drawHebrew("קרב!", 64, 14);
  oled.setFont(u8g2_font_6x13_t_hebrew);
  drawHebrew("לחץ מהר על הכפתור", 64, 32);
  drawHebrew("כל לחיצה היא מכה", 64, 46);
  drawHebrew("מהר יותר = ניצחון", 64, 60);
}

void draw(uint32_t now) {
  oled.clearBuffer();
  switch (state) {
    case TITLE: drawTitle(); break;
    case CATCH:
    case THROW: drawCatch(now); break;
    case RESULT: drawResult(); break;
    case BATTLE_INTRO: drawBattleIntro(); break;
    case BATTLE:
    case END: drawBattle(now); break;
  }
  oled.sendBuffer();
}

// ---------------- Game flow ----------------

void startCatch() {
  do wild = random(COUNT(MONS));
  while (wild == mine);
  setState(CATCH);
  PLAY(THEME, 110, true);
}

void throwBall(uint32_t now) {
  int r = ringRadius(now);
  int chance = r <= 13 ? 85 : r <= 16 ? 45 : 15;  // a smaller ring is a better throw
  caught = random(100) < chance;
  wobbles = caught ? 3 : random(1, 3);
  lastWobble = -1;
  setState(THROW);
  PLAY(THROW_SFX, 35);
}

void startBattle() {
  myHp = MONS[mine].hp;
  dumHp = DUMMY_HP;
  stopSound();
  setState(BATTLE_INTRO);
}

void beginFight(uint32_t now) {
  myAtkAt = dumAtkAt = now - 1000;
  lastPressAt = now;
  nextDummyAtk = now + 2000;
  setState(BATTLE);
  PLAY(THROW_SFX, 35);
}

void endBattle() {
  won = dumHp <= 0;
  myHp = max(myHp, 0);
  dumHp = max(dumHp, 0);
  setState(END);
  if (won) PLAY(WIN_SFX, 90);
  else PLAY(LOSE_SFX, 120);
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
      if (press) startCatch();
      break;
    case CATCH:
      if (press) throwBall(now);
      break;
    case THROW: {
      int32_t t = inState - FLY_MS;
      if (t < 0) break;
      int w = t / WOBBLE_MS;
      if (w >= wobbles) {
        setState(RESULT);
        if (caught) {
          mine = wild;
          PLAY(CAUGHT_SFX, 90);
        } else {
          PLAY(ESCAPE_SFX, 90);
        }
      } else if (w != lastWobble) {
        lastWobble = w;
        PLAY(CLICK_SFX, 20);
      }
      break;
    }
    case RESULT:
      if (inState > RESULT_MS || (press && inState > 800)) startBattle();
      break;
    case BATTLE_INTRO:
      if (inState > BATTLE_INTRO_MS) beginFight(now);
      break;
    case BATTLE:
      if (press) {
        myAtkAt = lastPressAt = now;
        dumHp -= MONS[mine].power + random(-1, 2);
        PLAY(HIT_SFX, 30);
      }
      if ((int32_t)(now - nextDummyAtk) >= 0) {
        dumAtkAt = now;
        nextDummyAtk = now + DUMMY_EVERY_MS;
        myHp -= random(5, 10);
        PLAY(HURT_SFX, 40);
      }
      if (dumHp <= 0 || myHp <= 0) endBattle();
      break;
    case END:
      if (press && inState > 1000) startCatch();
      break;
  }

  if (now - lastFrame >= FRAME_MS) {
    lastFrame = now;
    draw(now);
  }
}
