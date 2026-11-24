#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>

// STACK: a one-button tower builder (ESP32 + SSD1306 + passive buzzer).
// Press to drop the sliding block; whatever overhangs the tower is cut off.

const int BUTTON_PIN = 4;   // other leg to GND
const int BUZZER_PIN = 23;  // passive buzzer

const int SCREEN_W = 128, SCREEN_H = 64;
const int BLOCK_H = 6;
const int BASE_Y = 58;      // world y of the ground
const int CAM_LINE = 26;    // the camera keeps the sliding block at or below this screen y
const int START_W = 48;
const int PERFECT_TOL = 2;  // px of error still counted as a perfect drop
const int FRAME_MS = 33;

#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);
Preferences prefs;

// ---------------- Sound ----------------

struct Note {
  uint16_t f, ms;
};

const Note SFX_START[] = {{523, 60}, {659, 60}, {784, 60}, {1047, 120}};
const Note SFX_CUT[] = {{392, 30}, {262, 50}};
const Note SFX_MISS[] = {{440, 70}, {330, 70}, {247, 90}, {165, 110}, {110, 200}};
const Note SFX_RECORD[] = {{1047, 90}, {1319, 90}, {1568, 90}, {2093, 140}, {1568, 90}, {2093, 260}};
const uint16_t SCALE[] = {523, 587, 659, 698, 784, 880, 988, 1047, 1175, 1319, 1397, 1568};

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

// True once per press; after an edge, changes are ignored for 25 ms (debounce).
bool buttonPressed() {
  static bool down;
  static uint32_t edgeT;
  bool raw = digitalRead(BUTTON_PIN) == LOW;
  if (raw == down || millis() - edgeT < 25) return false;
  down = raw;
  edgeT = millis();
  return raw;
}

// ---------------- Game state ----------------

enum State { TITLE, HELP, PLAY, FALL, OVER };
State state = TITLE;
uint32_t frame;
int stateT;

int16_t towerX[16], towerW[16];  // ring buffer of the most recent layers
int layers;                      // placed layers, including the base
float curX, speed, cam;
int dir, combo, best, glowT, glowLayer, msgT;
bool newBest;

struct Debris {
  float x, y, vx, vy;  // y is in world coordinates
  int16_t w;
  bool on;
};
Debris debris[4];

int layerY(int i) { return BASE_Y - (i + 1) * BLOCK_H; }

uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

void addDebris(int x, int y, int w, float vx) {
  for (auto &d : debris)
    if (!d.on) {
      d = {(float)x, (float)y, vx, 0, (int16_t)w, true};
      return;
    }
}

// ---------------- Gameplay ----------------

void nextBlock() {
  dir = (layers & 1) ? 1 : -1;
  curX = dir > 0 ? 0 : SCREEN_W - towerW[(layers - 1) & 15];
  speed = min(1.8f + layers * 0.05f, 3.6f);
}

void startGame() {
  state = PLAY;
  stateT = 0;
  layers = 1;
  combo = glowT = msgT = 0;
  cam = 0;
  newBest = false;
  towerX[0] = (SCREEN_W - START_W) / 2;
  towerW[0] = START_W;
  for (auto &d : debris) d.on = false;
  nextBlock();
  SFX(SFX_START);
}

void drop() {
  int top = (layers - 1) & 15, w = towerW[top];
  int x = lroundf(curX), d = x - towerX[top], y = layerY(layers);

  if (abs(d) <= PERFECT_TOL) {
    x = towerX[top];
    combo++;
    // A streak of perfect drops wins some width back.
    if (combo >= 3 && w < START_W) {
      w += 4;
      x = constrain(x - 2, 0, SCREEN_W - w);
    }
    glowT = 6;
    glowLayer = layers;
    msgT = 24;
    uint16_t f = SCALE[min(combo, COUNT(SCALE)) - 1];
    Note chime[] = {{f, 45}, {(uint16_t)(f * 2), 90}};
    SFX(chime);
  } else if (abs(d) >= w) {
    addDebris(x, y, w, 0);
    state = FALL;
    stateT = 0;
    SFX(SFX_MISS);
    return;
  } else {
    combo = 0;
    if (d > 0) {
      addDebris(x + w - d, y, d, 0.6f);
    } else {
      addDebris(x, y, -d, -0.6f);
      x = towerX[top];
    }
    w -= abs(d);
    SFX(SFX_CUT);
  }

  towerX[layers & 15] = x;
  towerW[layers & 15] = w;
  layers++;
  nextBlock();
}

void updatePlay(bool press) {
  int w = towerW[(layers - 1) & 15];
  curX += dir * speed;
  if (curX <= 0) {
    curX = 0;
    dir = 1;
  } else if (curX >= SCREEN_W - w) {
    curX = SCREEN_W - w;
    dir = -1;
  }
  if (press) drop();
  cam += (max(0, CAM_LINE - layerY(layers)) - cam) * 0.2f;
}

void endGame() {
  state = OVER;
  stateT = 0;
  newBest = layers - 1 > best;
  if (newBest) {
    best = layers - 1;
    prefs.putInt("best", best);
    SFX(SFX_RECORD);
  }
}

// ---------------- Drawing ----------------

void drawScene() {
  int c = lroundf(cam);

  // Stars drift down at half speed as the tower grows.
  for (int i = 0; i < 16; i++) {
    uint32_t h = hash32(i + 1);
    oled.drawPixel(h % SCREEN_W, ((h >> 8) + c / 2) % SCREEN_H, WHITE);
  }

  int gy = BASE_Y + c;
  oled.drawFastHLine(0, gy, SCREEN_W, WHITE);
  for (int y = gy + 2; y < SCREEN_H; y += 2)
    for (int x = (y - gy) & 2; x < SCREEN_W; x += 4) oled.drawPixel(x, y, WHITE);

  for (int i = max(0, layers - 12); i < layers; i++)
    oled.fillRect(towerX[i & 15], layerY(i) + c, towerW[i & 15], BLOCK_H - 1, WHITE);
  if (state == PLAY)
    oled.fillRect(lroundf(curX), layerY(layers) + c, towerW[(layers - 1) & 15], BLOCK_H - 1, WHITE);

  if (glowT > 0) {
    int i = glowLayer & 15, k = 7 - glowT;
    glowT--;
    oled.drawRect(towerX[i] - k, layerY(glowLayer) + c - k, towerW[i] + 2 * k, BLOCK_H - 1 + 2 * k, WHITE);
  }

  for (auto &d : debris) {
    if (!d.on) continue;
    d.vy += 0.35f;
    d.x += d.vx;
    d.y += d.vy;
    if (d.y + c > SCREEN_H) d.on = false;
    else oled.fillRect((int)d.x, (int)d.y + c, d.w, BLOCK_H - 1, WHITE);
  }

  oled.setTextSize(2);
  oled.setCursor(1, 0);
  oled.print(layers - 1);
  oled.setTextSize(1);
  if (msgT > 0) {
    msgT--;
    char msg[20];
    if (combo > 1) snprintf(msg, sizeof(msg), "PERFECT x%d", combo);
    else strcpy(msg, "PERFECT");
    oled.setCursor(SCREEN_W - strlen(msg) * 6, 0);
    oled.print(msg);
  }
}

void drawGameOver() {
  int y = 8 + max(0, 48 - stateT * 8);
  oled.fillRoundRect(4, y, 120, 48, 5, BLACK);
  oled.drawRoundRect(4, y, 120, 48, 5, WHITE);
  oled.setTextSize(2);
  oled.setCursor(11, y + 5);
  oled.print("GAME OVER");
  oled.setTextSize(1);
  oled.setCursor(31, y + 25);
  oled.printf("SCORE  %4d", layers - 1);
  oled.setCursor(31, y + 36);
  if (!newBest) oled.printf("BEST   %4d", best);
  else if ((stateT / 8) & 1) oled.print("NEW RECORD!");
}

void drawTitle() {
  oled.setTextSize(3);
  oled.setCursor(20, 3);
  oled.print("STACK");
  oled.setTextSize(1);

  oled.fillRect(44, 37, 40, BLOCK_H - 1, WHITE);
  oled.fillRect(44 + lroundf(sinf(frame * 0.09f) * 36), 31, 40, BLOCK_H - 1, WHITE);

  char buf[16];
  snprintf(buf, sizeof(buf), "BEST %d", best);
  oled.setCursor(64 - strlen(buf) * 3, 46);
  oled.print(buf);
  if ((frame / 12) & 1) {
    oled.setCursor(16, 56);
    oled.print("PRESS THE BUTTON");
  }
}

void printCentered(const char *s, int y) {
  oled.setCursor(64 - strlen(s) * 3, y);
  oled.print(s);
}

void drawHelp() {
  printCentered("HOW TO PLAY", 0);
  oled.drawFastHLine(31, 9, 66, WHITE);
  printCentered("PRESS = DROP BLOCK", 13);
  printCentered("EXTRA EDGE IS CUT", 22);
  printCentered("3 PERFECT = WIDER", 31);
  printCentered("MISS IT = GAME OVER", 40);
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

  prefs.begin("stack");
  best = prefs.getInt("best", 0);
  Serial.println("STACK ready");
}

void loop() {
  static uint32_t nextFrame = 0;
  static bool pressed = false;
  updateSound();
  pressed |= buttonPressed();

  uint32_t now = millis();
  if ((int32_t)(now - nextFrame) < 0) {
    delay(1);
    return;
  }
  nextFrame = now - nextFrame > 100 ? now + FRAME_MS : nextFrame + FRAME_MS;
  frame++;
  stateT++;
  bool press = pressed;
  pressed = false;

  oled.clearDisplay();
  switch (state) {
    case TITLE:
      drawTitle();
      if (press) {
        state = HELP;
        stateT = 0;
      }
      break;
    case HELP:
      drawHelp();
      if (press && stateT > 10) startGame();
      break;
    case PLAY:
      updatePlay(press);
      drawScene();
      break;
    case FALL:
      drawScene();
      if (stateT > 25) endGame();
      break;
    case OVER:
      drawScene();
      drawGameOver();
      if (press && stateT > 15) startGame();
      else if (stateT > 600) state = TITLE;
      break;
  }
  oled.display();
}
