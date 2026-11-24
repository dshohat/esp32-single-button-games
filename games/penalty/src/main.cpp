#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// PENALTY: one-button penalty kick practice (ESP32 + SSD1306 + passive buzzer).
// An arrow sweeps across the goal; press to shoot where it points.

const int BUTTON_PIN = 4;   // other leg to GND
const int BUZZER_PIN = 23;  // passive buzzer

const int SCREEN_W = 128, SCREEN_H = 64;
const int GOAL_W = 36;  // between the posts
const int GOAL_TOP = 16, GOAL_LINE = 30;
const int AIM_Y = 23;  // height in the goal mouth the ball flies to
const float GOAL_MIN = GOAL_W / 2 + 8, GOAL_MAX = SCREEN_W - GOAL_W / 2 - 8;  // goal center range
const float GOAL_SPEED = 0.9f;
const int AIM_MARGIN = GOAL_W / 10;
const float AIM_MIN = GOAL_MIN - GOAL_W / 2 - AIM_MARGIN, AIM_MAX = GOAL_MAX + GOAL_W / 2 + AIM_MARGIN;
const float AIM_SPEED = 2.6f;
const int BALL_X = 64, BALL_Y = 52;
const int RUNUP_FRAMES = 8, FLIGHT_FRAMES = 16;
const int FRAME_MS = 33;

#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);

// ---------------- Sound ----------------

struct Note {
  uint16_t f, ms;
};

const Note SFX_WHISTLE[] = {{2600, 90}, {0, 50}, {2600, 300}};
const Note SFX_KICK[] = {{160, 25}, {80, 50}};
const Note SFX_GOAL[] = {{523, 110}, {659, 110}, {784, 110}, {1047, 220}, {0, 60},
                         {784, 110}, {1047, 450}, {0, 80}, {1319, 90}, {1568, 90}, {2093, 400}};
const Note SFX_MISS[] = {{392, 280}, {0, 30}, {370, 280}, {0, 30}, {349, 280}, {0, 30},
                         {330, 200}, {311, 200}, {330, 200}, {311, 400}};

Note sfx[16];
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

// ---------------- Sprites ('#' = lit pixel), player seen from behind ----------------

struct Sprite {
  const char *const *rows;
  uint8_t w, h;
};

#define SPRITE(name, ...)                          \
  const char *const name##_rows[] = {__VA_ARGS__}; \
  const Sprite name = {name##_rows, (uint8_t)strlen(name##_rows[0]), (uint8_t)COUNT(name##_rows)}

SPRITE(STAND,
  "....####....",
  "...######...",
  "...######...",
  "....####....",
  "..########..",
  ".##########.",
  ".##.####.##.",
  ".##.####.##.",
  ".##.####.##.",
  "..#.####.#..",
  "...######...",
  "...######...",
  "...##..##...",
  "...##..##...",
  "...##..##...",
  "...##..##...",
  "..###..###..");

SPRITE(KICK,
  "....####....",
  "...######...",
  "...######...",
  "....####....",
  "#.########..",
  ".##########.",
  "...######.##",
  "...######..#",
  "...######...",
  "...######...",
  "...######...",
  "...######...",
  "...##..###..",
  "...##...###.",
  "...##....###",
  "...##.....##",
  "..###.......");

SPRITE(CHEER,
  "#..........#",
  "##..####..##",
  ".#.######.#.",
  ".#.######.#.",
  ".##.####.##.",
  "..########..",
  "...######...",
  "...######...",
  "...######...",
  "...######...",
  "...######...",
  "...######...",
  "...##..##...",
  "...##..##...",
  "...##..##...",
  "...##..##...",
  "..###..###..");

SPRITE(SAD,
  "....####....",
  "..########..",
  ".##.####.##.",
  ".#..####..#.",
  ".#.######.#.",
  ".##########.",
  "...######...",
  "...######...",
  "...######...",
  "...######...",
  "...######...",
  "...######...",
  "...##..##...",
  "...##..##...",
  "...##..##...",
  "...##..##...",
  "..###..###..");

void plot(const Sprite &s, int x, int y, uint16_t color) {
  for (int j = 0; j < s.h; j++)
    for (int i = 0; s.rows[j][i]; i++)
      if (s.rows[j][i] == '#') oled.drawPixel(x + i, y + j, color);
}

// Draws with a 1 px black halo so the sprite stays readable over the field.
void draw(const Sprite &s, int x, int y) {
  plot(s, x - 1, y, BLACK);
  plot(s, x + 1, y, BLACK);
  plot(s, x, y - 1, BLACK);
  plot(s, x, y + 1, BLACK);
  plot(s, x, y, WHITE);
}

// ---------------- Game state ----------------

enum State { TITLE, AIM, RUNUP, FLIGHT, RESULT };
State state = TITLE;
uint32_t frame;
int stateT;

float aimX, shotX, goalX = SCREEN_W / 2;
int aimDir, goalDir;
bool scored, inverted;

// x of the first pixel inside the left post; the right post starts at goalL() + GOAL_W.
int goalL() { return lroundf(goalX) - GOAL_W / 2; }

struct Confetti {
  float x, y, vx, vy;
};
Confetti confetti[40];

uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

void setInvert(bool on) {
  if (on == inverted) return;
  inverted = on;
  oled.invertDisplay(on);
}

void startAim() {
  state = AIM;
  stateT = 0;
  aimX = random((int)AIM_MIN, (int)AIM_MAX + 1);
  aimDir = random(2) ? 1 : -1;
  goalX = random((int)GOAL_MIN, (int)GOAL_MAX + 1);
  goalDir = random(2) ? 1 : -1;
  setInvert(false);
  SFX(SFX_WHISTLE);
}

void shoot() {
  state = RUNUP;
  stateT = 0;
  shotX = roundf(aimX);
}

void moveGoal() {
  goalX += goalDir * GOAL_SPEED;
  if (goalX <= GOAL_MIN) {
    goalX = GOAL_MIN;
    goalDir = 1;
  } else if (goalX >= GOAL_MAX) {
    goalX = GOAL_MAX;
    goalDir = -1;
  }
}

void spawnConfetti(Confetti &c, bool anywhere) {
  c.x = random(SCREEN_W);
  c.y = anywhere ? -random(SCREEN_H) : -2;
  c.vx = random(-10, 11) / 20.0f;
  c.vy = random(5, 16) / 10.0f;
}

// Ball position on the way to the goal; p = 0 at the penalty spot, 1 at the goal plane.
void ballAt(float p, float &x, float &y, float &r) {
  x = BALL_X + (shotX - BALL_X) * p;
  y = BALL_Y + (AIM_Y - BALL_Y) * p - 4 * sinf(PI * min(p, 1.0f));
  r = max(0.0f, 3 - 2 * p);
}

// ---------------- Drawing ----------------

void printCentered(const char *s, int y, int size = 1) {
  oled.setTextSize(size);
  oled.setCursor(64 - (strlen(s) * 6 * size - size) / 2, y);
  oled.print(s);
  oled.setTextSize(1);
}

void drawCrowd(bool cheer) {
  for (int r = 0; r < 2; r++)
    for (int x = r * 2; x < SCREEN_W; x += 4) {
      int up = cheer && (hash32(x * 3 + r + (frame / 3) * 131) & 1);
      oled.fillRect(x, 1 + r * 3 - up, 2, 2, WHITE);
    }
  oled.drawFastHLine(0, 7, SCREEN_W, WHITE);
}

void drawField() {
  for (int i = 0; i < 28; i++) {
    uint32_t h = hash32(i + 77);
    oled.drawPixel(h % SCREEN_W, GOAL_LINE + 3 + (h >> 8) % (SCREEN_H - GOAL_LINE - 3), WHITE);
  }
  oled.drawFastHLine(0, GOAL_LINE, SCREEN_W, WHITE);
  // Goal area, drawn in perspective.
  oled.drawLine(18, GOAL_LINE, 10, 38, WHITE);
  oled.drawLine(110, GOAL_LINE, 118, 38, WHITE);
  oled.drawFastHLine(10, 38, 109, WHITE);
  oled.fillRect(BALL_X - 1, BALL_Y + 1, 3, 2, WHITE);  // penalty spot
}

void drawNet(int shake) {
  int l = goalL();
  for (int y = GOAL_TOP; y < GOAL_LINE; y++)
    for (int x = l; x < l + GOAL_W; x++)
      if ((x + y + shake) % 6 == 0 || (x - y + 120 - shake) % 6 == 0) oled.drawPixel(x, y, WHITE);
}

void drawPosts() {
  int l = goalL();
  oled.fillRect(l - 2, GOAL_TOP - 2, GOAL_W + 4, 2, WHITE);
  oled.fillRect(l - 2, GOAL_TOP, 2, GOAL_LINE - GOAL_TOP + 1, WHITE);
  oled.fillRect(l + GOAL_W, GOAL_TOP, 2, GOAL_LINE - GOAL_TOP + 1, WHITE);
}

void drawBall(float x, float y, float r) {
  int ix = lroundf(x), iy = lroundf(y), ir = lroundf(r);
  oled.fillCircle(ix, iy, ir + 1, BLACK);
  if (ir >= 3) {
    oled.drawCircle(ix, iy, ir, WHITE);
    oled.fillRect(ix - 1, iy - 1, 2, 2, WHITE);
  } else {
    oled.fillCircle(ix, iy, ir, WHITE);
  }
}

void drawArrow(float tx) {
  float sx = BALL_X, sy = BALL_Y - 6;
  float dx = tx - sx, dy = AIM_Y - sy;
  float len = sqrtf(dx * dx + dy * dy);
  float ux = dx / len, uy = dy / len;
  float ex = sx + dx * 0.8f, ey = sy + dy * 0.8f;  // tip
  float bx = ex - ux * 6, by = ey - uy * 6;         // base of the head
  float px = -uy * 3.5f, py = ux * 3.5f;

  for (int o = -1; o <= 2; o++) oled.drawLine(sx + o, sy, bx + o, by, BLACK);
  oled.fillTriangle(ex + ux * 2, ey + uy * 2, bx + px * 1.5f - ux, by + py * 1.5f - uy,
                    bx - px * 1.5f - ux, by - py * 1.5f - uy, BLACK);
  oled.drawLine(sx, sy, bx, by, WHITE);
  oled.drawLine(sx + 1, sy, bx + 1, by, WHITE);
  oled.fillTriangle(ex, ey, bx + px, by + py, bx - px, by - py, WHITE);
}

void drawScene() {
  bool cheer = state == RESULT && scored;
  drawCrowd(cheer);
  drawField();
  drawNet(cheer && stateT < 16 ? ((stateT & 1) ? 1 : -1) : 0);

  float bx = BALL_X, by = BALL_Y, br = 3, p = 0;
  bool ballOn = true;
  if (state == FLIGHT) {
    float t = (float)stateT / FLIGHT_FRAMES;
    p = 1 - (1 - t) * (1 - t);  // eases out, as if receding into the distance
    ballAt(p, bx, by, br);
  } else if (state == RESULT) {
    p = 1;
    if (scored) {
      bx = shotX;
      by = min(AIM_Y + 0.15f * stateT * stateT, GOAL_LINE - 2.0f);  // drops into the net
      br = 1;
    } else {
      float q = 1 + stateT * 0.06f;
      ballAt(q, bx, by, br);
      ballOn = q < 1.8f;
    }
  }

  // Once the ball reaches the goal plane it is behind the posts.
  if (ballOn && p >= 1) drawBall(bx, by, br);
  drawPosts();
  if (p < 1) {
    if (state == FLIGHT) {
      int shy = lroundf(BALL_Y + 3 + (GOAL_LINE + 1 - BALL_Y - 3) * p);
      for (int i = -(int)br; i <= (int)br; i += 2) oled.drawPixel(lroundf(bx) + i, shy, WHITE);
    }
    drawBall(bx, by, br);
  }
  if (state == AIM || state == RUNUP || state == TITLE) drawArrow(state == TITLE ? BALL_X : aimX);

  if (state == TITLE || state == AIM) {
    draw(STAND, 40, 46);
  } else if (state == RUNUP) {
    float t = (float)stateT / RUNUP_FRAMES;
    int x = 40 + lroundf(11 * t), y = 46 - lroundf(4 * t) + ((stateT / 2) & 1);
    draw(stateT >= RUNUP_FRAMES - 2 ? KICK : STAND, x, y);
  } else if (state == FLIGHT) {
    draw(KICK, 51, 42);
  } else if (scored) {
    draw(CHEER, 51, 42 - (stateT > 4 && ((stateT / 4) & 1) ? 3 : 0));
  } else {
    draw(SAD, 51, 42);
  }
}

void drawPrompt(const char *s, int y = 56) {
  if (!((frame / 12) & 1)) return;
  oled.fillRect(0, y - 1, SCREEN_W, 9, BLACK);
  printCentered(s, y);
}

void drawResult() {
  if (scored) {
    setInvert(stateT >= 2 && stateT < 14 && ((stateT / 3) & 1));
    for (int i = 0; i < COUNT(confetti); i++) {
      Confetti &c = confetti[i];
      c.x += c.vx + sinf((frame + i * 7) * 0.2f) * 0.4f;
      c.y += c.vy;
      if (c.y > SCREEN_H && stateT < 150) spawnConfetti(c, false);
      if ((frame + i) & 2) oled.fillRect(c.x, c.y, 2, 1, WHITE);
      else oled.fillRect(c.x, c.y, 1, 2, WHITE);
    }
    if (stateT >= 14) {
      int size = stateT < 16 ? 1 : stateT < 18 ? 2 : 3;  // pops in
      oled.fillRoundRect(10, 10, 108, 41, 5, BLACK);
      oled.drawRoundRect(10, 10, 108, 41, 5, WHITE);
      printCentered("GOAL!", 26 - 4 * size - (size == 3 && ((stateT / 6) & 1)), size);
      if (size == 3) printCentered("GREAT SHOT!", 41);
    }
  } else {
    setInvert(stateT < 3);
    if (stateT >= 14) {
      int dx = stateT < 30 ? ((stateT & 2) ? 2 : -2) : 0;
      oled.fillRoundRect(10 + dx, 10, 108, 41, 5, BLACK);
      oled.drawRoundRect(10 + dx, 10, 108, 41, 5, WHITE);
      oled.setTextSize(3);
      oled.setCursor(64 - 44 + dx, 14);
      oled.print("MISS!");
      oled.setTextSize(1);
      printCentered(shotX < goalL() ? "WIDE LEFT" : "WIDE RIGHT", 41);
    }
  }
  if (stateT > 60) drawPrompt("PRESS TO PLAY AGAIN");
}

void drawTitle() {
  oled.fillRoundRect(16, 32, 96, 21, 4, BLACK);
  oled.drawRoundRect(16, 32, 96, 21, 4, WHITE);
  printCentered("PENALTY", 35, 2);
  drawPrompt("PRESS TO START");
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
  Serial.println("PENALTY ready");
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

  switch (state) {
    case TITLE:
      if (press) startAim();
      break;
    case AIM:
      moveGoal();
      aimX += aimDir * AIM_SPEED;
      if (aimX <= AIM_MIN) {
        aimX = AIM_MIN;
        aimDir = 1;
      } else if (aimX >= AIM_MAX) {
        aimX = AIM_MAX;
        aimDir = -1;
      }
      if (press) shoot();
      break;
    case RUNUP:
      moveGoal();
      if (stateT >= RUNUP_FRAMES) {
        state = FLIGHT;
        stateT = 0;
        SFX(SFX_KICK);
      }
      break;
    case FLIGHT:
      moveGoal();
      if (stateT >= FLIGHT_FRAMES) {
        state = RESULT;
        stateT = 0;
        // The goal keeps moving while the ball flies; at arrival all 3 px of ball must be inside.
        scored = shotX - 1 >= goalL() && shotX + 1 < goalL() + GOAL_W;
        if (scored) {
          for (auto &c : confetti) spawnConfetti(c, true);
          SFX(SFX_GOAL);
        } else {
          SFX(SFX_MISS);
        }
      }
      break;
    case RESULT:
      // Ignore presses for 2 s so a late press doesn't skip the result.
      if (press && stateT > 60) startAim();
      break;
  }

  oled.clearDisplay();
  drawScene();
  if (state == TITLE) drawTitle();
  else if (state == AIM && stateT < 90) drawPrompt("PRESS TO SHOOT", 0);
  else if (state == RESULT) drawResult();
  oled.display();
}
