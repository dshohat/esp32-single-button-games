#include <Arduino.h>
#include <U8g2lib.h>

// Hebrew song player: one button, SSD1306 OLED (I2C on GPIO21/22), passive buzzer.

const int BUTTON_PIN = 4;   // other leg to GND
const int BUZZER_PIN = 23;  // passive buzzer
const uint32_t LONG_PRESS_MS = 600;
const int NOTE_GAP_MS = 20;  // silence ending each note so repeated notes stay distinct

#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);

// ---------------- Songs ----------------

struct Note {
  uint16_t f;  // frequency in Hz, REST or LYRIC
  uint8_t d;   // length in sixteenth notes; for LYRIC, the lyric line to show
};

enum : uint16_t {
  REST = 0,
  nG4 = 392, nA4 = 440, nC5 = 523, nCs5 = 554, nD5 = 587, nE5 = 659, nF5 = 698, nFs5 = 740, nG5 = 784,
  nGs5 = 831, nA5 = 880, nBb5 = 932, nB5 = 988, nC6 = 1047, nD6 = 1175, nEb6 = 1245,
  LYRIC = 0xFFFF
};
#define LN(i) {LYRIC, i}

const char *const MAOZ_LINES[] = {
  "מעוז צור ישועתי", "לך נאה לשבח", "תיכון בית תפילתי", "ושם תודה נזבח",
  "לעת תכין מטבח", "מצר המנבח", "אז אגמור בשיר מזמור", "חנוכת המזבח",
};
const Note MAOZ[] = {
  LN(0), {nC5, 4}, {nG4, 4}, {nC5, 4}, {nF5, 4}, {nE5, 4}, {nD5, 4}, {nC5, 6},
  LN(1), {nG5, 2}, {nG5, 4}, {nA5, 4}, {nD5, 4}, {nE5, 2}, {nF5, 2}, {nE5, 4}, {nD5, 4}, {nC5, 8},
  LN(2), {nC5, 4}, {nG4, 4}, {nC5, 4}, {nF5, 4}, {nE5, 4}, {nD5, 4}, {nC5, 6},
  LN(3), {nG5, 2}, {nG5, 4}, {nA5, 4}, {nD5, 4}, {nE5, 2}, {nF5, 2}, {nE5, 4}, {nD5, 4}, {nC5, 6},
  LN(4), {nG5, 2}, {nG5, 6}, {nG5, 2}, {nA5, 4}, {nB5, 4}, {nC6, 8}, {nG5, 8},
  LN(5), {nC6, 4}, {nB5, 4}, {nA5, 4}, {nG5, 4}, {nG5, 2}, {nF5, 2}, {nE5, 2}, {nF5, 2}, {nD5, 8},
  LN(6), {nE5, 6}, {nF5, 2}, {nG5, 6}, {nA5, 2}, {nD5, 6}, {nE5, 2}, {nF5, 8},
  LN(7), {nE5, 4}, {nC5, 4}, {nA5, 4}, {nG5, 2}, {nF5, 2}, {nE5, 4}, {nF5, 4}, {nG5, 8},
  LN(6), {nE5, 6}, {nF5, 2}, {nG5, 6}, {nA5, 2}, {nD5, 6}, {nE5, 2}, {nF5, 8},
  LN(7), {nE5, 4}, {nC5, 4}, {nA5, 4}, {nG5, 2}, {nF5, 2}, {nE5, 4}, {nD5, 4}, {nC5, 8},
};

// The Hebrew lyrics of the songs below are still under copyright, so only the title is shown.

const Note YEMEI[] = {
  LN(0), {nD5, 2}, {nA5, 2}, {nA5, 2}, {nA5, 2}, {nA5, 2}, {nD6, 2}, {nA5, 2}, {nA5, 2}, {nA5, 2},
         {nA5, 4}, {nG5, 2}, {nF5, 2}, {nG5, 4}, {nE5, 2}, {nE5, 2},
  LN(0), {nA5, 2}, {nA5, 2}, {nA5, 2}, {nA5, 2}, {nD6, 2}, {nD6, 2}, {nA5, 2}, {nA5, 2}, {nA5, 4}, {nG5, 2}, {nF5, 2}, {nG5, 4}, {nE5, 4},
  LN(0), {nF5, 2}, {nG5, 2}, {nA5, 2}, {nG5, 2}, {nF5, 4}, {nF5, 2}, {nF5, 2}, {nG5, 4}, {nF5, 2}, {nE5, 2}, {nF5, 8},
  LN(0), {nF5, 2}, {nG5, 2}, {nA5, 2}, {nG5, 2}, {nF5, 4}, {nF5, 4}, {nG5, 4}, {nF5, 2}, {nE5, 2}, {nD5, 8},
  LN(0), {nA5, 4}, {nA5, 8}, {nD5, 4}, {nA5, 4}, {nA5, 8}, {nD5, 4}, {nA5, 2}, {nA5, 2}, {nA5, 2}, {nA5, 2}, {nG5, 4}, {nBb5, 4}, {nA5, 16},
  LN(0), {nA5, 2}, {nA5, 2}, {nA5, 2}, {nA5, 2}, {nBb5, 4}, {nA5, 2}, {nG5, 2}, {nA5, 4}, {nA5, 2}, {nA5, 2}, {nBb5, 4}, {nA5, 2}, {nG5, 2},
         {nF5, 4}, {nF5, 2}, {nF5, 2}, {nG5, 4}, {nF5, 2}, {nG5, 2}, {nA5, 16},
  LN(0), {nA5, 2}, {nA5, 2}, {nA5, 2}, {nA5, 2}, {nBb5, 4}, {nA5, 2}, {nG5, 2}, {nA5, 4}, {nA5, 2}, {nA5, 2}, {nBb5, 4}, {nA5, 2}, {nG5, 2},
         {nF5, 4}, {nF5, 2}, {nF5, 2}, {nG5, 4}, {nF5, 2}, {nE5, 2}, {nD5, 8},
};

const Note SEVIVON[] = {
  LN(0), {nD5, 2}, {nD5, 2}, {nE5, 4}, {nF5, 2}, {nF5, 2}, {nE5, 4},
         {nA5, 2}, {nA5, 2}, {nG5, 1}, {nF5, 1}, {nE5, 1}, {nD5, 1}, {nF5, 2}, {nF5, 2}, {nE5, 4},
  LN(0), {nD5, 2}, {nD5, 2}, {nE5, 4}, {nF5, 2}, {nF5, 2}, {nG5, 4}, {nA5, 2}, {nA5, 2}, {nA5, 2}, {nA4, 2}, {nD5, 2}, {nD5, 2}, {nD5, 4},
  LN(0), {nG5, 2}, {nG5, 2}, {nG5, 2}, {nBb5, 2}, {nA5, 2}, {nA5, 2}, {nF5, 1}, {nE5, 1}, {nD5, 2},
         {nCs5, 2}, {nCs5, 2}, {nA4, 2}, {nCs5, 2}, {nD5, 3}, {nE5, 1}, {nF5, 1}, {nE5, 1}, {nD5, 2},
  LN(0), {nG5, 2}, {nG5, 2}, {nG5, 2}, {nBb5, 2}, {nA5, 2}, {nA5, 2}, {nF5, 1}, {nE5, 1}, {nD5, 2},
         {nCs5, 2}, {nCs5, 2}, {nA4, 2}, {nCs5, 2}, {nD5, 2}, {nD5, 2}, {nD5, 4},
};

const Note MI_YEMALEL[] = {
  LN(0), {nF5, 2}, {nF5, 1}, {nC5, 1}, {nF5, 2}, {nG5, 2}, {nA5, 2}, {nA5, 1}, {nF5, 1}, {nG5, 2}, {nBb5, 2}, {nA5, 4}, {nC6, 4}, {nA5, 4}, {nC6, 4},
  LN(0), {nF5, 2}, {nF5, 1}, {nC5, 1}, {nF5, 2}, {nG5, 2}, {nA5, 2}, {nA5, 1}, {nF5, 1}, {nG5, 2}, {nBb5, 2}, {nF5, 4}, {nD5, 4}, {nF5, 8},
  LN(0), {nF5, 2}, {nF5, 1}, {nC5, 1}, {nF5, 2}, {nG5, 2}, {nA5, 2}, {nA5, 1}, {nF5, 1}, {nG5, 2}, {nBb5, 2}, {nA5, 4}, {nC6, 4}, {nA5, 4}, {nC6, 4},
  LN(0), {nF5, 2}, {nF5, 1}, {nC5, 1}, {nF5, 2}, {nG5, 2}, {nA5, 2}, {nA5, 1}, {nF5, 1}, {nG5, 2}, {nBb5, 2}, {nF5, 4}, {nD5, 4}, {nF5, 8},
  LN(0), {nC6, 12}, {REST, 4}, {nC6, 2}, {nEb6, 2}, {nD6, 2}, {nC6, 2}, {nB5, 2}, {nG5, 2}, {nB5, 2}, {nD6, 2},
  LN(0), {nC6, 12}, {REST, 4}, {nC6, 2}, {nEb6, 2}, {nD6, 2}, {nC6, 2}, {nB5, 2}, {nG5, 2}, {nB5, 2}, {nD6, 2},
  LN(0), {nC6, 12}, {REST, 4}, {nGs5, 2}, {nC6, 2}, {nB5, 2}, {nGs5, 2}, {nG5, 2}, {nGs5, 2}, {nB5, 2}, {nC6, 2},
  LN(0), {nC6, 12}, {REST, 4}, {nGs5, 2}, {nC6, 2}, {nB5, 2}, {nGs5, 2}, {nG5, 2}, {nF5, 2}, {nG5, 2}, {nGs5, 2}, {nF5, 12},
};

const Note NARIMA[] = {
  LN(0), {nA5, 8}, {nFs5, 6}, {nG5, 2}, {nA5, 8}, {nD5, 8}, {nE5, 2}, {nFs5, 2}, {nG5, 2}, {nA5, 2}, {nG5, 4}, {nFs5, 4}, {nE5, 16},
  LN(0), {nFs5, 2}, {nG5, 2}, {nA5, 2}, {nB5, 2}, {nA5, 4}, {nA5, 4}, {nD6, 8}, {nA5, 8}, {nG5, 4}, {nFs5, 2}, {nE5, 2}, {nE5, 6}, {nD5, 2}, {nD5, 16},
  LN(0), {nFs5, 2}, {nE5, 2}, {nFs5, 2}, {nG5, 2}, {nFs5, 4}, {nFs5, 4}, {nE5, 8}, {nD5, 8}, {nG5, 4}, {nFs5, 4}, {nE5, 4}, {nD5, 4}, {nCs5, 16},
  LN(0), {nD5, 2}, {nCs5, 2}, {nD5, 2}, {nE5, 2}, {nD5, 4}, {nD5, 4}, {nB5, 8}, {nGs5, 8}, {nA5, 4}, {nB5, 2}, {nA5, 2}, {nGs5, 6}, {nA5, 2}, {nA5, 16},
  LN(0), {nA5, 8}, {nFs5, 6}, {nG5, 2}, {nA5, 8}, {nD5, 8}, {nE5, 2}, {nFs5, 2}, {nG5, 2}, {nA5, 2}, {nG5, 4}, {nFs5, 4}, {nE5, 16},
  LN(0), {nFs5, 2}, {nG5, 2}, {nA5, 2}, {nB5, 2}, {nA5, 4}, {nA5, 4}, {nD6, 8}, {nA5, 8}, {nG5, 4}, {nFs5, 2}, {nE5, 2}, {nE5, 6}, {nD5, 2}, {nD5, 16},
};

struct Song {
  const char *title;
  const char *const *lines;  // nullptr: no lyrics, the title is shown instead
  const Note *notes;
  int count;
  int unitMs;  // duration of a sixteenth note
};

const Song SONGS[] = {
  {"מעוז צור", MAOZ_LINES, MAOZ, COUNT(MAOZ), 150},
  {"ימי החנוכה", nullptr, YEMEI, COUNT(YEMEI), 120},
  {"סביבון סוב סוב סוב", nullptr, SEVIVON, COUNT(SEVIVON), 125},
  {"מי ימלל", nullptr, MI_YEMALEL, COUNT(MI_YEMALEL), 115},
  {"הבה נרימה", nullptr, NARIMA, COUNT(NARIMA), 120},
};

// ---------------- State ----------------

enum State { INTRO, MENU, PLAY };
State state = INTRO;
int sel;
const Song *song;
int noteIdx, line;
uint32_t noteEnd;
bool sounding;

// ---------------- Display ----------------

// U8g2 draws left to right, so Hebrew is reversed by code point, then centered.
void drawHebrew(const char *s, int y, int len = -1) {
  if (len < 0) len = strlen(s);
  char buf[64];
  int out = len;
  buf[out] = 0;
  for (int i = 0; i < len;) {
    int n = 1;
    while (i + n < len && (s[i + n] & 0xC0) == 0x80) n++;
    out -= n;
    memcpy(buf + out, s + i, n);
    i += n;
  }
  oled.drawUTF8((128 - oled.getUTF8Width(buf)) / 2, y, buf);
}

void drawIntro() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_unifont_t_hebrew);
  drawHebrew("שירי חנוכה", 16);
  oled.setFont(u8g2_font_6x13_t_hebrew);
  drawHebrew("לחיצה קצרה: השיר הבא", 38);
  drawHebrew("לחיצה ארוכה: נגן", 54);
  oled.sendBuffer();
}

void drawMenu() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x13_t_hebrew);
  for (int i = 0; i < COUNT(SONGS); i++) drawHebrew(SONGS[i].title, i * 12 + 12);
  oled.setDrawColor(2);  // XOR: inverts the selected row
  oled.drawBox(0, sel * 12 + 2, 128, 12);
  oled.setDrawColor(1);
  oled.sendBuffer();
}

void drawPlaying() {
  const char *s = song->lines ? song->lines[line] : song->title;
  int len = strlen(s);
  oled.clearBuffer();
  if (song->lines) {
    oled.setFont(u8g2_font_6x13B_t_hebrew);
    drawHebrew(song->title, 11);
    oled.drawHLine(0, 14, 128);
  }
  oled.setFont(u8g2_font_unifont_t_hebrew);
  if (oled.getUTF8Width(s) <= 128) {
    drawHebrew(s, 42);
  } else {
    // Too wide for one row: wrap at the space nearest the middle.
    int split = 0;
    for (int i = 0; i < len; i++)
      if (s[i] == ' ' && abs(i * 2 - len) < abs(split * 2 - len)) split = i;
    drawHebrew(s, 34, split);
    drawHebrew(s + split + 1, 52, len - split - 1);
  }
  oled.drawBox(0, 61, 128 * noteIdx / song->count, 3);
  oled.sendBuffer();
}

// ---------------- Playback ----------------

void stopSong() {
  noTone(BUZZER_PIN);
  state = MENU;
  drawMenu();
}

void nextNote() {
  bool newLine = false;
  if (noteIdx < song->count && song->notes[noteIdx].f == LYRIC) {
    line = song->notes[noteIdx++].d;
    newLine = true;
  }
  if (noteIdx >= song->count) {
    stopSong();
    return;
  }
  const Note &n = song->notes[noteIdx++];
  if (n.f != REST) tone(BUZZER_PIN, n.f);
  sounding = n.f != REST;
  noteEnd += n.d * song->unitMs;
  if (newLine) drawPlaying();
}

void startSong() {
  song = &SONGS[sel];
  noteIdx = 0;
  noteEnd = millis();
  state = PLAY;
  nextNote();
}

// ---------------- Button ----------------

enum Press { NO_PRESS, SHORT_PRESS, LONG_PRESS };

Press readButton() {
  static bool down = false, held = false;
  static uint32_t changedAt = 0;
  bool raw = digitalRead(BUTTON_PIN) == LOW;
  uint32_t now = millis();
  if (raw != down && now - changedAt > 30) {
    down = raw;
    changedAt = now;
    if (down) held = false;
    else if (!held) return SHORT_PRESS;
  }
  if (down && !held && now - changedAt >= LONG_PRESS_MS) {
    held = true;
    return LONG_PRESS;
  }
  return NO_PRESS;
}

void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  oled.setBusClock(400000);
  oled.begin();
  drawIntro();
}

void loop() {
  Press press = readButton();
  switch (state) {
    case INTRO:
      if (press) {
        state = MENU;
        drawMenu();
      }
      break;
    case MENU:
      if (press == SHORT_PRESS) {
        sel = (sel + 1) % COUNT(SONGS);
        drawMenu();
      } else if (press == LONG_PRESS) {
        startSong();
      }
      break;
    case PLAY: {
      int32_t left = noteEnd - millis();
      if (press) stopSong();
      else if (left <= 0) nextNote();
      else if (sounding && left <= NOTE_GAP_MS) {
        noTone(BUZZER_PIN);
        sounding = false;
      }
      break;
    }
  }
}