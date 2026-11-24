#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>

// DINO RUN: the Chrome offline dino game (ESP32 + SSD1306 + passive buzzer).
// Tap = small jump, hold = big jump. Avoid cacti; stay down under high birds.

const int BUTTON_PIN = 4;   // other leg to GND
const int BUZZER_PIN = 23;  // passive buzzer

const int SCREEN_W = 128, SCREEN_H = 64;
const int GROUND = 56;  // y of the ground line; sprites stand with their bottom row on it
const int DINO_X = 10;
const float JUMP_V = 5.5f, CUT_V = 3.0f, GRAVITY = 0.55f;
const float START_SPEED = 3.0f, MAX_SPEED = 7.0f, ACCEL = 0.0012f;
const int FRAME_MS = 33;

#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);
Preferences prefs;

// ---------------- Sprites ----------------

const char *const DINO[] = {
    "........#######.",
    "........#.######",
    "........########",
    "........########",
    "........####....",
    "........######..",
    "#......#####....",
    "#.....######....",
    "##...########...",
    "###.#######.#...",
    "##########......",
    ".#########......",
    "..########......",
    "...######.......",
};
const char *const LEGS_STAND[] = {"....#...#.......", "....##..##......"};
const char *const LEGS_A[] = {"....#...##......", "....##.........."};
const char *const LEGS_B[] = {"....##..#.......", "........##......"};

const char *const CACTUS_S[] = {
    "...#...",
    "..###..",
    "..###.#",
    "#.###.#",
    "#.###.#",
    "#.#####",
    "#.###..",
    "#####..",
    "..###..",
    "..###..",
    "..###..",
    "..###..",
    "..###..",
};
const char *const CACTUS_L[] = {
    "....#....",
    "...###...",
    "...###..#",
    "#..###..#",
    "#..###..#",
    "#..###..#",
    "#..######",
    "#..###...",
    "######...",
    "...###...",
    "...###...",
    "...###...",
    "...###...",
    "...###...",
    "...###...",
    "...###...",
    "...###...",
};
// BIRD_DOWN is drawn 2px lower so both frames share the body row.
const char *const BIRD_UP[] = {
    "......#......",
    "......##.....",
    "..#...###....",
    ".##...####...",
    "#############",
    "....#########",
    ".....#######.",
};
const char *const BIRD_DOWN[] = {
    "..#..........",
    ".##..........",
    "#############",
    "....#########",
    ".....#######.",
    "......###....",
    "......##.....",
    "......#......",
};
const char *const CLOUD[] = {
    "....####....",
    "..##....##..",
    ".#........#.",
    "############",
};

// ---------------- Sound ----------------

struct Note {
  uint16_t f, ms;
};

const Note SFX_START[] = {{523, 60}, {659, 60}, {784, 60}, {1047, 120}};
const Note SFX_JUMP[] = {{784, 25}, {1175, 35}};
const Note SFX_POINT[] = {{1047, 70}, {1568, 140}};
const Note SFX_HIT[] = {{330, 60}, {220, 80}, {147, 120}, {110, 200}};
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

enum ObsType : uint8_t { SMALL, LARGE, BIRD };
struct Obs {
  float x;
  ObsType type;
  uint8_t count;
  int8_t y;  // birds only
  bool on;
};
Obs obs[4];

struct Cloud {
  float x;
  int8_t y;
};
Cloud clouds[3];

float h, vh;  // dino height above the ground and its upward speed
bool onGround, night, newBest;
int jumpBuf, flashT, best;
float speed, groundX, dist, nextGap, score;
uint32_t playT;

uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

int obsWidth(const Obs &o) {
  if (o.type == SMALL) return o.count * 8 - 1;
  if (o.type == LARGE) return o.count * 10 - 1;
  return 13;
}

void setNight(bool on) {
  if (on == night) return;
  night = on;
  oled.invertDisplay(on);
}

// ---------------- Gameplay ----------------

void resetClouds() {
  for (int i = 0; i < COUNT(clouds); i++) clouds[i] = {20.0f + i * 45, (int8_t)(10 + random(18))};
}

void startGame() {
  state = PLAY;
  stateT = 0;
  h = vh = 0;
  onGround = true;
  jumpBuf = flashT = 0;
  speed = START_SPEED;
  groundX = dist = score = 0;
  nextGap = 60;
  playT = 0;
  newBest = false;
  for (auto &o : obs) o.on = false;
  resetClouds();
  setNight(false);
  SFX(SFX_START);
}

void spawnObstacle() {
  Obs o = {(float)SCREEN_W + 2, SMALL, 1, 0, true};
  int r = random(100);
  if (score > 300 && r < 20) {
    o.type = BIRD;
    o.y = random(2) ? GROUND - 13 : GROUND - 34;
  } else if (r < 60) {
    o.count = random(1, speed > 4 ? 4 : 3);
  } else {
    o.type = LARGE;
    o.count = random(1, speed > 4.5f ? 3 : 2);
  }
  for (auto &slot : obs)
    if (!slot.on) {
      slot = o;
      break;
    }
  nextGap = obsWidth(o) + (22 * speed + 16) * (1 + random(60) / 100.0f);
}

void updatePlay() {
  if (evPress) jumpBuf = 5;  // a press just before landing still counts
  if (jumpBuf > 0 && onGround) {
    vh = JUMP_V;
    onGround = false;
    jumpBuf = 0;
    SFX(SFX_JUMP);
  } else if (jumpBuf > 0) {
    jumpBuf--;
  }
  if (!onGround) {
    h += vh;
    vh -= GRAVITY;
    if (!btnDown && vh > CUT_V) vh = CUT_V;
    if (h <= 0) {
      h = vh = 0;
      onGround = true;
    }
  }

  playT++;
  speed = min(MAX_SPEED, START_SPEED + playT * ACCEL);
  groundX += speed;
  for (auto &c : clouds) {
    c.x -= speed * 0.2f;
    if (c.x < -12) c = {(float)SCREEN_W + random(60), (int8_t)(10 + random(18))};
  }
  for (auto &o : obs) {
    if (!o.on) continue;
    o.x -= o.type == BIRD ? speed + 0.6f : speed;
    if (o.x + obsWidth(o) < 0) o.on = false;
  }
  dist += speed;
  if (dist >= nextGap) {
    dist = 0;
    spawnObstacle();
  }

  int before = (int)score / 100;
  score += speed * 0.11f;
  if ((int)score / 100 > before) {
    flashT = 40;
    SFX(SFX_POINT);
  }
  if (flashT > 0) flashT--;
  setNight((int)score >= 700 && ((int)score / 700) & 1);
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

void goTitle() {
  state = TITLE;
  stateT = 0;
  h = 0;
  setNight(false);
}

// ---------------- Drawing ----------------

// Returns true if any pixel landed on an already lit pixel (used for collision).
bool drawSprite(const char *const *rows, int n, int x, int y, bool collide = false) {
  bool hit = false;
  for (int r = 0; r < n; r++)
    for (int c = 0; rows[r][c]; c++) {
      if (rows[r][c] != '#') continue;
      if (collide && oled.getPixel(x + c, y + r)) hit = true;
      oled.drawPixel(x + c, y + r, WHITE);
    }
  return hit;
}

void drawObstacles() {
  for (auto &o : obs) {
    if (!o.on) continue;
    int x = lroundf(o.x);
    for (int k = 0; k < o.count && o.type == SMALL; k++)
      drawSprite(CACTUS_S, COUNT(CACTUS_S), x + k * 8, GROUND - 12);
    for (int k = 0; k < o.count && o.type == LARGE; k++)
      drawSprite(CACTUS_L, COUNT(CACTUS_L), x + k * 10, GROUND - 16);
    if (o.type == BIRD) {
      if ((frame / 6) & 1) drawSprite(BIRD_UP, COUNT(BIRD_UP), x, o.y);
      else drawSprite(BIRD_DOWN, COUNT(BIRD_DOWN), x, o.y + 2);
    }
  }
}

bool drawDino(bool collide) {
  int y = GROUND - 15 - lroundf(h);
  const char *const *legs = LEGS_STAND;
  if (state == PLAY && onGround) legs = (frame / 3) & 1 ? LEGS_A : LEGS_B;
  bool hit = drawSprite(DINO, COUNT(DINO), DINO_X, y, collide);
  hit |= drawSprite(legs, 2, DINO_X, y + 14, collide);
  if (state == OVER) {
    oled.fillRect(DINO_X + 9, y, 3, 3, BLACK);
    oled.drawPixel(DINO_X + 10, y + 1, WHITE);
  } else if (state == TITLE && frame % 70 < 4) {
    oled.drawPixel(DINO_X + 9, y + 1, WHITE);  // blink
  }
  return hit;
}

void drawGround() {
  oled.drawFastHLine(0, GROUND, SCREEN_W, WHITE);
  int gx = (int)groundX;
  for (int x = 0; x < SCREEN_W; x++) {
    uint32_t r = hash32(x + gx);
    if (r % 11 == 0) oled.drawPixel(x, GROUND + 2 + (r >> 8) % 5, WHITE);
    else if (r % 53 == 0) oled.drawFastHLine(x, GROUND + 2 + (r >> 8) % 5, 3, WHITE);
    if (r % 89 == 0) oled.drawPixel(x, GROUND - 1, WHITE);
  }
}

void drawHud() {
  oled.setTextSize(1);
  oled.setCursor(44, 0);
  oled.printf("HI %05d", best);
  int shown = flashT > 0 ? (int)score / 100 * 100 : (int)score;
  if (flashT == 0 || (flashT / 5) & 1) {
    oled.setCursor(98, 0);
    oled.printf("%05d", shown);
  }
}

void printCentered(const char *s, int y) {
  oled.setCursor(64 - strlen(s) * 3, y);
  oled.print(s);
}

void drawScene(bool collide) {
  drawObstacles();
  bool hit = drawDino(collide);
  for (auto &c : clouds) drawSprite(CLOUD, COUNT(CLOUD), lroundf(c.x), c.y);
  drawGround();
  drawHud();
  if (hit) die();
}

void drawGameOver() {
  oled.fillRect(11, 12, 106, 11, BLACK);
  printCentered("G A M E   O V E R", 14);

  oled.fillCircle(64, 34, 9, BLACK);
  oled.drawCircle(64, 34, 6, WHITE);
  oled.fillRect(65, 27, 6, 5, BLACK);
  oled.fillTriangle(64, 25, 64, 31, 68, 28, WHITE);

  if (newBest && (stateT / 8) & 1) {
    oled.fillRect(25, 45, 78, 9, BLACK);
    printCentered("NEW HI SCORE!", 46);
  }
}

void drawTitle() {
  oled.setTextSize(2);
  oled.setCursor(16, 2);
  oled.print("DINO RUN");
  oled.setTextSize(1);

  char buf[16];
  snprintf(buf, sizeof(buf), "HI %05d", best);
  printCentered(buf, 21);
  if ((frame / 12) & 1) printCentered("PRESS TO START", 31);

  drawDino(false);
  drawSprite(CACTUS_L, COUNT(CACTUS_L), 96, GROUND - 16);
  drawSprite(CACTUS_S, COUNT(CACTUS_S), 108, GROUND - 12);
  drawGround();
}

void drawHelp() {
  printCentered("HOW TO PLAY", 0);
  oled.drawFastHLine(31, 9, 66, WHITE);
  printCentered("TAP  = SMALL JUMP", 13);
  printCentered("HOLD = BIG JUMP", 22);
  printCentered("JUMP OVER CACTI", 31);
  printCentered("HIGH BIRD? STAY DOWN", 40);
  if ((frame / 12) & 1) printCentered("PRESS TO START", 55);
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

  prefs.begin("dino");
  best = prefs.getInt("best", 0);
  resetClouds();
  Serial.println("DINO RUN ready");
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
      drawScene(true);
      break;
    case OVER:
      drawScene(false);
      drawGameOver();
      if (evPress && stateT > 15) startGame();
      else if (stateT > 600) goTitle();
      break;
  }
  evPress = false;
  oled.display();
}
