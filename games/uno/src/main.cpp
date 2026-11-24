#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <stdarg.h>

// UNO: one-button card game against 1-3 CPU players (ESP32 + SSD1306 + passive buzzer).
// Tap to move between playable cards, hold to play the highlighted one.

const int BUTTON_PIN = 4;   // other leg to GND
const int BUZZER_PIN = 23;  // passive buzzer

const int SCREEN_W = 128, SCREEN_H = 64;
const int FRAME_MS = 33;
const int HOLD_MS = 450;
const int HAND_START = 7;
const int MAXP = 4, MAXH = 40, DECK_N = 108;
const int FLY_FRAMES = 7;

const int PILE_X = 3, PILE_Y = 12, PILE_W = 18, PILE_H = 24;
const int DISC_X = 26, DISC_Y = 11, DISC_W = 26, DISC_H = 28;
const int PANEL_X = 55;
const int CARD_W = 14, CARD_H = 20, SLOT_W = 16, HAND_Y = 43, VISIBLE = 8;

#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);
Preferences prefs;

// ---------------- Cards ----------------

// A card is (color << 4) | value. Color 4 is wild.
enum { SKIP = 10, REV, DRAW2, WILD, WILD4 };
const int WILD_COL = 4;
#define CCOL(c) ((c) >> 4)
#define CVAL(c) ((c) & 15)

const char COLOR_CH[] = "RYGB";
const char *COLOR_NAME[] = {"RED", "YELLOW", "GREEN", "BLUE"};
const char *NAMES[] = {"YOU", "CPU1", "CPU2", "CPU3"};

// ---------------- Sound ----------------

struct Note {
  uint16_t f, ms;
};

const Note SFX_START[] = {{523, 60}, {659, 60}, {784, 60}, {1047, 120}};
const Note SFX_TICK[] = {{1760, 12}};
const Note SFX_PLAY[] = {{880, 25}, {1320, 35}};
const Note SFX_DRAW[] = {{330, 20}};
const Note SFX_SKIP[] = {{784, 60}, {0, 30}, {392, 90}};
const Note SFX_REV[] = {{523, 50}, {784, 50}, {523, 50}, {784, 70}};
const Note SFX_PLUS[] = {{196, 90}, {147, 90}, {110, 160}};
const Note SFX_WILD[] = {{523, 40}, {659, 40}, {784, 40}, {988, 40}, {1175, 80}};
const Note SFX_UNO[] = {{1047, 70}, {0, 40}, {1047, 70}, {1568, 180}};
const Note SFX_WIN[] = {{1047, 90}, {1319, 90}, {1568, 90}, {2093, 140}, {1568, 90}, {2093, 260}};
const Note SFX_LOSE[] = {{440, 90}, {415, 90}, {392, 90}, {370, 260}};

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

enum { NONE, TAP, HOLD };
bool btnDown, holdFired;
uint32_t btnT, edgeT;

// TAP on a release before HOLD_MS, HOLD once when held that long; 25 ms debounce.
int pollButton() {
  bool raw = digitalRead(BUTTON_PIN) == LOW;
  uint32_t now = millis();
  if (raw != btnDown && now - edgeT >= 25) {
    btnDown = raw;
    edgeT = now;
    if (raw) {
      btnT = now;
      holdFired = false;
    } else if (!holdFired) {
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
  if (!btnDown || holdFired || d < 100) return 0;
  return min(1.0f, d / (float)HOLD_MS);
}

// ---------------- Game state ----------------

enum State { TITLE, HELP, DEAL, FLIP, HUMAN, HUMAN_DRAWN, COLOR, CPU, LAND, NEXT, PENALTY, OVER };
State state = TITLE;
uint32_t frame;
int stateT, waitT;

uint8_t deck[DECK_N], pile[DECK_N], hands[MAXP][MAXH];
int deckN, pileN, handN[MAXP];
int numOpp = 3, numPlayers, turn, dirn, curColor, roundNo, dealIdx;
uint8_t pend;  // card in flight to the discard pile
int opts[MAXH + 1], optN, cursor, scroll;  // opts: hand indices, -1 = draw / pass
int colorSel, penWho, penN, winner, winPts, wins, games;
bool cpuDrew;
char msg[16];
int msgT, unoT, unoWho;

struct Fly {
  float sx, sy, dx, dy;
  uint8_t card;
  bool back;
  int t;
  bool on;
};
Fly flies[6];

void launch(uint8_t card, bool back, int sx, int sy, int dx, int dy) {
  for (auto &f : flies)
    if (!f.on) {
      f = {(float)sx, (float)sy, (float)dx, (float)dy, card, back, 0, true};
      return;
    }
}

void setMsg(const char *fmt, ...) {
  va_list a;
  va_start(a, fmt);
  vsnprintf(msg, sizeof(msg), fmt, a);
  va_end(a);
  msgT = 50;
}

int nextPlayer(int p, int steps = 1) {
  return ((p + dirn * steps) % numPlayers + numPlayers) % numPlayers;
}

void seatPos(int p, int &x, int &y) {
  if (p == 0) {
    x = 57;
    y = HAND_Y;
    return;
  }
  int bw = SCREEN_W / numOpp;
  x = (p - 1) * bw + bw / 2 - CARD_W / 2;
  y = -8;
}

bool humanTurn() { return state == HUMAN || state == HUMAN_DRAWN; }
int handSlots() { return handN[0] + (humanTurn() ? 1 : 0); }
int selSlot() { return opts[cursor] >= 0 ? opts[cursor] : handN[0]; }

int slotX(int i) {
  int n = handSlots();
  if (n <= VISIBLE) return (SCREEN_W - n * SLOT_W + 2) / 2 + i * SLOT_W;
  return (i - scroll) * SLOT_W + 1;
}

// ---------------- Deck ----------------

void shuffle(uint8_t *a, int n) {
  for (int i = n - 1; i > 0; i--) {
    int j = random(i + 1);
    uint8_t t = a[i];
    a[i] = a[j];
    a[j] = t;
  }
}

void buildDeck() {
  deckN = 0;
  for (int c = 0; c < 4; c++) {
    deck[deckN++] = c << 4;
    for (int v = 1; v <= DRAW2; v++) {
      deck[deckN++] = c << 4 | v;
      deck[deckN++] = c << 4 | v;
    }
  }
  for (int i = 0; i < 4; i++) {
    deck[deckN++] = WILD_COL << 4 | WILD;
    deck[deckN++] = WILD_COL << 4 | WILD4;
  }
  shuffle(deck, deckN);
}

bool refillDeck() {
  if (pileN <= 1) return false;
  memcpy(deck, pile, pileN - 1);
  deckN = pileN - 1;
  pile[0] = pile[pileN - 1];
  pileN = 1;
  shuffle(deck, deckN);
  setMsg("RESHUFFLE");
  return true;
}

bool canPlay(uint8_t c) {
  return CCOL(c) == WILD_COL || CCOL(c) == curColor || CVAL(c) == CVAL(pile[pileN - 1]);
}

// Returns the card's index in the hand, or -1 if nothing could be drawn.
int drawTo(int p) {
  if (handN[p] >= MAXH || (deckN == 0 && !refillDeck())) return -1;
  uint8_t c = deck[--deckN];
  int i = handN[p]++;
  if (p == 0)
    for (; i > 0 && hands[0][i - 1] > c; i--) hands[0][i] = hands[0][i - 1];
  hands[p][i] = c;
  int x, y;
  seatPos(p, x, y);
  launch(c, true, PILE_X, PILE_Y, x, y);
  SFX(SFX_DRAW);
  return i;
}

// ---------------- Gameplay ----------------

int bestColor(int p) {
  int cnt[4] = {0}, best = random(4);
  for (int i = 0; i < handN[p]; i++)
    if (CCOL(hands[p][i]) < 4) cnt[CCOL(hands[p][i])]++;
  for (int c = 0; c < 4; c++)
    if (cnt[c] > cnt[best]) best = c;
  return best;
}

int cpuChoose(int p) {
  int cnt[4] = {0};
  for (int i = 0; i < handN[p]; i++)
    if (CCOL(hands[p][i]) < 4) cnt[CCOL(hands[p][i])]++;
  bool threat = handN[nextPlayer(p)] <= 2;
  int best = -1, bestS = -1;
  for (int i = 0; i < handN[p]; i++) {
    uint8_t c = hands[p][i];
    if (!canPlay(c)) continue;
    int v = CVAL(c), s;
    if (CCOL(c) == WILD_COL) {
      // Wilds are saved for emergencies.
      s = (v == WILD4 && threat) ? 70 : (handN[p] <= 2 ? 30 : 1);
    } else {
      s = 10 + cnt[CCOL(c)] * 3 + (v <= 9 ? v / 2 : 0);
      if (v >= SKIP) s += threat ? 40 : 4;
    }
    s += random(4);
    if (s > bestS) {
      bestS = s;
      best = i;
    }
  }
  return best;
}

void startTurn() {
  cpuDrew = false;
  if (turn == 0) {
    state = HUMAN;
    optN = 0;
    for (int i = 0; i < handN[0]; i++)
      if (canPlay(hands[0][i])) opts[optN++] = i;
    opts[optN++] = -1;
    cursor = 0;
  } else {
    state = CPU;
    waitT = 20 + random(15);
  }
}

void endTurn() {
  turn = nextPlayer(turn);
  state = NEXT;
  waitT = 12;
}

void newRound() {
  numPlayers = numOpp + 1;
  buildDeck();
  memset(handN, 0, sizeof(handN));
  pileN = 0;
  dirn = 1;
  msgT = unoT = 0;
  dealIdx = scroll = 0;
  for (auto &f : flies) f.on = false;
  state = DEAL;
  waitT = 6;
  SFX(SFX_START);
}

void roundOver(int w) {
  winner = w;
  winPts = 0;
  for (int p = 0; p < numPlayers; p++)
    for (int i = 0; i < handN[p]; i++) {
      int v = CVAL(hands[p][i]);
      winPts += v <= 9 ? v : v <= DRAW2 ? 20 : 50;
    }
  games++;
  if (w == 0) wins++;
  prefs.putInt("wins", wins);
  prefs.putInt("games", games);
  roundNo++;
  state = OVER;
  stateT = 0;
  if (w == 0) SFX(SFX_WIN);
  else SFX(SFX_LOSE);
}

void playCard(int p, int i) {
  int sx, sy;
  if (p == 0) {
    sx = slotX(i);
    sy = HAND_Y - 3;
  } else {
    seatPos(p, sx, sy);
  }
  pend = hands[p][i];
  memmove(&hands[p][i], &hands[p][i + 1], handN[p] - i - 1);
  handN[p]--;
  launch(pend, false, sx, sy, DISC_X + (DISC_W - CARD_W) / 2, DISC_Y + (DISC_H - CARD_H) / 2);
  state = LAND;
  waitT = FLY_FRAMES;
  SFX(SFX_PLAY);
}

void applyEffect() {
  int p = turn, v = CVAL(pile[pileN - 1]);
  if (v == REV) dirn = -dirn;
  int nx = nextPlayer(p);
  state = NEXT;
  waitT = 12;
  switch (v) {
    case SKIP:
      setMsg("SKIP: %s", NAMES[nx]);
      turn = nextPlayer(p, 2);
      SFX(SFX_SKIP);
      break;
    case REV:
      setMsg("REVERSE!");
      turn = numPlayers == 2 ? p : nx;
      SFX(SFX_REV);
      break;
    case DRAW2:
    case WILD4:
      penWho = nx;
      penN = v == DRAW2 ? 2 : 4;
      setMsg("%s DRAW%s %d", NAMES[nx], nx ? "S" : "", penN);
      state = PENALTY;
      SFX(SFX_PLUS);
      break;
    default:
      turn = nx;
  }
  if (handN[p] == 1) {
    unoT = 40;
    unoWho = p;
    SFX(SFX_UNO);
  }
}

void land() {
  pile[pileN++] = pend;
  int p = turn;
  if (handN[p] == 0) {
    roundOver(p);
    return;
  }
  if (CCOL(pend) == WILD_COL) {
    if (p == 0) {
      state = COLOR;
      colorSel = bestColor(0);
      return;
    }
    curColor = bestColor(p);
    setMsg("WILD: %s", COLOR_NAME[curColor]);
    SFX(SFX_WILD);
  } else {
    curColor = CCOL(pend);
  }
  applyEffect();
}

void humanInput(int ev) {
  if (ev == TAP) {
    cursor = (cursor + 1) % optN;
    msgT = 0;
    SFX(SFX_TICK);
    return;
  }
  if (ev != HOLD) return;
  int o = opts[cursor];
  if (o >= 0) {
    playCard(0, o);
    return;
  }
  if (state == HUMAN_DRAWN) {
    endTurn();
    return;
  }
  int i = drawTo(0);
  if (i >= 0 && canPlay(hands[0][i])) {
    state = HUMAN_DRAWN;
    opts[0] = i;
    opts[1] = -1;
    optN = 2;
    cursor = 0;
    waitT = FLY_FRAMES;
  } else {
    setMsg(i < 0 ? "CAN'T DRAW" : "NO MATCH");
    endTurn();
  }
}

void cpuTurn() {
  int i = cpuChoose(turn);
  if (i >= 0) {
    playCard(turn, i);
  } else if (!cpuDrew && drawTo(turn) >= 0) {
    cpuDrew = true;
    setMsg("%s DRAWS", NAMES[turn]);
    waitT = 14;
  } else {
    endTurn();
  }
}

void updateGame(int ev) {
  if (waitT > 0) {
    waitT--;
    return;
  }
  switch (state) {
    case DEAL:
      if (dealIdx < HAND_START * numPlayers) {
        drawTo(dealIdx++ % numPlayers);
        waitT = 2;
      } else {
        // The starting card must be a number.
        int i = deckN - 1;
        while (CVAL(deck[i]) > 9) i--;
        pend = deck[i];
        deck[i] = deck[--deckN];
        launch(pend, false, PILE_X, PILE_Y, DISC_X + (DISC_W - CARD_W) / 2, DISC_Y + (DISC_H - CARD_H) / 2);
        SFX(SFX_PLAY);
        state = FLIP;
        waitT = FLY_FRAMES;
      }
      break;
    case FLIP:
      pile[pileN++] = pend;
      curColor = CCOL(pend);
      turn = roundNo % numPlayers;
      startTurn();
      break;
    case HUMAN:
    case HUMAN_DRAWN:
      humanInput(ev);
      break;
    case COLOR:
      if (ev == TAP) {
        colorSel = (colorSel + 1) % 4;
        SFX(SFX_TICK);
      } else if (ev == HOLD) {
        curColor = colorSel;
        setMsg("WILD: %s", COLOR_NAME[curColor]);
        SFX(SFX_WILD);
        applyEffect();
      }
      break;
    case CPU:
      cpuTurn();
      break;
    case LAND:
      land();
      break;
    case NEXT:
      startTurn();
      break;
    case PENALTY:
      if (penN > 0) {
        drawTo(penWho);
        penN--;
        waitT = 6;
      } else {
        turn = nextPlayer(penWho);
        startTurn();
      }
      break;
    case OVER:
      if (ev && stateT > 20) newRound();
      else if (stateT > 900) state = TITLE;
      break;
    default:
      break;
  }
}

// ---------------- Drawing ----------------

void printC(const char *s, int cx, int y, int size = 1) {
  oled.setTextSize(size);
  oled.setCursor(cx - (int)strlen(s) * 3 * size, y);
  oled.print(s);
}

void dottedRect(int x, int y, int w, int h) {
  for (int i = 0; i < w; i += 2) {
    oled.drawPixel(x + i, y, WHITE);
    oled.drawPixel(x + i, y + h - 1, WHITE);
  }
  for (int j = 0; j < h; j += 2) {
    oled.drawPixel(x, y + j, WHITE);
    oled.drawPixel(x + w - 1, y + j, WHITE);
  }
}

void drawSymbol(int cx, int cy, uint8_t c, uint16_t col, int s) {
  int v = CVAL(c);
  if (v == SKIP) {
    int r = 3 * s + 1, d = r * 7 / 10;
    oled.drawCircle(cx, cy, r, col);
    if (s > 1) oled.drawCircle(cx, cy, r - 1, col);
    oled.drawLine(cx - d, cy + d, cx + d, cy - d, col);
    return;
  }
  if (v == REV) {
    int w = 4 * s, h = 2 * s, a = 2 * s;
    for (int t = 0; t < s; t++) {
      int y1 = cy - h + t, y2 = cy + h - t;
      oled.drawLine(cx - w, y1, cx + w, y1, col);
      oled.drawLine(cx + w, y1, cx + w - a, y1 - a, col);
      oled.drawLine(cx + w, y1, cx + w - a, y1 + a, col);
      oled.drawLine(cx + w, y2, cx - w, y2, col);
      oled.drawLine(cx - w, y2, cx - w + a, y2 - a, col);
      oled.drawLine(cx - w, y2, cx - w + a, y2 + a, col);
    }
    return;
  }
  char buf[3] = {0};
  if (v <= 9) buf[0] = '0' + v;
  else if (v == WILD) buf[0] = 'W';
  else {
    buf[0] = '+';
    buf[1] = v == DRAW2 ? '2' : '4';
  }
  int n = strlen(buf);
  oled.setTextSize(s);
  oled.setTextColor(col);
  oled.setCursor(cx - (n * 6 - 1) * s / 2, cy - 7 * s / 2);
  oled.print(buf);
  oled.setTextColor(WHITE);
}

void drawFace(int x, int y, int w, int h, uint8_t c, bool inv, bool dotted, int s) {
  uint16_t fg = inv ? BLACK : WHITE;
  oled.fillRoundRect(x, y, w, h, 2, inv ? WHITE : BLACK);
  if (dotted) dottedRect(x, y, w, h);
  else oled.drawRoundRect(x, y, w, h, 2, WHITE);
  oled.setTextSize(1);
  oled.setTextColor(fg);
  oled.setCursor(x + 2, y + 2);
  oled.print(CCOL(c) < 4 ? COLOR_CH[CCOL(c)] : '*');
  oled.setTextColor(WHITE);
  drawSymbol(x + w / 2, y + (h + 8) / 2, c, fg, s);
}

void drawBack(int x, int y, int w, int h) {
  oled.fillRoundRect(x, y, w, h, 2, BLACK);
  oled.drawRoundRect(x, y, w, h, 2, WHITE);
  for (int j = y + 2; j < y + h - 2; j += 2)
    for (int i = x + 2 + ((j - y) & 2); i < x + w - 2; i += 4) oled.drawPixel(i, j, WHITE);
}

void drawOptionSlot(int x, int y, bool inv) {
  oled.fillRoundRect(x, y, CARD_W, CARD_H, 2, inv ? WHITE : BLACK);
  oled.drawRoundRect(x, y, CARD_W, CARD_H, 2, WHITE);
  oled.setTextSize(1);
  oled.setTextColor(inv ? BLACK : WHITE);
  oled.setCursor(x + 2, y + 7);
  oled.print(state == HUMAN ? "+1" : ">>");
  oled.setTextColor(WHITE);
}

void cardName(uint8_t c, char *buf, int n) {
  static const char *ACTION[] = {"SKIP", "REV", "+2"};
  int v = CVAL(c);
  if (CCOL(c) == WILD_COL) snprintf(buf, n, v == WILD ? "WILD" : "WILD +4");
  else if (v <= 9) snprintf(buf, n, "%s %d", COLOR_NAME[CCOL(c)], v);
  else snprintf(buf, n, "%s %s", COLOR_NAME[CCOL(c)], ACTION[v - SKIP]);
}

void drawTopBar() {
  int bw = SCREEN_W / numOpp;
  bool live = state != DEAL && state != FLIP;
  for (int p = 1; p <= numOpp; p++) {
    int x = (p - 1) * bw;
    bool cur = live && turn == p;
    char buf[12];
    if (handN[p] == 1 && ((frame / 8) & 1)) strcpy(buf, "UNO!");
    else snprintf(buf, sizeof(buf), "%s %d", NAMES[p], handN[p]);
    if (cur) oled.fillRect(x, 0, bw, 10, WHITE);
    oled.setTextColor(cur ? BLACK : WHITE);
    printC(buf, x + bw / 2, 1);
    oled.setTextColor(WHITE);
    if (p > 1) oled.drawFastVLine(x, 0, 10, WHITE);
  }
  oled.drawFastHLine(0, 10, SCREEN_W, WHITE);
}

void drawTable() {
  if (deckN > 0) {
    if (deckN > 1) drawBack(PILE_X + 2, PILE_Y + 2, PILE_W, PILE_H);
    drawBack(PILE_X, PILE_Y, PILE_W, PILE_H);
  } else {
    dottedRect(PILE_X, PILE_Y, PILE_W, PILE_H);
  }
  if (pileN > 0) drawFace(DISC_X, DISC_Y, DISC_W, DISC_H, pile[pileN - 1], false, false, 2);
  else dottedRect(DISC_X, DISC_Y, DISC_W, DISC_H);
}

void drawPanel() {
  char buf[16];
  bool live = state != DEAL && state != FLIP;
  if (pileN > 0) {
    oled.fillRect(PANEL_X, 12, 72, 10, WHITE);
    oled.setTextColor(BLACK);
    printC(COLOR_NAME[curColor], PANEL_X + 36, 13);
    oled.setTextColor(WHITE);
  }

  if (!live) strcpy(buf, "DEALING");
  else if (turn == 0) strcpy(buf, "YOUR TURN");
  else snprintf(buf, sizeof(buf), "%s TURN", NAMES[turn]);
  oled.setTextSize(1);
  oled.setCursor(PANEL_X, 24);
  oled.print(buf);
  if (live) {
    // Play order runs left to right across the top bar when dirn is +1.
    int ax = 122, ay = 27;
    oled.drawFastHLine(ax - 4, ay, 9, WHITE);
    oled.drawLine(ax + 4 * dirn, ay, ax + 2 * dirn, ay - 2, WHITE);
    oled.drawLine(ax + 4 * dirn, ay, ax + 2 * dirn, ay + 2, WHITE);
  }

  buf[0] = 0;
  if (msgT > 0) {
    msgT--;
    strcpy(buf, msg);
  } else if (humanTurn()) {
    if (opts[cursor] >= 0) cardName(hands[0][opts[cursor]], buf, sizeof(buf));
    else strcpy(buf, state == HUMAN ? "DRAW A CARD" : "KEEP & PASS");
  }
  oled.setCursor(PANEL_X, 32);
  oled.print(buf);

  if (unoT > 0) {
    unoT--;
    oled.fillRect(PANEL_X - 2, 11, 75, 29, BLACK);
    oled.drawRoundRect(PANEL_X - 2, 11, 75, 29, 3, WHITE);
    printC("UNO!", PANEL_X + 35, 14, 2);
    printC(NAMES[unoWho], PANEL_X + 35, 31);
  }
}

void drawHand() {
  bool ht = humanTurn();
  int n = handSlots(), sel = ht ? selSlot() : -1;
  if (ht) {
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + VISIBLE) scroll = sel - VISIBLE + 1;
  }
  scroll = constrain(scroll, 0, max(0, n - VISIBLE));

  for (int i = 0; i < n; i++) {
    int x = slotX(i);
    if (x <= -CARD_W || x >= SCREEN_W) continue;
    bool s = i == sel;
    int y = s ? HAND_Y - 3 : HAND_Y;
    if (i < handN[0]) {
      bool playable = !ht || (state == HUMAN ? canPlay(hands[0][i]) : i == opts[0]);
      drawFace(x, y, CARD_W, CARD_H, hands[0][i], s, !playable, 1);
    } else {
      drawOptionSlot(x, y, s);
    }
    float p = s ? holdProgress() : 0;
    if (p > 0) oled.fillRect(x, SCREEN_H - 3, lroundf(CARD_W * p), 2, WHITE);
  }
  if (scroll > 0) oled.fillTriangle(0, 40, 2, 38, 2, 42, WHITE);
  if (scroll + VISIBLE < n) oled.fillTriangle(127, 40, 125, 38, 125, 42, WHITE);
}

void drawFlies() {
  for (auto &f : flies) {
    if (!f.on) continue;
    float k = min(1.0f, f.t / (float)FLY_FRAMES);
    k = 1 - (1 - k) * (1 - k);
    int x = lroundf(f.sx + (f.dx - f.sx) * k), y = lroundf(f.sy + (f.dy - f.sy) * k);
    if (f.back) drawBack(x, y, CARD_W, CARD_H);
    else drawFace(x, y, CARD_W, CARD_H, f.card, false, false, 1);
    if (++f.t > FLY_FRAMES) f.on = false;
  }
}

void drawColorPicker() {
  int x = 14, y = 12, w = 100, h = 40;
  oled.fillRoundRect(x, y, w, h, 4, BLACK);
  oled.drawRoundRect(x, y, w, h, 4, WHITE);
  printC("PICK A COLOR", 64, y + 3);
  for (int i = 0; i < 4; i++) {
    int bx = x + 4 + (i % 2) * 47, by = y + 13 + (i / 2) * 12;
    bool s = i == colorSel;
    if (s) oled.fillRect(bx, by, 45, 11, WHITE);
    else oled.drawRect(bx, by, 45, 11, WHITE);
    oled.setTextColor(s ? BLACK : WHITE);
    printC(COLOR_NAME[i], bx + 22, by + 2);
    oled.setTextColor(WHITE);
  }
  oled.drawFastHLine(x + 4, y + h - 3, lroundf(92 * holdProgress()), WHITE);
}

void drawRoundOver() {
  int y = 8 + max(0, 48 - stateT * 8);
  oled.fillRoundRect(4, y, 120, 50, 5, BLACK);
  oled.drawRoundRect(4, y, 120, 50, 5, WHITE);
  char buf[20];
  if (winner == 0) strcpy(buf, "YOU WIN!");
  else snprintf(buf, sizeof(buf), "%s WINS", NAMES[winner]);
  printC(buf, 64, y + 4, 2);
  snprintf(buf, sizeof(buf), "%d POINTS", winPts);
  printC(buf, 64, y + 23);
  snprintf(buf, sizeof(buf), "WON %d OF %d", wins, games);
  printC(buf, 64, y + 32);
  if (stateT > 20 && ((frame / 12) & 1)) printC("PRESS FOR NEXT", 64, y + 41);
}

void drawGame() {
  drawTopBar();
  drawTable();
  drawPanel();
  drawHand();
  drawFlies();
  if (state == COLOR) drawColorPicker();
  if (state == OVER) drawRoundOver();
}

void drawTitle() {
  printC("UNO", 64, 0, 3);
  static const uint8_t demo[] = {0x07, 0x1C, 0x2A, 0x3B, 0x4E};
  for (int i = 0; i < COUNT(demo); i++) {
    int x = 28 + i * 15, y = 24 + lroundf(sinf(frame * 0.12f + i * 0.9f) * 2);
    drawFace(x, y, CARD_W, CARD_H, demo[i], i == (int)(frame / 15) % COUNT(demo), false, 1);
  }
  char buf[20];
  snprintf(buf, sizeof(buf), "OPPONENTS: %d", numOpp);
  printC(buf, 64, 47);
  if ((frame / 12) & 1) printC("TAP:CPUS  HOLD:PLAY", 64, 56);
}

void drawHelp() {
  printC("HOW TO PLAY", 64, 0);
  oled.drawFastHLine(31, 9, 66, WHITE);
  printC("TAP  = NEXT CARD", 64, 12);
  printC("HOLD = PLAY IT", 64, 21);
  printC("MATCH COLOR OR NUMBER", 64, 30);
  printC("NO MATCH? DRAW (+1)", 64, 39);
  if ((frame / 12) & 1) printC("PRESS TO START", 64, 55);
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

  prefs.begin("uno");
  wins = prefs.getInt("wins", 0);
  games = prefs.getInt("games", 0);
  numOpp = constrain(prefs.getInt("opps", 3), 1, 3);
  Serial.println("UNO ready");
}

void loop() {
  static uint32_t nextFrame = 0;
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
  frame++;
  stateT++;
  int press = ev;
  ev = NONE;

  oled.clearDisplay();
  switch (state) {
    case TITLE:
      drawTitle();
      if (press == TAP) {
        numOpp = numOpp % 3 + 1;
        prefs.putInt("opps", numOpp);
        SFX(SFX_TICK);
      } else if (press == HOLD) {
        state = HELP;
        stateT = 0;
        SFX(SFX_TICK);
      }
      break;
    case HELP:
      drawHelp();
      if (press && stateT > 10) {
        roundNo = 0;
        newRound();
      }
      break;
    default:
      updateGame(press);
      drawGame();
      break;
  }
  oled.display();
}
