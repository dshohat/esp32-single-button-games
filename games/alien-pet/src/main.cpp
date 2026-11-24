#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>

// ALIEN PET: one-button Tamagotchi (ESP32 + SSD1306 + passive buzzer).
// Tap to move between FEED / SLEEP / BATH, hold 3 s to do it.

const int BUTTON_PIN = 4;   // other leg to GND
const int BUZZER_PIN = 23;  // passive buzzer

const int SCREEN_W = 128, SCREEN_H = 64;
const int FRAME_MS = 33;
const uint32_t HOLD_MS = 3000, TAP_MAX_MS = 600;
const uint32_t SAD_MS = 10000;  // unhappy this long = dead
const uint32_t ACT_MS = 1800, NO_MS = 1000;
const int CARE_PTS = 10;

const int MENU_Y = 52, BOX_W = 42;
const int ALIEN_X = 62, ALIEN_Y = 33;
const int BUB_X = 96, BUB_Y = 12, BUB_W = 31, BUB_H = 26;
const int BUB_CX = BUB_X + BUB_W / 2, BUB_CY = BUB_Y + BUB_H / 2;

#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);
Preferences prefs;

// ---------------- Sound ----------------

struct Note {
  uint16_t f, ms;
};

const Note SFX_START[] = {{523, 60}, {659, 60}, {784, 60}, {1047, 120}};
const Note SFX_TICK[] = {{1760, 12}};
const Note SFX_NEED[] = {{1175, 60}, {0, 50}, {1175, 60}, {0, 50}, {1568, 90}};
const Note SFX_WARN[] = {{2093, 60}};
const Note SFX_FEED[] = {{330, 40}, {0, 120}, {294, 40}, {0, 120}, {330, 40}, {0, 120}, {262, 60}};
const Note SFX_SLEEP[] = {{659, 200}, {523, 200}, {587, 200}, {392, 400}};
const Note SFX_BATH[] = {{1319, 30}, {1760, 30}, {1175, 30}, {1568, 30}, {1397, 30}, {1865, 40}};
const Note SFX_HAPPY[] = {{784, 70}, {988, 70}, {1175, 70}, {1568, 160}};
const Note SFX_NO[] = {{220, 120}, {0, 40}, {196, 200}};
const Note SFX_GROW[] = {{523, 80}, {659, 80}, {784, 80}, {1047, 80}, {1319, 80}, {1568, 240}};
const Note SFX_DEAD[] = {{392, 220}, {370, 220}, {349, 220}, {330, 600}};

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

void beep(uint16_t f, uint16_t ms) {
  Note n = {f, ms};
  play(&n, 1);
}

void updateSound() {
  if (sfxIdx >= sfxLen || millis() - sfxT < sfx[sfxIdx].ms) return;
  sfxT += sfx[sfxIdx].ms;
  sfxIdx++;
  ledcWriteTone(BUZZER_PIN, sfxIdx < sfxLen ? sfx[sfxIdx].f : 0);
}

// ---------------- Button ----------------

enum { NONE, TAP, HOLD };
bool btnDown, holdFired;
uint32_t btnT, edgeT;

// TAP on a short release, HOLD once held HOLD_MS; a long press let go early does nothing.
int pollButton() {
  bool raw = digitalRead(BUTTON_PIN) == LOW;
  uint32_t now = millis();
  if (raw != btnDown && now - edgeT >= 25) {
    btnDown = raw;
    edgeT = now;
    if (raw) {
      btnT = now;
      holdFired = false;
    } else if (!holdFired && now - btnT < TAP_MAX_MS) {
      return TAP;
    }
    return NONE;
  }
  if (btnDown && !holdFired && now - btnT >= HOLD_MS) {
    holdFired = true;
    return HOLD;
  }
  return NONE;
}

float holdProgress() {
  uint32_t d = millis() - btnT;
  if (!btnDown || holdFired || d < 150) return 0;
  return min(1.0f, d / (float)HOLD_MS);
}

// ---------------- Game state ----------------

enum Screen { TITLE, HELP, PLAY, DEAD };
// Needs share indices with the actions that fix them.
enum Mood { HUNGRY, TIRED, DIRTY, HAPPY };
enum Face { F_OPEN, F_JOY, F_HUNGRY, F_TIRED, F_DIRTY, F_SLEEP, F_EAT, F_NO, F_DEAD };

const char *MOOD_NAME[] = {"HUNGRY", "TIRED", "DIRTY", "HAPPY"};
const char *ACT_NAME[] = {"FEED", "SLEEP", "BATH"};
const char *ACT_MSG[] = {"YUM!", "ZZZ...", "SPLASH"};
const char *STAGE_NAME[] = {"BABY", "KID", "ADULT"};
const int STAGE_R[] = {9, 12, 14};
const int STAGE_CARES[] = {0, 5, 12};

Screen screen = TITLE;
uint32_t frame;
int stateT;

Mood mood;
uint32_t moodT, happyLen, happyAcc, actT;
int cursor, score, best, cares, stage, warnSec, holdSec, popT, growT;
int act = -1;
bool actOk, newBest;

uint32_t nextHappyLen() {
  int lo = max(3000, 8000 - cares * 200);
  return lo + random(3000);
}

void newNeed(uint32_t now) {
  mood = (Mood)random(3);
  moodT = now;
  warnSec = 99;
  SFX(SFX_NEED);
}

void newGame() {
  mood = HAPPY;
  moodT = millis();
  happyLen = 5000;
  happyAcc = 0;
  cursor = score = cares = stage = popT = growT = holdSec = 0;
  act = -1;
  newBest = false;
  screen = PLAY;
  stateT = 0;
  SFX(SFX_START);
}

void die() {
  screen = DEAD;
  stateT = 0;
  act = -1;
  newBest = score > best;
  if (newBest) {
    best = score;
    prefs.putInt("best", best);
  }
  SFX(SFX_DEAD);
}

void startAct(uint32_t now) {
  act = cursor;
  actT = now;
  actOk = act == mood;
  if (!actOk) {
    SFX(SFX_NO);
    return;
  }
  mood = HAPPY;  // stops the death clock during the animation
  if (act == HUNGRY) SFX(SFX_FEED);
  else if (act == TIRED) SFX(SFX_SLEEP);
  else SFX(SFX_BATH);
}

void finishAct(uint32_t now) {
  if (actOk) {
    cares++;
    score += CARE_PTS;
    popT = 30;
    moodT = now;
    happyLen = nextHappyLen();
    happyAcc = 0;
    if (stage < 2 && cares >= STAGE_CARES[stage + 1]) {
      stage++;
      growT = 60;
      SFX(SFX_GROW);
    } else {
      SFX(SFX_HAPPY);
    }
  }
  act = -1;
}

void updatePlay(int ev, uint32_t now, uint32_t dt) {
  if (act >= 0) {
    if (now - actT >= (actOk ? ACT_MS : NO_MS)) finishAct(now);
  } else if (ev == TAP) {
    cursor = (cursor + 1) % 3;
    SFX(SFX_TICK);
  } else if (ev == HOLD) {
    startAct(now);
  }

  int hs = (btnDown && !holdFired && act < 0) ? (now - btnT) / 1000 : 0;
  if (hs > holdSec && hs < 3) beep(880 + hs * 330, 30);
  holdSec = hs;

  if (mood == HAPPY) {
    if (act >= 0) return;
    happyAcc += dt;
    score += happyAcc / 1000;
    happyAcc %= 1000;
    if (now - moodT >= happyLen) newNeed(now);
    return;
  }
  uint32_t sad = now - moodT;
  if (sad >= SAD_MS) {
    die();
    return;
  }
  int left = (SAD_MS - sad + 999) / 1000;
  if (left <= 3 && left != warnSec) {
    warnSec = left;
    if (act < 0) SFX(SFX_WARN);
  }
}

// ---------------- Drawing ----------------

void printC(const char *s, int cx, int y, int size = 1) {
  oled.setTextSize(size);
  oled.setCursor(cx - (int)strlen(s) * 3 * size, y);
  oled.print(s);
  oled.setTextSize(1);
}

void printR(const char *s, int rx, int y) {
  oled.setCursor(rx - (int)strlen(s) * 6 + 1, y);
  oled.print(s);
}

void drawApple(int x, int y) {
  // Black rim keeps the apple visible over the white head.
  oled.fillCircle(x - 2, y + 1, 6, BLACK);
  oled.fillCircle(x + 2, y + 1, 6, BLACK);
  oled.fillCircle(x - 2, y + 1, 5, WHITE);
  oled.fillCircle(x + 2, y + 1, 5, WHITE);
  oled.drawLine(x, y - 3, x + 1, y - 7, WHITE);
  oled.fillTriangle(x + 1, y - 6, x + 5, y - 8, x + 4, y - 5, WHITE);
}

void drawMoon(int x, int y) {
  oled.fillCircle(x, y, 7, WHITE);
  oled.fillCircle(x + 3, y - 2, 6, BLACK);
}

void drawDrop(int x, int y) {
  oled.fillCircle(x, y + 3, 5, WHITE);
  oled.fillTriangle(x - 5, y + 2, x + 5, y + 2, x, y - 8, WHITE);
  oled.drawFastVLine(x - 2, y + 2, 3, BLACK);
}

void drawHeart(int x, int y, int p) {
  oled.fillCircle(x - 3, y - 2, 3 + p, WHITE);
  oled.fillCircle(x + 3, y - 2, 3 + p, WHITE);
  oled.fillTriangle(x - 6 - p, y - 1, x + 6 + p, y - 1, x, y + 6 + p, WHITE);
}

void drawAlien(int cx, int cy, int r, int face, bool dirty) {
  bool droop = face == F_TIRED || face == F_SLEEP || face == F_DEAD;
  int ab = r / 2, ar = r >= 12 ? 2 : 1;
  int wob = (face == F_OPEN || face == F_JOY) ? lroundf(sinf(frame * 0.25f) * 2) : 0;
  for (int s = -1; s <= 1; s += 2) {
    int tx = droop ? cx + s * (r + 3) : cx + s * (ab + 3) + wob;
    int ty = droop ? cy - r + r / 3 : cy - r - r / 3 - 2;
    oled.drawLine(cx + s * ab, cy - r + 2, tx, ty, WHITE);
    oled.fillCircle(tx, ty, ar, WHITE);
  }

  int ay = cy + r / 4, up = (face == F_JOY || face == F_EAT) ? ((frame / 5) & 1 ? -6 : -4) : 3;
  oled.drawLine(cx - r - 2, ay, cx - r - 6, ay + up, WHITE);
  oled.drawLine(cx + r + 1, ay, cx + r + 5, ay + up, WHITE);
  if (face != F_DEAD) {
    oled.fillRoundRect(cx - ab - 3, cy + r - 2, 6, 4, 1, WHITE);
    oled.fillRoundRect(cx + ab - 2, cy + r - 2, 6, 4, 1, WHITE);
  }
  oled.fillRoundRect(cx - r - 2, cy - r, 2 * r + 4, 2 * r, r, WHITE);

  int ew = max(3, r / 2), eh = r * 2 / 3, ey = cy - r / 5, my = cy + r / 2 + 1, mw = r / 3 + 1;
  bool blink = (face == F_OPEN || face == F_HUNGRY || face == F_DIRTY) && frame % 90 < 3;
  for (int s = -1; s <= 1; s += 2) {
    int x = cx + s * (r * 2 / 5 + 1), l = x - ew / 2;
    switch (face) {
      case F_TIRED:
        oled.fillRoundRect(l, ey, ew, eh / 2 + 1, 1, BLACK);
        oled.drawFastHLine(l - 1, ey, ew + 2, BLACK);
        break;
      case F_SLEEP:
        oled.drawFastHLine(l, ey + 1, ew, BLACK);
        break;
      case F_JOY:
      case F_EAT:
        oled.drawLine(l, ey + 1, x, ey - 2, BLACK);
        oled.drawLine(x, ey - 2, l + ew - 1, ey + 1, BLACK);
        break;
      case F_DEAD:
        oled.drawLine(l, ey - ew / 2, l + ew - 1, ey + ew / 2, BLACK);
        oled.drawLine(l, ey + ew / 2, l + ew - 1, ey - ew / 2, BLACK);
        break;
      default:
        if (blink) {
          oled.drawFastHLine(l, ey, ew, BLACK);
          break;
        }
        oled.fillRoundRect(l, ey - eh / 2, ew, eh, ew / 2, BLACK);
        oled.drawPixel(l + 1, ey - eh / 2 + 1, WHITE);
        if (face == F_NO)
          oled.drawLine(x + s * (ew / 2 + 1), ey - eh / 2 - 3, x - s * (ew / 2 + 1), ey - eh / 2 - 1, BLACK);
    }
  }

  switch (face) {
    case F_OPEN:
      oled.drawCircleHelper(cx, my - mw / 2, mw, 0x4 | 0x8, BLACK);
      break;
    case F_JOY:
      for (int dy = 0; dy <= mw; dy++) {
        int hw = lroundf(sqrtf(mw * mw - dy * dy));
        oled.drawFastHLine(cx - hw, my - mw / 2 + dy, 2 * hw + 1, BLACK);
      }
      break;
    case F_HUNGRY:
      oled.fillCircle(cx, my, (r >= 12 ? 3 : 2) + ((frame / 8) & 1), BLACK);
      break;
    case F_EAT:
      if ((frame / 4) & 1) oled.fillCircle(cx, my, mw - 1, BLACK);
      else oled.drawFastHLine(cx - mw, my, 2 * mw + 1, BLACK);
      break;
    case F_TIRED:
      if (frame % 120 < 25) oled.fillRoundRect(cx - 2, my - 2, 5, 6, 2, BLACK);  // yawn
      else oled.drawFastHLine(cx - mw / 2, my, mw, BLACK);
      break;
    case F_SLEEP:
      oled.drawCircle(cx, my, 1, BLACK);
      break;
    case F_DIRTY:
      for (int i = -mw; i <= mw; i++) oled.drawPixel(cx + i, my + (((i + mw) / 2) & 1), BLACK);
      break;
    case F_NO:
    case F_DEAD:
      oled.drawCircleHelper(cx, my + mw / 2 + 1, mw, 0x1 | 0x2, BLACK);
      break;
  }

  if (!dirty) return;
  oled.fillCircle(cx, cy - r + 4, 1, BLACK);
  oled.fillCircle(cx - r + 1, cy + r / 3 + 1, 1, BLACK);
  oled.fillCircle(cx + r - 1, cy + r / 2, 1, BLACK);
  for (int s = -1; s <= 1; s += 2)
    for (int j = 0; j < 8; j++)
      oled.drawPixel(cx + s * (r + 9) + lroundf(sinf((j + frame * 0.4f) * 0.9f) * 1.5f), cy - r - 2 + j, WHITE);
  float a = frame * 0.22f;
  int fx = cx + lroundf(cosf(a) * (r + 9)), fy = cy - 2 + lroundf(sinf(a) * (r + 4));
  oled.fillRect(fx, fy, 2, 2, WHITE);
  int wy = fy - 1 - (frame & 1);
  oled.drawPixel(fx - 1, wy, WHITE);
  oled.drawPixel(fx + 2, wy, WHITE);
}

void drawBubble() {
  oled.fillRoundRect(BUB_X, BUB_Y, BUB_W, BUB_H, 6, BLACK);
  oled.drawRoundRect(BUB_X, BUB_Y, BUB_W, BUB_H, 6, WHITE);
  oled.drawCircle(BUB_X - 4, BUB_Y + BUB_H + 1, 2, WHITE);
  oled.drawCircle(BUB_X - 8, BUB_Y + BUB_H + 5, 1, WHITE);
}

void drawLeftPanel(uint32_t now) {
  if (growT > 0) {
    if ((frame / 6) & 1) {
      oled.setCursor(0, 14);
      oled.print("GREW");
      oled.setCursor(0, 23);
      oled.print("UP!");
    }
    return;
  }
  oled.setCursor(0, 14);
  if (act >= 0 && actOk) {
    oled.print(ACT_MSG[act]);
    return;
  }
  oled.print(MOOD_NAME[mood]);
  char buf[8];
  if (mood == HAPPY) {
    drawHeart(6, 30, 0);
    snprintf(buf, sizeof(buf), "x%d", cares);
    oled.setCursor(15, 27);
    oled.print(buf);
    return;
  }
  uint32_t sad = min(now - moodT, SAD_MS);
  int left = (SAD_MS - sad + 999) / 1000;
  if (left > 3 || ((frame / 4) & 1)) {
    snprintf(buf, sizeof(buf), "%d", left);
    oled.setTextSize(2);
    oled.setCursor(2, 26);
    oled.print(buf);
    oled.setTextSize(1);
    oled.setCursor(oled.getCursorX() + 1, 33);
    oled.print("s");
  }
  oled.drawRect(0, 44, 34, 5, WHITE);
  oled.fillRect(1, 45, 32 * (SAD_MS - sad) / SAD_MS, 3, WHITE);
}

void drawMenu() {
  float p = act < 0 ? holdProgress() : 0;
  for (int i = 0; i < 3; i++) {
    int x = i * (BOX_W + 1);
    bool s = i == cursor;
    if (s) oled.fillRoundRect(x, MENU_Y, BOX_W, 12, 2, WHITE);
    else oled.drawRoundRect(x, MENU_Y, BOX_W, 12, 2, WHITE);
    oled.setTextColor(s ? BLACK : WHITE);
    printC(ACT_NAME[i], x + BOX_W / 2, MENU_Y + 2);
    oled.setTextColor(WHITE);
    if (s && p > 0) oled.fillRect(x + 2, MENU_Y + 9, lroundf((BOX_W - 4) * p), 2, BLACK);
  }
}

void drawPlay(uint32_t now) {
  char buf[20];
  snprintf(buf, sizeof(buf), "SCORE %d", score);
  oled.setCursor(0, 0);
  oled.print(buf);
  printR(STAGE_NAME[stage], SCREEN_W, 0);
  oled.drawFastHLine(0, 9, SCREEN_W, WHITE);

  int r = STAGE_R[stage];
  if (growT > 30 && ((growT / 4) & 1)) r = STAGE_R[stage - 1];
  int cx = ALIEN_X, cy = ALIEN_Y, face;
  bool dirty = false, appleIn = false;
  float t = act >= 0 ? (now - actT) / (float)ACT_MS : 0;

  if (act >= 0 && !actOk) {
    face = F_NO;
    cx += (frame / 2) & 1 ? 2 : -2;
    dirty = mood == DIRTY;
  } else if (act == HUNGRY) {
    appleIn = t < 0.35f;
    face = appleIn ? F_HUNGRY : F_EAT;
  } else if (act == TIRED) {
    face = F_SLEEP;
    cy += 1;
  } else if (act == DIRTY) {
    face = F_JOY;
  } else {
    switch (mood) {
      case HUNGRY:
        face = F_HUNGRY;
        if (frame % 40 < 6) cx += (frame & 1) ? 1 : -1;  // tummy rumble
        break;
      case TIRED:
        face = F_TIRED;
        cy += 1;
        break;
      case DIRTY:
        face = F_DIRTY;
        dirty = true;
        break;
      default:
        face = frame % 150 < 30 ? F_JOY : F_OPEN;
        cy -= lroundf(fabsf(sinf(frame * 0.18f)) * 2);
    }
  }

  if (act == DIRTY && actOk) {
    oled.fillRoundRect(cx - 8, 10, 17, 3, 1, WHITE);
    for (int i = 0; i < 5; i++)
      oled.drawFastVLine(cx - 6 + i * 3, 14 + (frame * 3 + i * 9) % (2 * r + 6), 3, WHITE);
  }

  drawAlien(cx, cy, r, face, dirty);

  if (face == F_TIRED) {
    int ph = frame % 40;
    oled.setCursor(cx + r + 3 + ph / 10, cy - r + 4 - ph / 4);
    oled.print('z');
  }

  if (act >= 0 && actOk) {
    if (act == HUNGRY && appleIn) {
      float k = t / 0.35f;
      drawApple(lroundf(21 + (cx - 21) * k), lroundf(56 + (cy + r / 2 + 1 - 56) * k));
    } else if (act == TIRED) {
      drawMoon(BUB_CX, BUB_CY - 2);
      static const uint8_t STARS[][2] = {{100, 14}, {124, 34}, {103, 33}, {125, 15}, {88, 20}};
      for (int i = 0; i < COUNT(STARS); i++)
        if (((frame / 6) + i) & 1) oled.drawPixel(STARS[i][0], STARS[i][1], WHITE);
      for (int i = 0; i < 3; i++) {
        int ph = (frame + i * 12) % 36, y = cy - r + 6 - ph / 2;
        if (y < 11) continue;
        oled.setCursor(cx + r + 2 + ph / 6, y);
        oled.print('Z');
      }
    } else if (act == DIRTY) {
      for (int i = 0; i < 4; i++) {
        int s = (i & 1) ? 1 : -1;
        oled.drawCircle(cx + s * (r + 5 + i), cy + r - (frame * 2 + i * 13) % (2 * r + 6), 1 + (i >> 1), WHITE);
      }
    }
  } else {
    drawBubble();
    if (act >= 0) printC("NO!", BUB_CX + 1, BUB_CY - 3);
    else if (mood == HUNGRY) drawApple(BUB_CX, BUB_CY);
    else if (mood == TIRED) {
      drawMoon(BUB_CX - 2, BUB_CY + 1);
      oled.setCursor(BUB_CX + 4, BUB_CY - 9);
      oled.print('z');
    } else if (mood == DIRTY) drawDrop(BUB_CX, BUB_CY);
    else drawHeart(BUB_CX, BUB_CY - 1, (frame / 8) & 1);
  }

  if (popT > 0) {
    popT--;
    oled.setCursor(cx + r + 4, cy - 14 + popT / 3);
    oled.print("+10");
  }
  if (growT > 0) growT--;

  drawLeftPanel(now);
  drawMenu();
}

void drawTitle() {
  printC("ALIEN PET", 64, 0, 2);
  int bob = lroundf(fabsf(sinf(frame * 0.15f)) * 3);
  drawAlien(64, 37 - bob, 12, (frame / 40) & 1 ? F_JOY : F_OPEN, false);
  char buf[12];
  oled.setCursor(0, 24);
  oled.print("BEST");
  snprintf(buf, sizeof(buf), "%d", best);
  oled.setCursor(0, 33);
  oled.print(buf);
  drawHeart(110, 32, (frame / 8) & 1);
  if ((frame / 12) & 1) printC("PRESS TO START", 64, 56);
}

void drawHelp() {
  printC("HOW TO PLAY", 64, 0);
  oled.drawFastHLine(31, 9, 66, WHITE);
  printC("TAP = NEXT ACTION", 64, 12);
  printC("HOLD 3s = DO IT", 64, 21);
  printC("MATCH HIS MOOD!", 64, 30);
  printC("SAD 10s = GAME OVER", 64, 39);
  if ((frame / 12) & 1) printC("PRESS TO START", 64, 55);
}

void drawDead() {
  printC("GAME OVER", 64, 0, 2);
  int gy = 42 - min(stateT, 30) / 3 + lroundf(sinf(frame * 0.15f) * 2);
  drawAlien(22, gy, 9, F_DEAD, false);
  oled.drawRoundRect(16, gy - 16, 13, 4, 2, WHITE);
  char buf[20];
  snprintf(buf, sizeof(buf), "SCORE %d", score);
  oled.setCursor(46, 22);
  oled.print(buf);
  snprintf(buf, sizeof(buf), "BEST  %d", best);
  oled.setCursor(46, 32);
  oled.print(buf);
  if (newBest && ((frame / 8) & 1)) {
    oled.setCursor(46, 42);
    oled.print("NEW BEST!");
  }
  if (stateT > 45 && ((frame / 12) & 1)) printC("PRESS TO RETRY", 64, 56);
}

// ---------------- Main ----------------

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  ledcAttach(BUZZER_PIN, 2000, 10);
  ledcWriteTone(BUZZER_PIN, 0);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed");
    for (;;) delay(1000);
  }
  oled.setTextColor(WHITE);
  oled.setTextWrap(false);
  randomSeed(esp_random());

  prefs.begin("alienpet");
  best = prefs.getInt("best", 0);
  Serial.println("ALIEN PET ready");
}

void loop() {
  static uint32_t nextFrame = 0, lastT = 0;
  static int ev = NONE;
  updateSound();
  int e = pollButton();
  if (e) ev = e;

  uint32_t now = millis();
  if ((int32_t)(now - nextFrame) < 0) {
    delay(1);
    return;
  }
  nextFrame = now - nextFrame > 100 ? now + FRAME_MS : nextFrame + FRAME_MS;
  uint32_t dt = now - lastT;
  lastT = now;
  frame++;
  stateT++;
  int press = ev;
  ev = NONE;

  oled.clearDisplay();
  switch (screen) {
    case TITLE:
      drawTitle();
      if (press) {
        screen = HELP;
        stateT = 0;
        SFX(SFX_TICK);
      }
      break;
    case HELP:
      drawHelp();
      if (press && stateT > 10) newGame();
      break;
    case PLAY:
      updatePlay(press, now, dt);
      if (screen == PLAY) drawPlay(now);
      else drawDead();
      break;
    case DEAD:
      drawDead();
      if (press && stateT > 45) newGame();
      else if (stateT > 900) screen = TITLE;
      break;
  }
  oled.display();
}
