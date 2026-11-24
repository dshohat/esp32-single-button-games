#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>

// LANE RACER: dodge traffic on a two-lane road (ESP32 + SSD1306 + passive buzzer).
// Each press switches lane. Grab coins for bonus points.

const int BUTTON_PIN = 4;   // other leg to GND
const int BUZZER_PIN = 23;  // passive buzzer

const int SCREEN_W = 128, SCREEN_H = 64;
const int ROAD_TOP = 10, ROAD_BOT = 63, MID_Y = 36;
const int LANE_Y[2] = {19, 46};  // top row of a car in each lane
const int PLAYER_X = 6, CAR_W = 18, CAR_H = 9, TRUCK_W = 28, TRUCK_H = 11, COIN_S = 7;
const float LANE_V = 4.5f;
const float START_SPEED = 2.5f, MAX_SPEED = 6.5f, ACCEL = 0.0015f;
const int FRAME_MS = 33;

#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);
Preferences prefs;

// ---------------- Sprites ----------------

const char *const PLAYER[] = {
    "..###.......###...",
    ".###############..",
    "#.....#......#.##.",
    "#....##......##..#",
    "#....##......##..#",
    "#....##......##..#",
    "#.....#......#.##.",
    ".###############..",
    "..###.......###...",
};
const char *const CAR[] = {
    "..###.......###...",
    ".################.",
    "##################",
    "###..#######..####",
    "###..#######..####",
    "###..#######..####",
    "##################",
    ".################.",
    "..###.......###...",
};

// ---------------- Sound ----------------

struct Note {
  uint16_t f, ms;
};

const Note SFX_START[] = {{523, 60}, {659, 60}, {784, 60}, {1047, 120}};
const Note SFX_SWITCH[] = {{1200, 15}, {900, 20}};
const Note SFX_COIN[] = {{1319, 50}, {1976, 120}};
const Note SFX_HIT[] = {{200, 40}, {150, 40}, {300, 30}, {100, 60}, {80, 150}};
const Note SFX_RECORD[] = {{1047, 90}, {1319, 90}, {1568, 90}, {2093, 140}, {1568, 90}, {2093, 260}};

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

// ---------------- Button ----------------

bool btnDown, evPress;

// Debounced: after an edge, changes are ignored for 25 ms.
void readButton() {
  static uint32_t edgeT;
  bool raw = digitalRead(BUTTON_PIN) == LOW;
  if (raw == btnDown || millis() - edgeT < 25) return;
  btnDown = raw;
  edgeT = millis();
  if (raw) evPress = true;
}

// ---------------- Game state ----------------

enum State { TITLE, HELP, PLAY, OVER };
State state = TITLE;
uint32_t frame;
int stateT;

struct Obs {
  float x;
  bool truck;
  uint8_t lane;
  bool on;
};
Obs obs[6];

struct Coin {
  float x;
  uint8_t lane;
  bool on;
};
Coin coins[4];

int lane, best, coinsGot, flashT;
float carY, speed, roadX, dist, nextGap, score;
uint32_t playT;
bool newBest;

// ---------------- Gameplay ----------------

void startGame() {
  state = PLAY;
  stateT = 0;
  lane = 1;
  carY = LANE_Y[1];
  speed = START_SPEED;
  roadX = dist = score = 0;
  nextGap = 40;
  playT = 0;
  coinsGot = flashT = 0;
  newBest = false;
  for (auto &o : obs) o.on = false;
  for (auto &c : coins) c.on = false;
  SFX(SFX_START);
}

void spawnObstacle() {
  Obs o = {(float)SCREEN_W, score > 150 && random(100) < 30, (uint8_t)random(2), true};
  for (auto &slot : obs)
    if (!slot.on) {
      slot = o;
      break;
    }
  int w = o.truck ? TRUCK_W : CAR_W;
  if (random(100) < 35)
    for (auto &c : coins)
      if (!c.on) {
        c = {(float)SCREEN_W + w / 2 - COIN_S / 2, (uint8_t)(1 - o.lane), true};
        break;
      }
  // Leave room behind this obstacle for the player to swap lanes.
  nextGap = w + CAR_W + speed * (27 / LANE_V) + 16 + random((int)(speed * 18));
}

bool overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
  return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

void die() {
  state = OVER;
  stateT = 0;
  newBest = (int)score > best;
  if (newBest) {
    best = score;
    prefs.putInt("best", best);
    SFX(SFX_RECORD);
  } else {
    SFX(SFX_HIT);
  }
}

void updatePlay() {
  if (evPress) {
    lane ^= 1;
    SFX(SFX_SWITCH);
  }
  float target = LANE_Y[lane];
  if (fabsf(target - carY) <= LANE_V) carY = target;
  else carY += target > carY ? LANE_V : -LANE_V;

  playT++;
  speed = min(MAX_SPEED, START_SPEED + playT * ACCEL);
  roadX += speed;
  for (auto &o : obs) {
    if (!o.on) continue;
    o.x -= speed;
    if (o.x + TRUCK_W < 0) o.on = false;
  }
  for (auto &c : coins) {
    if (!c.on) continue;
    c.x -= speed;
    if (c.x + COIN_S < 0) c.on = false;
  }
  dist += speed;
  if (dist >= nextGap) {
    dist = 0;
    spawnObstacle();
  }
  score += speed * 0.1f;
  if (flashT > 0) flashT--;

  int py = lroundf(carY);
  for (auto &c : coins) {
    if (c.on && overlap(PLAYER_X, py, CAR_W, CAR_H, lroundf(c.x), LANE_Y[c.lane] + 1, COIN_S, COIN_S)) {
      c.on = false;
      score += 25;
      coinsGot++;
      flashT = 20;
      SFX(SFX_COIN);
    }
  }
  // Hitboxes are shrunk by 1px on each side to forgive grazes.
  for (auto &o : obs) {
    if (!o.on) continue;
    int w = o.truck ? TRUCK_W : CAR_W, h = o.truck ? TRUCK_H : CAR_H;
    int oy = LANE_Y[o.lane] - (o.truck ? 1 : 0);
    if (overlap(PLAYER_X + 1, py + 1, CAR_W - 2, CAR_H - 2, lroundf(o.x) + 1, oy + 1, w - 2, h - 2)) {
      die();
      return;
    }
  }
}

void goTitle() {
  state = TITLE;
  stateT = 0;
  oled.invertDisplay(false);
}

// ---------------- Drawing ----------------

void drawSprite(const char *const *rows, int n, int x, int y) {
  for (int r = 0; r < n; r++)
    for (int c = 0; rows[r][c]; c++)
      if (rows[r][c] == '#') oled.drawPixel(x + c, y + r, WHITE);
}

void drawTruck(int x, int y) {
  oled.drawRect(x, y, 20, TRUCK_H, WHITE);
  for (int i = 4; i < 20; i += 4) oled.drawFastVLine(x + i, y + 2, TRUCK_H - 4, WHITE);
  oled.fillRect(x + 21, y + 1, 7, TRUCK_H - 2, WHITE);
  oled.drawFastVLine(x + 25, y + 2, TRUCK_H - 4, BLACK);  // windshield
}

void drawCoin(int x, int y) {
  static const int8_t HW[] = {3, 2, 1, 2};  // spinning half-width
  int hw = HW[(frame / 4) % 4];
  oled.drawRoundRect(x + 3 - hw, y, hw * 2 + 1, COIN_S, hw, WHITE);
}

void drawDashes(int y) {
  int off = (int)roadX % 16;
  for (int x = -off; x < SCREEN_W; x += 16) oled.drawFastHLine(x, y, 8, WHITE);
}

void drawRoad() {
  oled.drawFastHLine(0, ROAD_TOP, SCREEN_W, WHITE);
  oled.drawFastHLine(0, ROAD_BOT, SCREEN_W, WHITE);
  drawDashes(MID_Y);
  int off = (int)roadX % 32;
  for (int x = -off; x < SCREEN_W; x += 32) oled.drawFastVLine(x, ROAD_TOP - 2, 2, WHITE);
}

void drawPlayer() {
  int py = lroundf(carY);
  drawSprite(PLAYER, COUNT(PLAYER), PLAYER_X, py);
  if (state == PLAY && (frame & 2)) oled.drawPixel(PLAYER_X - 2, py + 4, WHITE);  // exhaust
  if (state == OVER && stateT < 20 && (stateT & 2)) {
    int sx = PLAYER_X + CAR_W, sy = py + 4;
    oled.drawLine(sx - 3, sy - 3, sx + 3, sy + 3, WHITE);
    oled.drawLine(sx - 3, sy + 3, sx + 3, sy - 3, WHITE);
    oled.drawFastHLine(sx - 4, sy, 9, WHITE);
  }
}

void drawHud() {
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.printf("HI %05d", best);
  oled.setCursor(62, 0);
  oled.printf("$%d", coinsGot);
  if (flashT == 0 || (flashT / 3) & 1) {
    oled.setCursor(98, 0);
    oled.printf("%05d", (int)score);
  }
}

void printCentered(const char *s, int y) {
  oled.setCursor(64 - strlen(s) * 3, y);
  oled.print(s);
}

void drawScene() {
  drawRoad();
  for (auto &c : coins)
    if (c.on) drawCoin(lroundf(c.x), LANE_Y[c.lane] + 1);
  for (auto &o : obs) {
    if (!o.on) continue;
    if (o.truck) drawTruck(lroundf(o.x), LANE_Y[o.lane] - 1);
    else drawSprite(CAR, COUNT(CAR), lroundf(o.x), LANE_Y[o.lane]);
  }
  drawPlayer();
  drawHud();
}

void drawGameOver() {
  if (stateT < 15) return;
  char buf[20];
  oled.fillRect(14, 15, 100, 36, BLACK);
  oled.drawRect(14, 15, 100, 36, WHITE);
  printCentered("GAME OVER", 19);
  snprintf(buf, sizeof(buf), "SCORE %d", (int)score);
  printCentered(buf, 29);
  if (!newBest) {
    snprintf(buf, sizeof(buf), "COINS %d", coinsGot);
    printCentered(buf, 39);
  } else if ((stateT / 8) & 1) {
    printCentered("NEW HI SCORE!", 39);
  }
}

void drawTitle() {
  oled.setTextSize(2);
  oled.setCursor(4, 2);
  oled.print("LANE RACER");
  oled.setTextSize(1);

  char buf[16];
  snprintf(buf, sizeof(buf), "HI %05d", best);
  printCentered(buf, 21);
  if ((frame / 12) & 1) printCentered("PRESS TO START", 31);

  roadX += 2;
  oled.drawFastHLine(0, 42, SCREEN_W, WHITE);
  oled.drawFastHLine(0, ROAD_BOT, SCREEN_W, WHITE);
  carY = LANE_Y[1];
  drawPlayer();
  drawSprite(CAR, COUNT(CAR), 96, LANE_Y[1]);
}

void drawHelp() {
  printCentered("HOW TO PLAY", 0);
  oled.drawFastHLine(31, 9, 66, WHITE);
  printCentered("PRESS = CHANGE LANE", 14);
  printCentered("AVOID CARS & TRUCKS", 24);
  printCentered("GRAB $ = +25", 34);
  if ((frame / 12) & 1) printCentered("PRESS TO START", 52);
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

  prefs.begin("racer");
  best = prefs.getInt("best", 0);
  Serial.println("LANE RACER ready");
}

void loop() {
  static uint32_t nextFrame = 0;
  updateSound();
  readButton();

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
      if (evPress) {
        state = HELP;
        stateT = 0;
      }
      break;
    case HELP:
      drawHelp();
      if (evPress && stateT > 10) startGame();
      break;
    case PLAY:
      updatePlay();
      drawScene();
      break;
    case OVER:
      if (stateT <= 10) oled.invertDisplay(stateT < 10 && (stateT & 2));
      drawScene();
      drawGameOver();
      if (evPress && stateT > 20) startGame();
      else if (stateT > 600) goTitle();
      break;
  }
  evPress = false;
  oled.display();
}
