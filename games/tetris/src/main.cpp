#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>

// TETRIS: one-button falling blocks (ESP32 + SSD1306 + passive buzzer).
// The piece slides left/right by itself. Tap = rotate, hold = drop it here.

const int BUTTON_PIN = 4;   // other leg to GND
const int BUZZER_PIN = 23;  // passive buzzer

const int SCREEN_W = 128, SCREEN_H = 64;
const int COLS = 10, ROWS = 16, CELL = 4;
const int WELL_X = 44;
const uint16_t FULL_ROW = (1 << COLS) - 1;
const uint32_t HOLD_MS = 260;  // press longer than this = hard drop
const int FRAME_MS = 33;

#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

// 4x4 masks, MSB = top-left; one entry per rotation.
const uint16_t SHAPES[7][4] = {
    {0x0F00, 0x2222, 0x00F0, 0x4444},  // I
    {0x8E00, 0x6440, 0x0E20, 0x44C0},  // J
    {0x2E00, 0x4460, 0x0E80, 0xC440},  // L
    {0x6600, 0x6600, 0x6600, 0x6600},  // O
    {0x6C00, 0x4620, 0x06C0, 0x8C40},  // S
    {0x4E00, 0x4640, 0x0E40, 0x4C40},  // T
    {0xC600, 0x2640, 0x0C60, 0x4C80},  // Z
};

Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);
Preferences prefs;

// ---------------- Sound ----------------

struct Note {
  uint16_t f, ms;
};

constexpr uint16_t Q = 260, E = 130;
const Note THEME[] = {
    {659, Q}, {494, E}, {523, E}, {587, Q}, {523, E}, {494, E},
    {440, Q - 20}, {0, 20}, {440, E}, {523, E}, {659, Q}, {587, E}, {523, E},
    {494, Q + E}, {523, E}, {587, Q}, {659, Q},
    {523, Q}, {440, Q - 20}, {0, 20}, {440, Q}};
const Note SFX_START[] = {{523, 60}, {659, 60}, {784, 60}, {1047, 120}};
const Note SFX_ROTATE[] = {{1400, 15}};
const Note SFX_LOCK[] = {{196, 25}, {131, 35}};
const Note SFX_L1[] = {{784, 50}, {1047, 100}};
const Note SFX_L2[] = {{659, 50}, {784, 50}, {1047, 110}};
const Note SFX_L3[] = {{523, 50}, {659, 50}, {784, 50}, {1047, 130}};
const Note SFX_L4[] = {{523, 60}, {659, 60}, {784, 60}, {1047, 60}, {1319, 60}, {1568, 180}};
const Note SFX_LEVEL[] = {{1047, 60}, {1319, 60}, {1568, 60}, {2093, 150}};
const Note SFX_OVER[] = {{440, 70}, {330, 70}, {247, 90}, {165, 110}, {110, 200}};
const Note SFX_RECORD[] = {{1047, 90}, {1319, 90}, {1568, 90}, {2093, 140}, {1568, 90}, {2093, 260}};

Note sfx[24];
int sfxLen, sfxIdx;
uint32_t sfxT;

void play(const Note *s, int len) {
  len = min(len, COUNT(sfx));
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

bool btnDown, evPress, evRelease;
uint32_t downT;

// Debounced: after an edge, changes are ignored for 25 ms.
void readButton() {
  static uint32_t edgeT;
  bool raw = digitalRead(BUTTON_PIN) == LOW;
  if (raw == btnDown || millis() - edgeT < 25) return;
  btnDown = raw;
  edgeT = millis();
  if (raw) {
    evPress = true;
    downT = edgeT;
  } else {
    evRelease = true;
  }
}

// ---------------- Game state ----------------

enum State { TITLE, HELP, PLAY, CLEAR, TOPOUT, OVER };
State state = TITLE;
uint32_t frame;
int stateT;

uint16_t board[ROWS];  // bit x = column x
int cur, rot, px, py, dir, nextPiece;
int gravT, sweepT;
bool holding, dropped;
uint8_t bag[7];
int bagLeft;
int score, lines, level, best;
uint16_t clearMask;
int clearCount;
bool newBest;

bool fits(int p, int r, int x, int y) {
  uint16_t m = SHAPES[p][r & 3];
  for (int i = 0; i < 16; i++) {
    if (!(m & (0x8000 >> i))) continue;
    int cx = x + (i & 3), cy = y + (i >> 2);
    if (cx < 0 || cx >= COLS || cy >= ROWS) return false;
    if (cy >= 0 && (board[cy] >> cx & 1)) return false;
  }
  return true;
}

// 7-bag: every piece appears once per 7 spawns.
int drawFromBag() {
  if (bagLeft == 0) {
    for (int i = 0; i < 7; i++) bag[i] = i;
    bagLeft = 7;
  }
  int k = random(bagLeft);
  int p = bag[k];
  bag[k] = bag[--bagLeft];
  return p;
}

int gravityFrames() { return max(2, 22 - (level - 1) * 2); }
int sweepFrames() { return max(2, 7 - (level - 1) / 2); }

// ---------------- Gameplay ----------------

void topOut() {
  state = TOPOUT;
  stateT = 0;
  SFX(SFX_OVER);
}

void spawn() {
  cur = nextPiece;
  nextPiece = drawFromBag();
  rot = 0;
  px = 3;
  py = -1;
  dir = random(2) ? 1 : -1;
  gravT = sweepT = 0;
  if (!fits(cur, rot, px, py)) topOut();
}

void startGame() {
  memset(board, 0, sizeof(board));
  score = lines = 0;
  level = 1;
  bagLeft = 0;
  holding = dropped = newBest = false;
  nextPiece = drawFromBag();
  state = PLAY;
  stateT = 0;
  spawn();
  SFX(SFX_START);
}

void lockPiece() {
  uint16_t m = SHAPES[cur][rot];
  bool out = false;
  for (int i = 0; i < 16; i++) {
    if (!(m & (0x8000 >> i))) continue;
    int cx = px + (i & 3), cy = py + (i >> 2);
    if (cy < 0) out = true;
    else board[cy] |= 1 << cx;
  }
  if (out) {
    topOut();
    return;
  }

  clearMask = 0;
  clearCount = 0;
  for (int y = 0; y < ROWS; y++)
    if (board[y] == FULL_ROW) {
      clearMask |= 1 << y;
      clearCount++;
    }

  switch (clearCount) {
    case 0: SFX(SFX_LOCK); spawn(); return;
    case 1: SFX(SFX_L1); break;
    case 2: SFX(SFX_L2); break;
    case 3: SFX(SFX_L3); break;
    default: SFX(SFX_L4); break;
  }
  state = CLEAR;
  stateT = 0;
}

void finishClear() {
  static const int LINE_PTS[] = {0, 100, 300, 500, 800};
  int w = ROWS - 1;
  for (int y = ROWS - 1; y >= 0; y--)
    if (!(clearMask >> y & 1)) board[w--] = board[y];
  while (w >= 0) board[w--] = 0;

  score += LINE_PTS[min(clearCount, 4)] * level;
  lines += clearCount;
  int newLevel = 1 + lines / 10;
  if (newLevel > level) {
    level = newLevel;
    SFX(SFX_LEVEL);
  }
  state = PLAY;
  stateT = 0;
  spawn();
}

void hardDrop() {
  while (fits(cur, rot, px, py + 1)) {
    py++;
    score += 2;
  }
  lockPiece();
}

void rotatePiece() {
  static const int8_t KICKS[] = {0, -1, 1, -2, 2};
  for (int k : KICKS)
    if (fits(cur, rot + 1, px + k, py)) {
      rot = (rot + 1) & 3;
      px += k;
      SFX(SFX_ROTATE);
      return;
    }
}

void updatePlay() {
  if (evPress) {
    holding = true;
    dropped = false;
  }
  if (holding && !dropped && btnDown && millis() - downT >= HOLD_MS) {
    dropped = true;
    hardDrop();
    return;
  }
  if (evRelease) {
    if (holding && !dropped) rotatePiece();
    holding = false;
  }

  // While the button is held the piece stops sliding, so you can aim.
  bool aiming = holding && btnDown && !dropped;
  if (!aiming && ++sweepT >= sweepFrames()) {
    sweepT = 0;
    if (fits(cur, rot, px + dir, py)) px += dir;
    else dir = -dir;
  }
  if (++gravT >= gravityFrames()) {
    gravT = 0;
    if (fits(cur, rot, px, py + 1)) py++;
    else lockPiece();
  }
}

void endGame() {
  state = OVER;
  stateT = 0;
  newBest = score > best;
  if (newBest) {
    best = score;
    prefs.putInt("best", best);
    SFX(SFX_RECORD);
  }
}

void goTitle() {
  state = TITLE;
  stateT = 0;
  SFX(THEME);
}

// ---------------- Drawing ----------------

void drawBlock(int x, int y, bool ghost) {
  if (!ghost) {
    oled.fillRect(x, y, CELL - 1, CELL - 1, WHITE);
    return;
  }
  oled.drawPixel(x, y, WHITE);
  oled.drawPixel(x + 2, y, WHITE);
  oled.drawPixel(x, y + 2, WHITE);
  oled.drawPixel(x + 2, y + 2, WHITE);
}

// ox, oy in pixels; cells above the screen are skipped.
void drawShape(int p, int r, int ox, int oy, bool ghost) {
  uint16_t m = SHAPES[p][r & 3];
  for (int i = 0; i < 16; i++) {
    if (!(m & (0x8000 >> i))) continue;
    int y = oy + (i >> 2) * CELL;
    if (y >= 0) drawBlock(ox + (i & 3) * CELL, y, ghost);
  }
}

void printCentered(const char *s, int y) {
  oled.setCursor(64 - strlen(s) * 3, y);
  oled.print(s);
}

void drawGame() {
  oled.drawFastVLine(WELL_X - 2, 0, SCREEN_H, WHITE);
  oled.drawFastVLine(WELL_X + COLS * CELL, 0, SCREEN_H, WHITE);

  for (int y = 0; y < ROWS; y++) {
    if (state == CLEAR && (clearMask >> y & 1)) {
      if (!((stateT / 3) & 1)) oled.fillRect(WELL_X, y * CELL, COLS * CELL - 1, CELL - 1, WHITE);
      continue;
    }
    for (int x = 0; x < COLS; x++)
      if (board[y] >> x & 1) drawBlock(WELL_X + x * CELL, y * CELL, false);
  }

  if (state == PLAY) {
    int gy = py;
    while (fits(cur, rot, px, gy + 1)) gy++;
    drawShape(cur, rot, WELL_X + px * CELL, gy * CELL, true);
    drawShape(cur, rot, WELL_X + px * CELL, py * CELL, false);
  }

  if (state == TOPOUT || state == OVER) {
    int k = min(ROWS, stateT / 2);
    if (state == OVER) k = ROWS;
    for (int y = ROWS - k; y < ROWS; y++)
      for (int x = 0; x < COLS; x++) drawBlock(WELL_X + x * CELL, y * CELL, true);
  }

  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("SCORE");
  oled.setCursor(0, 10);
  oled.print(score);
  oled.setCursor(0, 24);
  oled.print("LINES");
  oled.setCursor(0, 34);
  oled.print(lines);
  oled.setCursor(0, 46);
  oled.print("LEVEL");
  oled.setCursor(0, 56);
  oled.print(level);

  oled.setCursor(90, 0);
  oled.print("NEXT");
  drawShape(nextPiece, 0, 92, 11, false);
  oled.setCursor(90, 34);
  oled.print("BEST");
  oled.setCursor(90, 44);
  oled.print(best);

  if (state == CLEAR) {
    static const char *NAMES[] = {"", "SINGLE", "DOUBLE", "TRIPLE", "TETRIS"};
    oled.fillRect(WELL_X, 26, COLS * CELL - 1, 11, BLACK);
    oled.setCursor(WELL_X + 2, 28);
    oled.print(NAMES[min(clearCount, 4)]);
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
  oled.setCursor(28, y + 25);
  oled.printf("SCORE %6d", score);
  oled.setCursor(28, y + 36);
  if (!newBest) oled.printf("BEST  %6d", best);
  else if ((stateT / 8) & 1) oled.print(" NEW RECORD!");
}

void drawTitle() {
  oled.setTextSize(3);
  oled.setCursor(10, 2);
  oled.print("TETRIS");
  oled.setTextSize(1);

  static const int PIECES[] = {5, 4, 2, 3, 6};  // T S L O Z
  for (int i = 0; i < 5; i++) drawShape(PIECES[i], frame / 15 + i, 14 + i * 22, 28, false);

  char buf[16];
  snprintf(buf, sizeof(buf), "BEST %d", best);
  printCentered(buf, 46);
  if ((frame / 12) & 1) printCentered("PRESS THE BUTTON", 56);
}

void drawHelp() {
  printCentered("HOW TO PLAY", 0);
  oled.drawFastHLine(31, 9, 66, WHITE);
  printCentered("PIECE SLIDES ITSELF", 13);
  printCentered("TAP  = ROTATE", 22);
  printCentered("HOLD = DROP DOWN", 31);
  printCentered("FILL ROWS TO CLEAR", 40);
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

  prefs.begin("tetris");
  best = prefs.getInt("best", 0);
  level = 1;
  Serial.println("TETRIS ready");
  goTitle();
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
      drawGame();
      break;
    case CLEAR:
      drawGame();
      if (stateT > 15) finishClear();
      break;
    case TOPOUT:
      drawGame();
      if (stateT > ROWS * 2 + 15) endGame();
      break;
    case OVER:
      drawGame();
      drawGameOver();
      if (evPress && stateT > 20) startGame();
      else if (stateT > 600) goTitle();
      break;
  }
  evPress = evRelease = false;
  oled.display();
}
