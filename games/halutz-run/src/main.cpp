#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <atomic>

// HALUTZ RUN: a one-button pioneer runner (ESP32 + SSD1306 + passive buzzer).

const int BUTTON_PIN = 4;   // other leg to GND
const int BUZZER_PIN = 23;  // passive buzzer

const int SCREEN_W = 128, SCREEN_H = 64;
const int GROUND = 54;   // y of the ground line
const int HORIZON = 46;  // base of the far skyline
const int PLAYER_X = 18;
const int ZONE_W = 360;  // far-layer pixels per zone
const int FRAME_MS = 33;

#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);
GFXcanvas1 flagImg(46, 30);
Preferences prefs;

// ---------------- Sound and button (5 ms background task) ----------------

struct Note {
  uint16_t f, d;  // d: eighth notes for music, milliseconds for effects
};

enum : uint16_t {
  nD5 = 587, nE5 = 659, nF5 = 698, nG5 = 784, nGs5 = 831, nA5 = 880,
  nBb5 = 932, nB5 = 988, nC6 = 1047, nD6 = 1175
};

const Note HATIKVAH[] = {
  {nD5, 1}, {nE5, 1}, {nF5, 1}, {nG5, 1}, {nA5, 2}, {nA5, 2},
  {nBb5, 1}, {nA5, 1}, {nBb5, 1}, {nD6, 1}, {nA5, 4},
  {nG5, 2}, {nG5, 1}, {nG5, 1}, {nF5, 2}, {nF5, 2},
  {nE5, 1}, {nD5, 1}, {nE5, 1}, {nF5, 1}, {nD5, 3}, {0, 1},
  {nD5, 1}, {nE5, 1}, {nF5, 1}, {nG5, 1}, {nA5, 2}, {nA5, 2},
  {nBb5, 1}, {nA5, 1}, {nBb5, 1}, {nD6, 1}, {nA5, 4},
  {nG5, 2}, {nG5, 1}, {nG5, 1}, {nF5, 2}, {nF5, 2},
  {nE5, 1}, {nD5, 1}, {nE5, 1}, {nF5, 1}, {nD5, 3}, {0, 1},
  {nD5, 2}, {nD6, 2}, {nD6, 2}, {nD6, 2},
  {nC6, 1}, {nD6, 1}, {nC6, 1}, {nBb5, 1}, {nA5, 4},
  {nD5, 2}, {nD6, 2}, {nD6, 2}, {nD6, 2},
  {nC6, 1}, {nD6, 1}, {nC6, 1}, {nBb5, 1}, {nA5, 4},
  {0, 6},
};

const Note HAVA[] = {
  {nE5, 2}, {nE5, 2}, {nGs5, 2}, {nF5, 1}, {nE5, 1},
  {nGs5, 2}, {nGs5, 2}, {nB5, 2}, {nA5, 1}, {nGs5, 1},
  {nA5, 2}, {nA5, 2}, {nC6, 2}, {nB5, 1}, {nA5, 1},
  {nGs5, 2}, {nF5, 1}, {nE5, 1}, {nE5, 4},
  {nE5, 2}, {nE5, 2}, {nGs5, 2}, {nF5, 1}, {nE5, 1},
  {nGs5, 2}, {nGs5, 2}, {nB5, 2}, {nA5, 1}, {nGs5, 1},
  {nA5, 2}, {nA5, 2}, {nC6, 2}, {nB5, 1}, {nA5, 1},
  {nGs5, 2}, {nF5, 1}, {nE5, 1}, {nE5, 4},
  {nF5, 2}, {nF5, 1}, {nF5, 1}, {nA5, 2}, {nGs5, 1}, {nF5, 1},
  {nGs5, 2}, {nF5, 1}, {nE5, 1}, {nE5, 4},
  {nF5, 2}, {nF5, 1}, {nF5, 1}, {nA5, 2}, {nGs5, 1}, {nF5, 1},
  {nGs5, 2}, {nF5, 1}, {nE5, 1}, {nE5, 4},
};

const Note SFX_JUMP[] = {{520, 22}, {700, 22}, {940, 28}};
const Note SFX_STAR[] = {{1568, 45}, {2093, 45}, {2637, 90}};
const Note SFX_CRASH[] = {{440, 70}, {330, 70}, {247, 90}, {165, 110}, {110, 200}};
const Note SFX_ZONE[] = {{784, 80}, {988, 80}, {1175, 80}, {1568, 220}};
const Note SFX_RECORD[] = {{1047, 90}, {1319, 90}, {1568, 90}, {2093, 140}, {1568, 90}, {2093, 260}};

portMUX_TYPE audioMux = portMUX_INITIALIZER_UNLOCKED;
const Note *musReq, *sfxReq;
int musReqLen, sfxReqLen;
bool musReqLoop, musReqNew, sfxReqNew;
volatile uint16_t musUnit = 190;  // ms per eighth note
volatile bool btnDown;
std::atomic<bool> btnLatch{false};

void playMusic(const Note *m, int len, uint16_t unit, bool loop) {
  portENTER_CRITICAL(&audioMux);
  musReq = m;
  musReqLen = len;
  musReqLoop = loop;
  musReqNew = true;
  musUnit = unit;
  portEXIT_CRITICAL(&audioMux);
}

void playSfx(const Note *s, int len) {
  portENTER_CRITICAL(&audioMux);
  sfxReq = s;
  sfxReqLen = len;
  sfxReqNew = true;
  portEXIT_CRITICAL(&audioMux);
}

#define MUSIC(m, unit) playMusic(m, COUNT(m), unit, true)
#define SFX(s) playSfx(s, COUNT(s))

void ioTask(void *) {
  const Note *mus = nullptr, *sfx = nullptr;
  int musLen = 0, musIdx = 0, sfxLen = 0, sfxIdx = 0;
  bool musLoop = false;
  uint32_t musT = 0, sfxT = 0;
  uint16_t curF = 0;
  uint8_t cnt = 0;
  for (;;) {
    bool raw = digitalRead(BUTTON_PIN) == LOW;
    if (raw == btnDown) cnt = 0;
    else if (++cnt >= 3) {
      btnDown = raw;
      cnt = 0;
      if (raw) btnLatch = true;
    }

    uint32_t now = millis();
    portENTER_CRITICAL(&audioMux);
    if (musReqNew) {
      mus = musReq;
      musLen = musReqLen;
      musLoop = musReqLoop;
      musIdx = 0;
      musT = now;
      musReqNew = false;
    }
    if (sfxReqNew) {
      sfx = sfxReq;
      sfxLen = sfxReqLen;
      sfxIdx = 0;
      sfxT = now;
      sfxReqNew = false;
    }
    portEXIT_CRITICAL(&audioMux);

    uint16_t f = 0;
    if (mus) {
      uint32_t d = (uint32_t)mus[musIdx].d * musUnit;
      if (now - musT >= d) {
        musT += d;
        if (++musIdx >= musLen) {
          if (musLoop) musIdx = 0;
          else mus = nullptr;
        }
        if (mus) d = (uint32_t)mus[musIdx].d * musUnit;
      }
      // The last 25 ms of each note are silent so repeated notes stay distinct.
      if (mus && now - musT + 25 < d) f = mus[musIdx].f;
    }
    if (sfx) {
      if (now - sfxT >= sfx[sfxIdx].d) {
        sfxT += sfx[sfxIdx].d;
        if (++sfxIdx >= sfxLen) sfx = nullptr;
      }
      if (sfx) f = sfx[sfxIdx].f;
    }
    if (f != curF) {
      curF = f;
      ledcWriteTone(BUZZER_PIN, f);
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ---------------- Sprites ('#' = lit pixel) ----------------

struct Sprite {
  const char *const *rows;
  uint8_t w, h;
};

#define SPRITE(name, ...)                          \
  const char *const name##_rows[] = {__VA_ARGS__}; \
  const Sprite name = {name##_rows, (uint8_t)strlen(name##_rows[0]), (uint8_t)COUNT(name##_rows)}

SPRITE(RUN1,
  "......###.......",
  ".....#####......",
  "....#######.....",
  "...#########....",
  ".....####.......",
  ".....##.#.......",
  ".....####.......",
  "......##........",
  "...########.....",
  "..##.####.##....",
  ".##..####..##...",
  ".....####.......",
  "....###.##......",
  "...##....##.....",
  "..##......##....",
  "..#........##...");

SPRITE(RUN2,
  "......###.......",
  ".....#####......",
  "....#######.....",
  "...#########....",
  ".....####.......",
  ".....##.#.......",
  ".....####.......",
  "......##........",
  "....######......",
  "...#.####.#.....",
  "...#.####.#.....",
  ".....####.......",
  "......###.......",
  "......####......",
  ".....##.#.......",
  ".....#..##......");

SPRITE(JUMP,
  "......###.......",
  ".....#####......",
  "....#######.....",
  "...#########....",
  ".#...####....#..",
  ".##..##.#...##..",
  "..##.####..##...",
  "...##.##..##....",
  "....########....",
  ".....####.......",
  ".....####.......",
  ".....####.......",
  "....###.###.....",
  "..###.....###...",
  "..#.........#...",
  "................");

SPRITE(ROCK,
  "....####....",
  "..###.###...",
  ".####..####.",
  ".##########.",
  "####.#######",
  "############",
  "############");

SPRITE(SABRA,
  "......##..",
  ".....####.",
  ".##..####.",
  "####.####.",
  "####..##..",
  "####.###..",
  ".#######..",
  "..#####...",
  "..#.###...",
  "..#####...",
  "..###.#...",
  "...###....");

SPRITE(BIGSABRA,
  ".....###......",
  "....#####.....",
  "....##.##.....",
  "....#####..##.",
  ".##..###..####",
  "####.###..#.##",
  "#.##.###.####.",
  "####.#######..",
  ".##########...",
  "..###.####....",
  "...#######....",
  "...###.###....",
  "...#######....",
  "...##.####....",
  "...#######....",
  "....#####.....");

SPRITE(BIRD1,
  ".#.#....##......",
  "..##...###......",
  "..###.####......",
  "###########.....",
  "..############..",
  "...#########....",
  ".....#####......");

SPRITE(BIRD2,
  ".#.#............",
  "..##............",
  "..###...........",
  "###########.....",
  "..############..",
  "...#########....",
  ".....#####......",
  "......####......",
  ".......##.......");

SPRITE(GEM,
  "....#....",
  "...#.#...",
  "...#.#...",
  "#########",
  ".##...##.",
  ".#.....#.",
  ".##...##.",
  "#########",
  "...#.#...",
  "...#.#...",
  "....#....");

SPRITE(CLOUD,
  ".....####.......",
  "...########.##..",
  ".###############",
  "################",
  ".##############.");

SPRITE(CAMEL,
  "...........##.",
  ".....##....###",
  "....####...#..",
  "...######..#..",
  ".###########..",
  ".##########...",
  "#.#......#....",
  "..#......#....",
  "..##.....##...");

SPRITE(PALM,
  "..##.##..",
  ".#######.",
  "#.#####.#",
  "#..###..#",
  "....#....",
  "....#....",
  "...#.....",
  "...#.....",
  "....#....",
  "....#....");

SPRITE(CYPRESS,
  ".#.",
  ".#.",
  "###",
  "###",
  "###",
  "###",
  "###",
  ".#.",
  ".#.");

// sx squeezes the sprite horizontally (4 = full width) for the spinning star.
void plot(const Sprite &s, int x, int y, uint16_t color, bool flip, int sx) {
  int cx = s.w / 2;
  for (int j = 0; j < s.h; j++) {
    const char *r = s.rows[flip ? s.h - 1 - j : j];
    for (int i = 0; r[i]; i++)
      if (r[i] == '#') oled.drawPixel(x + cx + (i - cx) * sx / 4, y + j, color);
  }
}

// Draws with a 1 px black halo so sprites stay readable over the background.
void draw(const Sprite &s, int x, int y, bool flip = false, int sx = 4) {
  plot(s, x - 1, y, BLACK, flip, sx);
  plot(s, x + 1, y, BLACK, flip, sx);
  plot(s, x, y - 1, BLACK, flip, sx);
  plot(s, x, y + 1, BLACK, flip, sx);
  plot(s, x, y, WHITE, flip, sx);
}

// ---------------- Game state ----------------

enum State { TITLE, PLAY, DYING, OVER };
State state = TITLE;
uint32_t frame;
int stateT;

struct Obj {
  float x;
  int16_t y;
  int8_t type;
  bool on;
};
Obj obs[6], gems[4];

struct ObsType {
  const Sprite *a, *b;  // two animation frames
  int8_t y, ix, iy, hb; // top y, hitbox insets, hitbox bottom row
};
const ObsType OBS[] = {
  {&ROCK, &ROCK, GROUND - 7, 2, 1, 6},
  {&SABRA, &SABRA, GROUND - 12, 2, 1, 11},
  {&BIGSABRA, &BIGSABRA, GROUND - 16, 3, 1, 15},
  {&BIRD1, &BIRD2, GROUND - 15, 2, 2, 6},  // low bird: jump over it
  {&BIRD1, &BIRD2, GROUND - 31, 2, 2, 6},  // high bird: stay on the ground
};

struct Particle {
  float x, y, vx, vy;
  int8_t life;
};
Particle parts[28];

const char *const ZONE_NAME[] = {"NEGEV", "TEL AVIV", "GALILEE", "JERUSALEM"};

float dist, speed, jumpY, vy, night, nextSpawn;
float cloudX[2];
int cloudY[2];
bool onGround, newBest;
int airT, jumpBuffer, bonus, best, zone, bannerT, popT, popY;

int score() { return (int)(dist / 12) + bonus; }
int farScroll() { return 8 + (int)(dist / 4); }

uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

void emit(float x, float y, float vx, float vy, int life) {
  for (auto &p : parts)
    if (p.life <= 0) {
      p = {x, y, vx, vy, (int8_t)life};
      return;
    }
}

void burst(float x, float y, int n, float v) {
  for (int i = 0; i < n; i++) {
    float a = i * TWO_PI / n;
    emit(x, y, cosf(a) * v, sinf(a) * v - 0.6f, 9 + random(7));
  }
}

void dust() {
  for (int i = 0; i < 4; i++)
    emit(PLAYER_X + 5 + random(5), GROUND - 1, -0.4f - random(12) / 10.0f, -random(10) / 12.0f, 7 + random(6));
}

void addObj(Obj *pool, int n, float x, int y, int type) {
  for (int i = 0; i < n; i++)
    if (!pool[i].on) {
      pool[i] = {x, (int16_t)y, (int8_t)type, true};
      return;
    }
}

// ---------------- Far background ----------------

float jerusalemH(int wx) {
  int lx = wx % 120;
  float h = 9 + ((wx & 3) < 2 ? 2 : 0);  // crenellated wall
  if (lx >= 6 && lx < 18) h += 6;        // gate tower
  if (lx >= 24 && lx < 30) h = (lx >= 26 && lx < 28) ? 26 : (lx >= 25 && lx < 29) ? 24 : 22;  // Tower of David
  int d = abs(lx - 70);
  if (d <= 9) h = 11 + sqrtf(81 - d * d) + (d == 0 ? 3 : 0);
  d = abs(lx - 100);
  if (d <= 5) h = 11 + sqrtf(25 - d * d);
  return h;
}

// Skyline height above the horizon at far-layer world x.
int skylineH(int wx) {
  if (wx < 0) return 0;
  int z = (wx / ZONE_W) & 3, lx = wx % ZONE_W;
  float h;
  switch (z) {
    case 0:
      h = 8 + 5 * sinf(wx * 0.05f) + 2.5f * sinf(wx * 0.13f + 1);
      break;
    case 1: {
      int in = wx % 10;
      h = in == 9 ? 2 : 6 + hash32(wx / 10) % 16;
      if (h > 17 && in == 4) h += 4;
      break;
    }
    case 2:
      h = 12 + 7 * sinf(wx * 0.035f) + 3 * sinf(wx * 0.09f + 2);
      break;
    default:
      h = jerusalemH(wx);
  }
  int edge = min(lx, ZONE_W - 1 - lx);
  if (edge < 16) h = h * edge / 16;
  return (int)h;
}

bool texture(int z, int wx, int y) {
  int d = HORIZON - y;
  switch (z) {
    case 0:
      return (wx + d * 3) % 7 == 0 && (d & 1);
    case 1: {
      int in = wx % 10;
      return (in == 2 || in == 4 || in == 6) && d % 3 == 2 && hash32(wx * 31 + d) % 3;
    }
    case 2:
      return d % 4 == 2 && (wx + d * 2) % 5 < 2;
    default:
      return (d % 4 == 0 && (wx & 1)) || (d % 4 == 2 && (wx + (d & 4)) % 8 == 0);
  }
}

void drawSkyline(int scroll) {
  int prevTop = HORIZON - 1 - skylineH(scroll - 1);
  for (int x = 0; x < SCREEN_W; x++) {
    int wx = scroll + x, z = (wx / ZONE_W) & 3;
    int top = HORIZON - 1 - skylineH(wx);
    oled.drawFastVLine(x, top, HORIZON - top, BLACK);
    if (top <= prevTop) {
      oled.drawFastVLine(x, top, prevTop - top + 1, WHITE);
    } else {
      oled.drawFastVLine(x - 1, prevTop, top - prevTop, WHITE);
      oled.drawPixel(x, top, WHITE);
    }
    for (int y = top + 2; y < HORIZON; y++)
      if (texture(z, wx, y)) oled.drawPixel(x, y, WHITE);
    prevTop = top;
  }
}

void drawFarExtras(int scroll) {
  for (int k = (scroll - 20) / 60; k <= (scroll + SCREEN_W) / 60; k++) {
    int wx = k * 60 + 30, z = (wx / ZONE_W) & 3, lx = wx % ZONE_W;
    if (lx < 24 || lx > ZONE_W - 40) continue;
    int x = wx - scroll;
    if (z == 0) {
      const Sprite &s = (hash32(k) & 1) ? CAMEL : PALM;
      draw(s, x, HORIZON - skylineH(wx + s.w / 2) - s.h + 1);
    } else if (z == 2) {
      draw(CYPRESS, x, HORIZON - skylineH(wx + 1) - CYPRESS.h + 1);
      draw(CYPRESS, x + 6, HORIZON - skylineH(wx + 7) - CYPRESS.h + 2);
    }
  }
}

void drawSky() {
  if (night > 0.5f)
    for (int i = 0; i < 18; i++) {
      int x = hash32(i * 7 + 1) % SCREEN_W, y = hash32(i * 13 + 5) % 36;
      if (y < 9 && x < 34) continue;
      uint32_t tw = hash32(i + (frame >> 3) * 97) % 8;
      if (tw == 0) continue;
      oled.drawPixel(x, y, WHITE);
      if (tw == 1) {
        oled.drawFastHLine(x - 1, y, 3, WHITE);
        oled.drawFastVLine(x, y - 1, 3, WHITE);
      }
    }

  int sunY = 12 + (int)(night * 50), moonY = 12 + (int)((1 - night) * 50);
  if (sunY < HORIZON + 12) {
    oled.fillCircle(106, sunY, 5, WHITE);
    for (int i = 0; i < 8; i++) {
      float a = i * PI / 4 + frame * 0.04f, r = (i & 1) ? 10 : 12;
      oled.drawLine(106 + lroundf(cosf(a) * 8), sunY + lroundf(sinf(a) * 8),
                    106 + lroundf(cosf(a) * r), sunY + lroundf(sinf(a) * r), WHITE);
    }
  }
  if (moonY < HORIZON + 8) {
    oled.fillCircle(36, moonY, 6, WHITE);
    oled.fillCircle(39, moonY - 2, 5, BLACK);
  }
  if (night < 0.9f)
    for (int i = 0; i < 2; i++) draw(CLOUD, (int)cloudX[i], cloudY[i]);
}

// ---------------- Gameplay ----------------

void enterTitle() {
  state = TITLE;
  stateT = 0;
  MUSIC(HATIKVAH, 330);
}

void startGame() {
  randomSeed(micros());
  state = PLAY;
  stateT = 0;
  dist = 0;
  jumpY = vy = night = 0;
  onGround = true;
  jumpBuffer = bonus = bannerT = popT = 0;
  zone = -1;
  nextSpawn = 150;
  newBest = false;
  for (auto &o : obs) o.on = false;
  for (auto &g : gems) g.on = false;
  for (auto &p : parts) p.life = 0;
  cloudX[0] = 50;
  cloudY[0] = 3;
  cloudX[1] = 120;
  cloudY[1] = 10;
  MUSIC(HAVA, 190);
}

void spawn() {
  int s = score(), r = random(100), type;
  if (s < 40) type = r < 50 ? 0 : 1;
  else if (s < 150) type = r < 30 ? 0 : r < 70 ? 1 : 2;
  else type = r < 15 ? 0 : r < 40 ? 1 : r < 65 ? 2 : r < 82 ? 3 : 4;

  float x = SCREEN_W + 4;
  float gap = speed * random(s < 100 ? 34 : 28, 58);
  addObj(obs, COUNT(obs), x, OBS[type].y, type);
  if (type == 1 && s >= 100 && random(3) == 0) addObj(obs, COUNT(obs), x + 11, OBS[1].y, 1);

  if (type <= 2 && random(100) < 40)
    addObj(gems, COUNT(gems), x + OBS[type].a->w / 2 - 4, OBS[type].y - 22, 0);
  else if (random(100) < 35)
    addObj(gems, COUNT(gems), x + gap / 2, random(2) ? GROUND - 13 : GROUND - 34, 0);
  nextSpawn = gap;
}

bool overlap(int ax0, int ay0, int ax1, int ay1, int bx0, int by0, int bx1, int by1) {
  return ax0 <= bx1 && bx0 <= ax1 && ay0 <= by1 && by0 <= ay1;
}

void die() {
  state = DYING;
  stateT = 0;
  vy = 4.5f;
  playMusic(nullptr, 0, 190, false);
  SFX(SFX_CRASH);
  burst(PLAYER_X + 8, GROUND - 8 - jumpY, 12, 2.2f);
}

void updatePlay(bool press) {
  speed = min(2.4f + dist / 4000.0f, 5.2f);
  dist += speed;
  musUnit = 190 - (int)((speed - 2.4f) * 23);

  if (press) jumpBuffer = 5;
  else if (jumpBuffer > 0) jumpBuffer--;
  if (onGround && jumpBuffer > 0) {
    onGround = false;
    vy = 5.3f;
    airT = jumpBuffer = 0;
    SFX(SFX_JUMP);
    dust();
  }
  if (!onGround) {
    // Releasing the button early cuts the jump short.
    if (++airT > 2 && !btnDown && vy > 2.0f) vy = 2.0f;
    jumpY += vy;
    vy -= 0.55f;
    if (jumpY <= 0) {
      jumpY = 0;
      onGround = true;
      dust();
    }
  }
  int top = GROUND - 16 - (int)jumpY;

  int z = ((farScroll() + PLAYER_X + 40) / ZONE_W) & 3;
  if (z != zone) {
    zone = z;
    bannerT = 75;
    SFX(SFX_ZONE);
  }

  for (int i = 0; i < 2; i++) {
    cloudX[i] -= speed * 0.1f;
    if (cloudX[i] < -18) {
      cloudX[i] = SCREEN_W + random(40);
      cloudY[i] = 2 + random(10);
    }
  }

  nextSpawn -= speed;
  if (nextSpawn <= 0) spawn();

  for (auto &g : gems) {
    if (!g.on) continue;
    g.x -= speed;
    if (g.x < -12) g.on = false;
    else if (overlap(PLAYER_X + 2, top, PLAYER_X + 13, top + 15, (int)g.x, g.y, (int)g.x + 8, g.y + 10)) {
      g.on = false;
      bonus += 25;
      popT = 18;
      popY = top;
      SFX(SFX_STAR);
      burst(g.x + 4, g.y + 5, 10, 1.7f);
    }
  }

  for (auto &o : obs) {
    if (!o.on) continue;
    const ObsType &t = OBS[o.type];
    o.x -= speed + (o.type >= 3 ? 1.2f : 0);
    if (o.x < -20) {
      o.on = false;
      continue;
    }
    int ox = (int)o.x;
    if (overlap(PLAYER_X + 4, top + 3, PLAYER_X + 11, top + 15,
                ox + t.ix, o.y + t.iy, ox + t.a->w - 1 - t.ix, o.y + t.hb)) {
      die();
      return;
    }
  }
}

void updateDying() {
  jumpY += vy;
  vy -= 0.5f;
  if (stateT > 25 && GROUND - 16 - jumpY > SCREEN_H + 4) {
    state = OVER;
    stateT = 0;
    newBest = score() > best;
    if (newBest) {
      best = score();
      prefs.putInt("best", best);
      SFX(SFX_RECORD);
    }
  }
}

void drawScene() {
  int far = farScroll();
  night += constrain((zone & 1) - night, -0.02f, 0.02f);

  drawSky();
  drawSkyline(far);
  oled.fillRect(0, HORIZON, SCREEN_W, SCREEN_H - HORIZON, BLACK);
  drawFarExtras(far);

  oled.drawFastHLine(0, GROUND, SCREEN_W, WHITE);
  int g = (int)dist;
  for (int x = 0; x < SCREEN_W; x++) {
    uint32_t h = hash32(x + g);
    if (h % 9 == 0) oled.drawPixel(x, GROUND + 2 + (h >> 8) % 8, WHITE);
    if (h % 41 == 1) {
      oled.drawPixel(x, GROUND - 1, WHITE);
      oled.drawPixel(x - 1, GROUND - 2, WHITE);
      oled.drawPixel(x + 1, GROUND - 2, WHITE);
    }
  }

  static const int8_t SPIN[] = {4, 3, 2, 0, 2, 3};
  for (int i = 0; i < COUNT(gems); i++)
    if (gems[i].on) draw(GEM, (int)gems[i].x, gems[i].y, false, SPIN[(frame / 3 + i) % 6]);

  for (auto &o : obs)
    if (o.on) draw((frame / 5) & 1 ? *OBS[o.type].a : *OBS[o.type].b, (int)o.x, o.y);

  int top = GROUND - 16 - (int)jumpY;
  if (state == PLAY) draw(onGround ? (((int)(dist / 9) & 1) ? RUN1 : RUN2) : JUMP, PLAYER_X, top);
  else if (state == DYING) draw(JUMP, PLAYER_X, top, true);

  for (auto &p : parts)
    if (p.life > 0) {
      p.x += p.vx;
      p.y += p.vy;
      p.vy += 0.1f;
      p.life--;
      oled.drawPixel((int)p.x, (int)p.y, WHITE);
    }

  if (popT > 0) {
    popT--;
    oled.setCursor(PLAYER_X + 16, popY - (18 - popT) / 2);
    oled.print("+25");
  }

  oled.fillRect(0, 0, 31, 8, BLACK);
  oled.setCursor(0, 0);
  oled.printf("%05d", score());

  if (bannerT > 0 && zone >= 0) {
    const char *name = ZONE_NAME[zone];
    int w = strlen(name) * 6 + 10;
    int y = 14 - max(0, 6 - (75 - bannerT)) * 5 - max(0, 6 - bannerT) * 5;
    bannerT--;
    oled.fillRoundRect(64 - w / 2, y, w, 13, 3, BLACK);
    oled.drawRoundRect(64 - w / 2, y, w, 13, 3, WHITE);
    oled.setCursor(64 - w / 2 + 5, y + 3);
    oled.print(name);
  }
}

void drawGameOver() {
  int y = 8 + max(0, 48 - stateT * 8);
  oled.fillRoundRect(12, y, 104, 48, 5, BLACK);
  oled.drawRoundRect(12, y, 104, 48, 5, WHITE);
  oled.setTextSize(2);
  oled.setCursor(22, y + 5);
  oled.print("OY VEY!");
  oled.setTextSize(1);
  oled.setCursor(31, y + 25);
  oled.printf("SCORE %05d", score());
  oled.setCursor(31, y + 36);
  if (!newBest) oled.printf("BEST  %05d", best);
  else if ((stateT / 8) & 1) oled.print("NEW RECORD!");
}

void drawTitle() {
  oled.drawFastVLine(3, 4, 48, WHITE);
  oled.fillCircle(3, 3, 1, WHITE);
  oled.drawFastHLine(0, 52, 8, WHITE);

  // Waving flag: each column is shifted by a travelling sine, with dithered folds.
  for (int x = 0; x < 46; x++) {
    float ph = x * 0.22f - frame * 0.2f;
    int off = lroundf(sinf(ph) * (0.6f + x * 0.055f));
    bool shade = x > 6 && cosf(ph) < -0.75f;
    for (int y = 0; y < 30; y++)
      if (flagImg.getPixel(x, y) && !(shade && ((x + y) & 1))) oled.drawPixel(5 + x, 6 + y + off, WHITE);
  }

  oled.setTextSize(2);
  oled.setCursor(56, 2);
  oled.print("HALUTZ");
  oled.setCursor(88, 21);
  oled.print("RUN");
  oled.setTextSize(1);
  draw((frame / 4) & 1 ? RUN1 : RUN2, 64, 20);
  for (int x = 58; x < 126; x++)
    if ((x + frame * 2) % 8 < 4) oled.drawPixel(x, 36, WHITE);

  oled.setCursor(58, 42);
  oled.printf("BEST %05d", best);
  if ((frame / 12) & 1) {
    oled.setCursor(16, 56);
    oled.print("PRESS THE BUTTON");
  }
}

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

  flagImg.fillScreen(1);
  flagImg.fillRect(0, 3, 46, 4, 0);
  flagImg.fillRect(0, 23, 46, 4, 0);
  flagImg.drawTriangle(23, 8, 17, 18, 29, 18, 0);
  flagImg.drawTriangle(23, 22, 17, 12, 29, 12, 0);

  prefs.begin("halutz");
  best = prefs.getInt("best", 0);
  xTaskCreatePinnedToCore(ioTask, "io", 4096, nullptr, 2, nullptr, 0);
  enterTitle();
  Serial.println("HALUTZ RUN ready");
}

void loop() {
  static uint32_t nextFrame = 0;
  static int lastShake = 0;
  static bool lastInv = false;
  uint32_t now = millis();
  if ((int32_t)(now - nextFrame) < 0) {
    delay(1);
    return;
  }
  nextFrame = now - nextFrame > 100 ? now + FRAME_MS : nextFrame + FRAME_MS;
  frame++;
  stateT++;
  bool press = btnLatch.exchange(false);

  oled.clearDisplay();
  switch (state) {
    case TITLE:
      drawTitle();
      if (press) startGame();
      break;
    case PLAY:
      updatePlay(press);
      drawScene();
      break;
    case DYING:
      updateDying();
      drawScene();
      break;
    case OVER:
      drawScene();
      drawGameOver();
      if (press && stateT > 20) startGame();
      else if (stateT > 600) enterTitle();
      break;
  }

  // Crash feedback: hardware vertical shake plus a short inverted flash.
  int shake = (state == DYING && stateT < 12) ? ((stateT & 1) ? 1 : -1) * (1 + (12 - stateT) / 4) : 0;
  if (shake != lastShake) {
    oled.ssd1306_command(0x40 | (shake & 63));
    lastShake = shake;
  }
  bool inv = state == DYING && stateT < 3;
  if (inv != lastInv) {
    oled.invertDisplay(inv);
    lastInv = inv;
  }
  oled.display();
}
