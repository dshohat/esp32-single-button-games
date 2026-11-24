#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <driver/gpio.h>

// WHACK-A-MOLE: three holes, three buttons. Hit a mole with the button under its hole before it
// hides. Don't hit bombs or empty holes. Three mistakes and the game is over.

const int BTN_PINS[3] = {18, 4, 19};  // left, middle, right; other leg to GND
const int BUZZER_PIN = 23;            // passive buzzer
const int LED_GREEN = 25;             // long leg -> resistor -> pin, short leg -> GND
const int LED_RED = 26;

const int SCREEN_W = 128, SCREEN_H = 64;
const int FRAME_MS = 33;
const int HOLE_X[3] = {22, 64, 106};
const int HOLE_Y = 55;
const int MOLE_H = 22;
const int MAX_LIVES = 3;

#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);

const char *const HEART[] = {".##.##.", "#######", "#######", ".#####.", "..###..", "...#..."};

// ---------------- Sound ----------------

struct Note {
  uint16_t f, ms;
};

const Note SFX_HIT[] = {{1568, 40}, {2093, 70}};
const Note SFX_MISS[] = {{300, 90}, {200, 160}};
const Note SFX_BOMB[] = {{180, 100}, {120, 120}, {80, 250}};
const Note SFX_START[] = {{523, 90}, {659, 90}, {784, 90}, {1047, 180}};
const Note SFX_OVER[] = {{784, 150}, {659, 150}, {523, 150}, {392, 400}};

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

// ---------------- Buttons ----------------

bool btnDown[3];
uint32_t btnT[3];

// Bitmask of buttons that went down since the last call.
uint8_t readButtons(uint32_t now) {
  uint8_t pressed = 0;
  for (int i = 0; i < 3; i++) {
    bool d = digitalRead(BTN_PINS[i]) == LOW;
    if (d != btnDown[i] && now - btnT[i] >= 20) {
      btnDown[i] = d;
      btnT[i] = now;
      if (d) pressed |= 1 << i;
    }
  }
  return pressed;
}

// ---------------- Game state ----------------

enum State { TITLE, PLAY, OVER };
State state = TITLE;
uint32_t stateAt, frame;

enum Kind : uint8_t { EMPTY, MOLE, BOMB };
struct Hole {
  Kind kind;
  uint32_t upAt, downAt, hitAt;
  bool boom;
};
Hole holes[3];

int score, best, lives;
uint32_t nextSpawn, greenUntil, redUntil;

bool before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

int upMs() { return max(450, 1300 - score * 25); }
int gapMs() { return max(200, 800 - score * 20); }
int maxMoles() { return score < 12 ? 1 : 2; }

void setState(State s, uint32_t now) {
  state = s;
  stateAt = now;
}

void updateLeds(uint32_t now) {
  bool g = before(now, greenUntil), r = before(now, redUntil);
  if (state == TITLE) {
    g = (now / 400) % 2 == 0;
    r = !g;
  } else if (state == OVER) {
    r = (now / 300) % 2 == 0;
  }
  digitalWrite(LED_GREEN, g);
  digitalWrite(LED_RED, r);
}

void startGame(uint32_t now) {
  score = 0;
  lives = MAX_LIVES;
  memset(holes, 0, sizeof(holes));
  nextSpawn = now + 800;
  greenUntil = redUntil = now;
  SFX(SFX_START);
  setState(PLAY, now);
}

void clearHole(Hole &h, uint32_t now) {
  h.kind = EMPTY;
  uint32_t t = now + gapMs() / 2 + random(gapMs() / 2);
  if (before(nextSpawn, t)) nextSpawn = t;
}

void mistake(uint32_t now) {
  redUntil = now + 300;
  if (--lives > 0) {
    SFX(SFX_MISS);
    return;
  }
  best = max(best, score);
  SFX(SFX_OVER);
  setState(OVER, now);
}

void whack(int i, uint32_t now) {
  Hole &h = holes[i];
  if (h.kind == MOLE) {
    score++;
    h.hitAt = now;
    h.boom = false;
    greenUntil = now + 150;
    SFX(SFX_HIT);
    clearHole(h, now);
  } else if (h.kind == BOMB) {
    h.hitAt = now;
    h.boom = true;
    clearHole(h, now);
    mistake(now);
    if (state == PLAY) SFX(SFX_BOMB);
  } else {
    mistake(now);
  }
}

void spawn(uint32_t now) {
  int active = 0, nFree = 0, freeIdx[3];
  for (int i = 0; i < 3; i++) {
    if (holes[i].kind != EMPTY) active++;
    else if (now - holes[i].hitAt > 250) freeIdx[nFree++] = i;
  }
  if (active >= maxMoles() || nFree == 0) return;
  Hole &h = holes[freeIdx[random(nFree)]];
  h.kind = score >= 8 && random(100) < 20 ? BOMB : MOLE;
  h.upAt = now;
  h.downAt = now + upMs() + (h.kind == BOMB ? 300 : 0);
  nextSpawn = now + gapMs() + random(gapMs());
}

void updatePlay(uint32_t now) {
  for (Hole &h : holes) {
    if (h.kind == EMPTY || before(now, h.downAt)) continue;
    bool escaped = h.kind == MOLE;
    clearHole(h, now);
    if (escaped) mistake(now);
    if (state != PLAY) return;
  }
  if (!before(now, nextSpawn)) spawn(now);
}

void handlePress(uint8_t pressed, uint32_t now) {
  if (!pressed) return;
  uint32_t age = now - stateAt;
  if (state == TITLE && age > 300) {
    startGame(now);
  } else if (state == OVER && age > 1200) {
    startGame(now);
  } else if (state == PLAY) {
    for (int i = 0; i < 3 && state == PLAY; i++)
      if (pressed & (1 << i)) whack(i, now);
  }
}

// ---------------- Drawing ----------------

void drawSprite(const char *const *rows, int n, int x, int y) {
  for (int r = 0; r < n; r++)
    for (int c = 0; rows[r][c]; c++)
      if (rows[r][c] == '#') oled.drawPixel(x + c, y + r, WHITE);
}

void printCentered(const char *s, int y, int size = 1) {
  oled.setTextSize(size);
  oled.setCursor(64 - strlen(s) * 3 * size, y);
  oled.print(s);
  oled.setTextSize(1);
}

void drawHole(int cx) {
  oled.fillRoundRect(cx - 16, HOLE_Y, 32, 9, 4, WHITE);
  oled.fillRoundRect(cx - 13, HOLE_Y + 1, 26, 4, 2, BLACK);
}

void drawMole(int cx, int h) {
  int top = HOLE_Y + 2 - h;
  oled.fillRoundRect(cx - 9, top, 18, h + 2, min(8, h / 2), WHITE);
  if (h < 12) return;
  oled.fillCircle(cx - 4, top + 6, 1, BLACK);
  oled.fillCircle(cx + 4, top + 6, 1, BLACK);
  oled.fillCircle(cx, top + 10, 2, BLACK);
  oled.drawLine(cx - 9, top + 9, cx - 4, top + 11, BLACK);
  oled.drawLine(cx + 9, top + 9, cx + 4, top + 11, BLACK);
}

void drawBomb(int cx, int h) {
  int cy = HOLE_Y + 10 - h;
  oled.fillCircle(cx, cy, 8, WHITE);
  oled.drawLine(cx + 5, cy - 6, cx + 8, cy - 11, WHITE);
  if (frame & 1) oled.fillCircle(cx + 9, cy - 12, 1, WHITE);
  oled.fillRect(cx - 1, cy - 5, 2, 6, BLACK);
  oled.fillRect(cx - 1, cy + 3, 2, 2, BLACK);
}

void drawBurst(int cx, int r1, int r2) {
  int cy = HOLE_Y - 12;
  for (int k = 0; k < 8; k++) {
    float a = k * PI / 4;
    oled.drawLine(cx + cosf(a) * r1, cy + sinf(a) * r1, cx + cosf(a) * r2, cy + sinf(a) * r2, WHITE);
  }
}

// Height in pixels: rises over 100 ms when popping up and sinks over the last 100 ms.
int riseHeight(const Hole &h, uint32_t now) {
  int t = min((int)(now - h.upAt), (int)(h.downAt - now));
  return MOLE_H * constrain(t, 0, 100) / 100;
}

void drawPlay(uint32_t now) {
  oled.setCursor(0, 0);
  oled.print(score);
  for (int i = 0; i < lives; i++) drawSprite(HEART, COUNT(HEART), SCREEN_W - 8 * (i + 1), 0);

  for (int i = 0; i < 3; i++) {
    const Hole &h = holes[i];
    int cx = HOLE_X[i];
    if (h.kind == MOLE) drawMole(cx, riseHeight(h, now));
    else if (h.kind == BOMB) drawBomb(cx, riseHeight(h, now));
    else if (h.hitAt && now - h.hitAt < 220) {
      if (h.boom) drawBurst(cx, 4, 16);
      else {
        drawBurst(cx, 7, 12);
        oled.setCursor(cx - 5, HOLE_Y - 15);
        oled.print("+1");
      }
    }
    drawHole(cx);
  }
}

void drawTitle(uint32_t now) {
  printCentered("WHACK!", 2, 2);
  printCentered("any button = start", 20);
  for (int i = 0; i < 3; i++) {
    if (i == (now / 700) % 3) drawMole(HOLE_X[i], MOLE_H);
    drawHole(HOLE_X[i]);
  }
}

void drawOver(uint32_t now) {
  char buf[24];
  printCentered("GAME OVER", 4, 2);
  snprintf(buf, sizeof(buf), "Score: %d", score);
  printCentered(buf, 28);
  snprintf(buf, sizeof(buf), "Best:  %d", best);
  printCentered(buf, 40);
  if (now - stateAt > 1200 && (now / 400) % 2) printCentered("press to play", 54);
}

// ---------------- Main ----------------

void setup() {
  Serial.begin(115200);
  for (int pin : BTN_PINS) pinMode(pin, INPUT_PULLUP);
  for (int pin : {LED_GREEN, LED_RED}) {
    pinMode(pin, OUTPUT);
    // Weakest drive keeps LED current low even if wired without a resistor.
    gpio_set_drive_capability((gpio_num_t)pin, GPIO_DRIVE_CAP_0);
  }
  ledcAttach(BUZZER_PIN, 2000, 10);
  ledcWriteTone(BUZZER_PIN, 0);

  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed");
    for (;;) delay(1000);
  }
  oled.setTextColor(WHITE);
  oled.setTextWrap(false);
  randomSeed(esp_random());
  setState(TITLE, millis());
  Serial.println("WHACK ready");
}

void loop() {
  static uint32_t nextFrame = 0;
  updateSound();
  uint32_t now = millis();
  handlePress(readButtons(now), now);
  updateLeds(now);

  if (before(now, nextFrame)) {
    delay(1);
    return;
  }
  nextFrame = now - nextFrame > 100 ? now + FRAME_MS : nextFrame + FRAME_MS;
  frame++;

  if (state == PLAY) updatePlay(now);
  oled.clearDisplay();
  switch (state) {
    case TITLE: drawTitle(now); break;
    case PLAY: drawPlay(now); break;
    case OVER: drawOver(now); break;
  }
  oled.display();
}
