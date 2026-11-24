#include <Arduino.h>
#include <U8g2lib.h>
#include <Preferences.h>

// Naomi's Balloon: hold the button to float up, collect stars, dodge the bees.

const int BUTTON_PIN = 4;   // other leg to GND
const int BUZZER_PIN = 23;  // passive buzzer
const int FRAME_MS = 40;
const int TOP = 10;  // play area starts below the HUD
const int GROUND = 63;
const int PLAYER_X = 26;
const int PLAYER_H = 29;
const int START_LIVES = 3;
const int MAX_LIVES = 5;
const int STARS_PER_CHEER = 10;

#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
Preferences prefs;

const char *const CHEERS[] = {
  "כל הכבוד נעמי!",
  "נעמי אלופה!",
  "וואו! איזה יופי!",
  "את כוכבת, נעמי!",
  "מדהים! עוד קצת!",
};

const char *const HINTS[] = {
  "לחצי להתחלה",
  "החזיקי - הבלון עולה",
  "אספי כוכבים",
  "היזהרי מהדבורים",
};

// ---------------- Sound ----------------

struct Note {
  uint16_t f;  // Hz, 0 = rest
  uint8_t d;   // length in units
};

enum : uint16_t {
  REST = 0, nC5 = 523, nD5 = 587, nE5 = 659, nF5 = 698, nG5 = 784, nA5 = 880, nB5 = 988,
  nC6 = 1047, nD6 = 1175, nE6 = 1319, nG6 = 1568,
};

// An original happy tune.
const Note THEME[] = {
  {nC5, 2}, {nE5, 2}, {nG5, 2}, {nE5, 2}, {nA5, 4}, {nG5, 4},
  {nF5, 2}, {nA5, 2}, {nC6, 2}, {nA5, 2}, {nG5, 8},
  {nE5, 2}, {nG5, 2}, {nC6, 2}, {nB5, 2}, {nA5, 2}, {nG5, 2}, {nF5, 2}, {nE5, 2},
  {nD5, 4}, {nE5, 2}, {nF5, 2}, {nE5, 8},
  {nC5, 2}, {nE5, 2}, {nG5, 2}, {nE5, 2}, {nA5, 4}, {nG5, 4},
  {nF5, 2}, {nA5, 2}, {nC6, 2}, {nD6, 2}, {nE6, 8},
  {nD6, 2}, {nC6, 2}, {nB5, 2}, {nA5, 2}, {nG5, 4}, {nB5, 4},
  {nC6, 8}, {REST, 8},
};
const Note END_TUNE[] = {{nG5, 2}, {nE5, 2}, {nC5, 2}, {nE5, 2}, {nG5, 2}, {nC6, 6}};
const Note STAR_SFX[] = {{nC6, 1}, {nE6, 1}, {nG6, 2}};
const Note HEART_SFX[] = {{nG5, 1}, {nC6, 1}, {nE6, 1}, {nG6, 3}};
const Note OUCH_SFX[] = {{400, 2}, {300, 2}, {200, 3}};
const Note CHEER_SFX[] = {{nC6, 2}, {nC6, 1}, {nC6, 1}, {nE6, 2}, {nG6, 4}, {nE6, 2}, {nG6, 6}};

struct Track {
  const Note *t;
  int len, idx, unit;
  bool loop;
  uint32_t end;
  uint16_t f, dur;
};
Track music, sfx;
uint16_t curFreq;

void start(Track &k, const Note *t, int len, int unit, bool loop) {
  k = {t, len, 0, unit, loop, millis(), 0, 0};
}
#define MUSIC(t, unit, loop) start(music, t, COUNT(t), unit, loop)
#define SFX(t, unit) start(sfx, t, COUNT(t), unit, false)

uint16_t step(Track &k, uint32_t now) {
  if (!k.t) return 0;
  while ((int32_t)(now - k.end) >= 0) {
    if (k.idx >= k.len) {
      if (!k.loop) {
        k.t = nullptr;
        return 0;
      }
      k.idx = 0;
    }
    k.f = k.t[k.idx].f;
    k.dur = k.t[k.idx].d * k.unit;
    k.end += k.dur;
    k.idx++;
  }
  // A short silence at the end of each note keeps repeated notes distinct.
  return (int32_t)(k.end - now) > min(25, k.dur / 4) ? k.f : 0;
}

void audio() {
  uint32_t now = millis();
  uint16_t m = step(music, now), s = step(sfx, now);
  uint16_t f = sfx.t ? s : m;
  if (f == curFreq) return;
  curFreq = f;
  if (f) tone(BUZZER_PIN, f);
  else noTone(BUZZER_PIN);
}

// ---------------- Game state ----------------

enum State { TITLE, PLAY, ENDING, OVER };
State state = TITLE;
int stateFrames;
uint32_t frame;

enum Kind : uint8_t { EMPTY, STAR, HEART, BEE };
struct Obj {
  float x, y;
  Kind kind;
  uint8_t phase;
};
Obj objs[10];

struct Spark {
  float x, y, vx, vy;
  int8_t life;
};
Spark sparks[20];

float py, vy, speed, scroll, nextSpawn;
float cloudX[2] = {60, 130};
int lives, stars, best, invuln, cheerT, cheerMsg;
bool newBest, btnHeld, pressLatch;

void setState(State s) {
  state = s;
  stateFrames = 0;
}

// ---------------- Drawing ----------------

// U8g2 draws left to right, so Hebrew is reversed by code point; digit runs are flipped back.
void drawHebrew(const char *s, int cx, int y) {
  int len = strlen(s);
  char buf[96];
  if (len >= (int)sizeof buf) return;
  int out = len;
  buf[out] = 0;
  for (int i = 0; i < len;) {
    int n = 1;
    while (i + n < len && (s[i + n] & 0xC0) == 0x80) n++;
    out -= n;
    memcpy(buf + out, s + i, n);
    i += n;
  }
  for (int i = 0; i < len;) {
    if (!isdigit((unsigned char)buf[i])) {
      i++;
      continue;
    }
    int j = i;
    while (j < len && isdigit((unsigned char)buf[j])) j++;
    for (int a = i, b = j - 1; a < b; a++, b--) std::swap(buf[a], buf[b]);
    i = j;
  }
  oled.drawUTF8(cx - oled.getUTF8Width(buf) / 2, y, buf);
}

void drawNaomi(int x, int y) {
  int sway = (frame / 6) % 2;
  // Balloon with a shine and a knot.
  oled.drawDisc(x, y + 5, 5);
  oled.setDrawColor(0);
  oled.drawPixel(x - 2, y + 3);
  oled.drawPixel(x - 3, y + 4);
  oled.drawPixel(x - 2, y + 4);
  oled.setDrawColor(1);
  oled.drawTriangle(x - 1, y + 12, x + 2, y + 12, x, y + 10);
  oled.drawLine(x, y + 12, x + 4, y + 15);
  // Naomi: hair, face, ponytail, dress, arms and legs.
  oled.drawDisc(x, y + 18, 3, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  oled.drawCircle(x, y + 18, 3, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
  oled.drawPixel(x + 1, y + 19);
  oled.drawLine(x - 3, y + 16, x - 6, y + 18 + sway);
  oled.drawPixel(x - 6, y + 19 + sway);
  oled.drawTriangle(x - 4, y + 27, x + 5, y + 27, x, y + 21);
  oled.drawLine(x + 2, y + 22, x + 4, y + 15);
  oled.drawLine(x - 2, y + 23, x - 4, y + 25);
  oled.drawVLine(x - 2, y + 27, 2);
  oled.drawVLine(x + 2, y + 27, 2);
}

void drawStar(int x, int y, int r) {
  int sx[5], sy[5];
  for (int i = 0; i < 5; i++) {
    float a = -HALF_PI + i * TWO_PI / 5;
    sx[i] = x + lroundf(cosf(a) * r);
    sy[i] = y + lroundf(sinf(a) * r);
  }
  for (int i = 0; i < 5; i++) oled.drawLine(sx[i], sy[i], sx[(i + 2) % 5], sy[(i + 2) % 5]);
}

void drawHeart(int x, int y) {
  oled.drawDisc(x - 2, y - 1, 2);
  oled.drawDisc(x + 2, y - 1, 2);
  oled.drawTriangle(x - 4, y, x + 5, y, x, y + 4);
}

void drawBee(int x, int y, bool flap) {
  oled.drawFilledEllipse(x, y, 5, 3);
  oled.setDrawColor(0);
  oled.drawVLine(x, y - 3, 7);
  oled.drawVLine(x + 2, y - 3, 7);
  oled.drawPixel(x - 3, y - 1);
  oled.setDrawColor(1);
  oled.drawPixel(x + 6, y);
  oled.drawLine(x - 4, y - 2, x - 6, y - 4);
  int ry = flap ? 2 : 1;
  oled.drawEllipse(x - 1, y - 4 - ry, 2, ry);
  oled.drawEllipse(x + 2, y - 4 - ry, 2, ry);
}

void drawCloud(int x, int y) {
  oled.drawCircle(x - 5, y, 4, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  oled.drawCircle(x + 1, y, 6, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  oled.drawCircle(x + 8, y, 4, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  oled.drawHLine(x - 9, y, 22);
}

float objY(const Obj &o) {
  return o.kind == BEE ? o.y + sinf(o.phase * 0.15f) * 5 : o.y;
}

void drawWorld() {
  for (int i = 0; i < 2; i++) drawCloud((int)cloudX[i], i ? 22 : 16);

  oled.drawHLine(0, GROUND, 128);
  int g = (int)scroll;
  for (int x = -(g % 16); x < 128; x += 16) {
    oled.drawPixel(x, GROUND - 1);
    oled.drawPixel(x + 2, GROUND - 2);
    oled.drawPixel(x + 4, GROUND - 1);
  }
  for (int x = 40 - (g % 64); x < 128; x += 64) {
    oled.drawVLine(x, GROUND - 3, 3);
    oled.drawCircle(x, GROUND - 5, 1);
  }

  if (cheerT > 0) {
    for (int r = 46; r <= 54; r += 4) oled.drawCircle(64, 70, r, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
    oled.setDrawColor(0);
    oled.drawBox(10, 24, 108, 17);
    oled.setDrawColor(1);
    oled.drawRFrame(10, 24, 108, 17, 4);
    oled.setFont(u8g2_font_6x13_t_hebrew);
    drawHebrew(CHEERS[cheerMsg], 64, 37);
  }

  for (int i = 0; i < COUNT(objs); i++) {
    const Obj &o = objs[i];
    int x = (int)o.x, y = (int)objY(o);
    if (o.kind == STAR) drawStar(x, y, ((frame / 5 + i) % 3) ? 5 : 4);
    else if (o.kind == HEART) drawHeart(x, y);
    else if (o.kind == BEE) drawBee(x, y, (frame / 2) % 2);
  }

  if (!(invuln > 0 && (frame / 3) % 2)) drawNaomi(PLAYER_X, (int)py);

  for (auto &s : sparks)
    if (s.life > 0) {
      s.x += s.vx;
      s.y += s.vy;
      s.life--;
      oled.drawPixel((int)s.x, (int)s.y);
    }

  // HUD: hearts on the left, star count on the right.
  for (int i = 0; i < lives; i++) drawHeart(5 + i * 10, 4);
  char buf[12];
  snprintf(buf, sizeof buf, "%d", stars);
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(112 - oled.getStrWidth(buf), 9, buf);
  drawStar(121, 5, 4);
}

void drawTitle() {
  oled.setFont(u8g2_font_unifont_t_hebrew);
  drawHebrew("הבלון של נעמי", 64, 14);
  static const int8_t SX[] = {28, 100, 18, 110}, SY[] = {30, 26, 48, 46};
  for (int i = 0; i < 4; i++) drawStar(SX[i], SY[i], ((frame / 6 + i) % 3) ? 4 : 2);
  drawNaomi(64, 19 + lroundf(sinf(frame * 0.12f) * 2));
  oled.setFont(u8g2_font_6x13_t_hebrew);
  drawHebrew(HINTS[(frame / 50) % COUNT(HINTS)], 64, 62);
}

void drawOver() {
  char buf[48];
  oled.setFont(u8g2_font_unifont_t_hebrew);
  drawHebrew("כל הכבוד!", 64, 14);
  oled.setFont(u8g2_font_6x13_t_hebrew);
  if (stars == 1) snprintf(buf, sizeof buf, "אספת כוכב אחד");
  else snprintf(buf, sizeof buf, "אספת %d כוכבים", stars);
  drawHebrew(buf, 64, 31);
  if (newBest) {
    if ((frame / 6) % 2) drawHebrew("שיא חדש!", 64, 46);
  } else {
    snprintf(buf, sizeof buf, "השיא: %d", best);
    drawHebrew(buf, 64, 46);
  }
  if (stateFrames > 25) drawHebrew("לחצי לשחק שוב", 64, 62);
}

void draw() {
  oled.clearBuffer();
  if (state == TITLE) drawTitle();
  else if (state == OVER) drawOver();
  else drawWorld();
  oled.sendBuffer();
}

// ---------------- Gameplay ----------------

void enterTitle() {
  setState(TITLE);
  MUSIC(THEME, 110, true);
}

void startGame() {
  randomSeed(esp_random());
  setState(PLAY);
  py = 30;
  vy = scroll = 0;
  nextSpawn = 60;
  lives = START_LIVES;
  stars = invuln = cheerT = 0;
  newBest = false;
  for (auto &o : objs) o.kind = EMPTY;
  for (auto &s : sparks) s.life = 0;
  MUSIC(THEME, 110, true);
}

void add(Kind kind, float x, float y) {
  for (auto &o : objs)
    if (o.kind == EMPTY) {
      o = {x, y, kind, (uint8_t)random(256)};
      return;
    }
}

void spawn() {
  float x = 136;
  int r = random(100);
  int beeChance = cheerT > 0 ? 0 : min(20 + stars * 2, 45);
  if (r < beeChance) {
    add(BEE, x, random(TOP + 8, GROUND - 8));
  } else if (r < beeChance + 6 && lives < MAX_LIVES) {
    add(HEART, x, random(TOP + 6, GROUND - 6));
  } else if (random(3) == 0) {
    int y = random(TOP + 12, GROUND - 6);
    for (int i = 0; i < 3; i++) add(STAR, x + i * 13, y - (i == 1 ? 7 : 0));
    x += 26;
  } else {
    add(STAR, x, random(TOP + 6, GROUND - 6));
  }
  nextSpawn = random(38, 70) + (x - 136);
}

void sparkle(float x, float y) {
  int n = 0;
  for (auto &s : sparks)
    if (s.life <= 0 && n < 8) {
      float a = n * TWO_PI / 8;
      s = {x, y, cosf(a) * 1.5f, sinf(a) * 1.5f, 8};
      n++;
    }
}

// The bee hitbox is smaller than the drawing, to be forgiving.
bool touches(float x, float y, int r, int inset) {
  return x + r >= PLAYER_X - 5 + inset && x - r <= PLAYER_X + 5 - inset &&
         y + r >= py + inset && y - r <= py + PLAYER_H - 1 - inset;
}

void updatePlay(bool hold) {
  speed = min(1.4f + stars * 0.04f, 3.2f);
  scroll += speed;
  vy += hold ? -0.32f : 0.16f;
  vy = constrain(vy, -2.0f, 1.5f);
  py += vy;
  if (py < TOP) py = TOP, vy = 0;
  if (py > GROUND - PLAYER_H) py = GROUND - PLAYER_H, vy = 0;
  if (invuln > 0) invuln--;
  if (cheerT > 0) cheerT--;
  for (auto &c : cloudX)
    if ((c -= speed * 0.3f) < -16) c = 140 + random(40);

  nextSpawn -= speed;
  if (nextSpawn <= 0) spawn();

  for (auto &o : objs) {
    if (o.kind == EMPTY) continue;
    o.x -= speed + (o.kind == BEE ? 0.5f : 0);
    o.phase++;
    if (o.x < -12) {
      o.kind = EMPTY;
      continue;
    }
    float y = objY(o);
    if (o.kind == BEE) {
      if (invuln == 0 && touches(o.x, y, 3, 2)) {
        lives--;
        invuln = 50;
        SFX(OUCH_SFX, 45);
        if (lives <= 0) {
          setState(ENDING);
          MUSIC(END_TUNE, 120, false);
          return;
        }
      }
    } else if (touches(o.x, y, 5, 0)) {
      sparkle(o.x, y);
      if (o.kind == HEART) {
        lives = min(lives + 1, MAX_LIVES);
        SFX(HEART_SFX, 50);
      } else if (++stars % STARS_PER_CHEER == 0) {
        cheerT = 60;
        cheerMsg = random(COUNT(CHEERS));
        SFX(CHEER_SFX, 70);
      } else {
        SFX(STAR_SFX, 45);
      }
      o.kind = EMPTY;
    }
  }
}

// Naomi floats gently down to the grass before the results.
void updateEnding() {
  invuln = 0;
  if (py < GROUND - PLAYER_H) py = min(py + 0.8f, (float)(GROUND - PLAYER_H));
  else if (stateFrames > 40) {
    setState(OVER);
    newBest = stars > best;
    if (newBest) {
      best = stars;
      prefs.putInt("best", best);
      SFX(CHEER_SFX, 70);
    }
  }
}

// ---------------- Main ----------------

bool readButton() {
  static uint32_t changedAt = 0;
  bool raw = digitalRead(BUTTON_PIN) == LOW;
  uint32_t now = millis();
  if (raw != btnHeld && now - changedAt > 20) {
    btnHeld = raw;
    changedAt = now;
    return raw;
  }
  return false;
}

void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  oled.setBusClock(400000);
  oled.begin();
  prefs.begin("naomi");
  best = prefs.getInt("best", 0);
  enterTitle();
}

void loop() {
  static uint32_t lastFrame;
  audio();
  if (readButton()) pressLatch = true;
  uint32_t now = millis();
  if (now - lastFrame < FRAME_MS) return;
  lastFrame = now;
  bool press = pressLatch;
  pressLatch = false;
  frame++;
  stateFrames++;

  switch (state) {
    case TITLE:
      if (press) startGame();
      break;
    case PLAY:
      updatePlay(btnHeld);
      break;
    case ENDING:
      updateEnding();
      break;
    case OVER:
      if (press && stateFrames > 25) startGame();
      else if (stateFrames > 750) enterTitle();
      break;
  }
  draw();
}
