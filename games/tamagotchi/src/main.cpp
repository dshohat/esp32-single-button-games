#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <Wire.h>
#include <esp_system.h>

static constexpr uint8_t PIN_BUTTON = 4;
static constexpr uint8_t PIN_BUZZER = 23;
static constexpr uint8_t PIN_SDA = 21;
static constexpr uint8_t PIN_SCL = 22;
static constexpr uint8_t OLED_WIDTH = 128;
static constexpr uint8_t OLED_HEIGHT = 64;

static constexpr uint16_t DEBOUNCE_MS = 30;
static constexpr uint16_t DOUBLE_CLICK_MS = 300;
static constexpr uint16_t LONG_PRESS_MS = 600;
static constexpr uint16_t FRAME_MS = 100;
static constexpr uint16_t HATCH_MS = 1500;
static constexpr uint16_t RESTART_LOCK_MS = 1500;
static constexpr uint16_t WANDER_STEP_MS = 200;
static constexpr int GROUND_Y = 53;
static constexpr int PET_MIN_X = 40;
static constexpr int PET_MAX_X = 80;
static constexpr int BABY_RADIUS = 8;
static constexpr int ADULT_RADIUS = 12;
static constexpr int GROW_AGE_MIN = 2;
static constexpr int MAX_POOPS = 3;
static constexpr int ALERT_PERIOD_S = 15;

enum Menu { MENU_FEED, MENU_PLAY, MENU_SLEEP, MENU_CLEAN, MENU_HEAL, MENU_COUNT };
enum class Stage { Egg, Hatching, Alive, Dead };
enum class Anim { None, Eat, Play, Clean, Heal, Refuse };
enum class Face { Happy, Neutral, Sad, Asleep, Sick, Chew };
enum class ButtonEvent { None, Click, DoubleClick, LongPress };

static const uint8_t ICON_FEED[] PROGMEM = {0x3C, 0x7E, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x7E};
static const uint8_t ICON_PLAY[] PROGMEM = {0x3C, 0x5A, 0x99, 0xE7, 0xE7, 0x99, 0x5A, 0x3C};
static const uint8_t ICON_SLEEP[] PROGMEM = {0x38, 0x70, 0xE0, 0xE0, 0xE1, 0xF3, 0x7E, 0x3C};
static const uint8_t ICON_CLEAN[] PROGMEM = {0x10, 0x10, 0x38, 0x38, 0x7C, 0x7C, 0x7C, 0x38};
static const uint8_t ICON_HEAL[] PROGMEM = {0x3C, 0x3C, 0xFF, 0xFF, 0xFF, 0xFF, 0x3C, 0x3C};
static const uint8_t ICON_POOP[] PROGMEM = {0x10, 0x38, 0x38, 0x7C, 0x7C, 0xFE, 0xFE, 0x00};
static const uint8_t* const MENU_ICONS[MENU_COUNT] = {ICON_FEED, ICON_PLAY, ICON_SLEEP, ICON_CLEAN, ICON_HEAL};

struct Note {
  uint16_t freq;
  uint16_t ms;
};
static const Note MELODY_CLICK[] = {{1800, 25}};
static const Note MELODY_EAT[] = {{600, 60}, {0, 60}, {800, 60}, {0, 60}, {600, 60}, {0, 60}, {800, 60}};
static const Note MELODY_PLAY[] = {{523, 90}, {659, 90}, {784, 90}, {1047, 150}, {784, 90}, {1047, 200}};
static const Note MELODY_SLEEP[] = {{784, 180}, {659, 180}, {523, 300}};
static const Note MELODY_WAKE[] = {{523, 120}, {659, 120}, {784, 200}};
static const Note MELODY_CLEAN[] = {{1200, 50}, {1500, 50}, {1800, 50}, {2100, 80}};
static const Note MELODY_HEAL[] = {{880, 100}, {1175, 100}, {1760, 200}};
static const Note MELODY_REFUSE[] = {{220, 120}, {0, 40}, {220, 120}};
static const Note MELODY_ALERT[] = {{2000, 80}, {0, 80}, {2000, 80}, {0, 80}, {2000, 80}};
static const Note MELODY_POOP[] = {{300, 80}, {200, 120}};
static const Note MELODY_SICK[] = {{400, 150}, {350, 150}, {300, 250}};
static const Note MELODY_HATCH[] = {{523, 100}, {659, 100}, {784, 100}, {1047, 100}, {1319, 100}, {1568, 250}};
static const Note MELODY_GROW[] = {{659, 100}, {784, 100}, {988, 100}, {1319, 300}};
static const Note MELODY_DEATH[] = {{494, 300}, {466, 300}, {440, 300}, {392, 600}};

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

Stage stage = Stage::Egg;
Anim anim = Anim::None;
bool showStats = false;
int selected = MENU_FEED;
int fullness = 0;
int happy = 0;
int energy = 0;
int health = 0;
int poops = 0;
int poopCountdown = 0;
int petX = OLED_WIDTH / 2;
int petDir = 1;
bool sick = false;
bool asleep = false;
uint16_t ageMin = 0;
uint32_t aliveSec = 0;
uint32_t lastAlertSec = 0;
uint32_t stageStartedAt = 0;
uint32_t animStartedAt = 0;
uint16_t animMs = 0;
uint32_t lastTickAt = 0;
uint32_t lastFrameAt = 0;
uint32_t lastWanderAt = 0;

bool rawButtonState = HIGH;
bool stableButtonState = HIGH;
bool longFired = false;
uint8_t pendingClicks = 0;
uint32_t buttonChangedAt = 0;
uint32_t pressedAt = 0;
uint32_t lastReleaseAt = 0;

const Note* melody = nullptr;
size_t melodyLen = 0;
size_t melodyIdx = 0;
uint32_t noteEndsAt = 0;

void initBuzzer() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_BUZZER, 2000, 8);
#else
  ledcSetup(0, 2000, 8);
  ledcAttachPin(PIN_BUZZER, 0);
#endif
}

void writeTone(uint16_t frequency) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(PIN_BUZZER, frequency);
#else
  ledcWriteTone(0, frequency);
#endif
}

template <size_t N>
void play(const Note (&notes)[N]) {
  melody = notes;
  melodyLen = N;
  melodyIdx = 0;
  writeTone(notes[0].freq);
  noteEndsAt = millis() + notes[0].ms;
}

void updateSound(uint32_t now) {
  if (melody == nullptr || static_cast<int32_t>(now - noteEndsAt) < 0) return;
  if (++melodyIdx >= melodyLen) {
    writeTone(0);
    melody = nullptr;
    return;
  }
  writeTone(melody[melodyIdx].freq);
  noteEndsAt = now + melody[melodyIdx].ms;
}

ButtonEvent pollButton(uint32_t now) {
  const bool reading = digitalRead(PIN_BUTTON);
  if (reading != rawButtonState) {
    rawButtonState = reading;
    buttonChangedAt = now;
  }
  if (now - buttonChangedAt >= DEBOUNCE_MS && stableButtonState != rawButtonState) {
    stableButtonState = rawButtonState;
    if (stableButtonState == LOW) {
      pressedAt = now;
      longFired = false;
    } else if (!longFired) {
      lastReleaseAt = now;
      if (++pendingClicks == 2) {
        pendingClicks = 0;
        return ButtonEvent::DoubleClick;
      }
    }
  }
  if (stableButtonState == LOW && !longFired && now - pressedAt >= LONG_PRESS_MS) {
    longFired = true;
    pendingClicks = 0;
    return ButtonEvent::LongPress;
  }
  // A single click is only reported once the double-click window has passed.
  if (pendingClicks == 1 && stableButtonState == HIGH && now - lastReleaseAt >= DOUBLE_CLICK_MS) {
    pendingClicks = 0;
    return ButtonEvent::Click;
  }
  return ButtonEvent::None;
}

void clampStats() {
  fullness = constrain(fullness, 0, 100);
  happy = constrain(happy, 0, 100);
  energy = constrain(energy, 0, 100);
  health = constrain(health, 0, 100);
}

const char* currentNeed() {
  if (sick) return "SICK!";
  if (fullness < 25) return "HUNGRY!";
  if (poops > 0) return "DIRTY!";
  if (energy < 20) return "SLEEPY!";
  if (happy < 25) return "BORED!";
  return nullptr;
}

Face moodFace() {
  if (sick) return Face::Sick;
  if (asleep) return Face::Asleep;
  const int lowest = min(fullness, min(happy, energy));
  if (lowest < 25) return Face::Sad;
  if (lowest < 50) return Face::Neutral;
  return Face::Happy;
}

int petRadius() { return ageMin >= GROW_AGE_MIN ? ADULT_RADIUS : BABY_RADIUS; }

const char* menuName(int item) {
  switch (item) {
    case MENU_FEED: return "FEED";
    case MENU_PLAY: return "PLAY";
    case MENU_SLEEP: return asleep ? "WAKE" : "SLEEP";
    case MENU_CLEAN: return "CLEAN";
    default: return "HEAL";
  }
}

void startAnim(Anim next, uint16_t durationMs, uint32_t now) {
  anim = next;
  animStartedAt = now;
  animMs = durationMs;
}

void refuse(uint32_t now) {
  startAnim(Anim::Refuse, 600, now);
  play(MELODY_REFUSE);
}

void doAction(uint32_t now) {
  switch (selected) {
    case MENU_FEED:
      if (asleep || fullness > 90) return refuse(now);
      fullness += 30;
      startAnim(Anim::Eat, 1600, now);
      play(MELODY_EAT);
      break;

    case MENU_PLAY:
      if (asleep || sick || energy < 15) return refuse(now);
      happy += 25;
      energy -= 10;
      fullness -= 5;
      startAnim(Anim::Play, 2000, now);
      play(MELODY_PLAY);
      break;

    case MENU_SLEEP:
      asleep = !asleep;
      if (asleep) play(MELODY_SLEEP); else play(MELODY_WAKE);
      break;

    case MENU_CLEAN:
      if (poops == 0) return refuse(now);
      happy += 5;
      startAnim(Anim::Clean, 1200, now);
      play(MELODY_CLEAN);
      break;

    case MENU_HEAL:
      if (!sick) return refuse(now);
      sick = false;
      health += 10;
      startAnim(Anim::Heal, 1200, now);
      play(MELODY_HEAL);
      break;
  }
  clampStats();
}

void hatch(uint32_t now) {
  stage = Stage::Alive;
  anim = Anim::None;
  showStats = false;
  selected = MENU_FEED;
  fullness = 60;
  happy = 60;
  energy = 80;
  health = 100;
  poops = 0;
  poopCountdown = random(45, 80);
  petX = OLED_WIDTH / 2;
  sick = false;
  asleep = false;
  ageMin = 0;
  aliveSec = 0;
  lastAlertSec = 0;
  lastTickAt = now;
  Serial.println("Pet hatched");
}

void die(uint32_t now) {
  stage = Stage::Dead;
  stageStartedAt = now;
  anim = Anim::None;
  showStats = false;
  play(MELODY_DEATH);
  Serial.printf("Pet died at age %u min\n", ageMin);
}

void tickSecond(uint32_t now) {
  aliveSec++;
  if (aliveSec % 60 == 0) {
    ageMin++;
    if (ageMin == GROW_AGE_MIN) play(MELODY_GROW);
  }

  if (asleep) {
    energy += 2;
    if (aliveSec % 6 == 0) fullness--;
    if (energy >= 100) {
      asleep = false;
      play(MELODY_WAKE);
    }
  } else {
    if (aliveSec % 3 == 0) fullness--;
    if (aliveSec % 4 == 0) happy -= (poops > 0 || sick) ? 2 : 1;
    if (aliveSec % 5 == 0) energy--;
    if (--poopCountdown <= 0) {
      poopCountdown = random(45, 80);
      if (poops < MAX_POOPS) {
        poops++;
        play(MELODY_POOP);
      }
    }
    if (energy <= 0) {
      asleep = true;
      happy -= 15;
      play(MELODY_SLEEP);
    }
  }

  if (!sick && aliveSec % 15 == 0) {
    const int chancePercent = poops * 12 + (fullness <= 0 ? 15 : 0);
    if (random(100) < chancePercent) {
      sick = true;
      play(MELODY_SICK);
    }
  }

  if (sick && aliveSec % 2 == 0) health--;
  if (fullness <= 0 && aliveSec % 3 == 0) health--;
  if (!sick && fullness > 20 && aliveSec % 3 == 0) health++;
  clampStats();
  if (health <= 0) return die(now);

  if (!asleep && melody == nullptr && currentNeed() != nullptr &&
      aliveSec - lastAlertSec >= ALERT_PERIOD_S) {
    lastAlertSec = aliveSec;
    play(MELODY_ALERT);
  }
}

void updateAlive(uint32_t now, ButtonEvent event) {
  if (anim != Anim::None && now - animStartedAt >= animMs) {
    if (anim == Anim::Clean) poops = 0;
    anim = Anim::None;
  }

  if (event != ButtonEvent::None) {
    if (showStats) {
      showStats = false;
      play(MELODY_CLICK);
    } else if (anim == Anim::None) {
      if (event == ButtonEvent::Click) {
        selected = (selected + 1) % MENU_COUNT;
        play(MELODY_CLICK);
      } else if (event == ButtonEvent::DoubleClick) {
        doAction(now);
      } else {
        showStats = true;
        play(MELODY_CLICK);
      }
    }
  }

  if (now - lastWanderAt >= WANDER_STEP_MS) {
    lastWanderAt = now;
    if (!asleep && !sick && anim == Anim::None) {
      petX += petDir;
      if (petX <= PET_MIN_X || petX >= PET_MAX_X) petDir = -petDir;
    }
  }

  if (now - lastTickAt >= 1000) {
    lastTickAt += 1000;
    tickSecond(now);
  }
}

void drawPet(int cx, int cy, int r, Face face, bool blink) {
  const int earOffset = r * 2 / 3;
  display.fillCircle(cx - earOffset, cy - earOffset - 1, r / 3, SSD1306_WHITE);
  display.fillCircle(cx + earOffset, cy - earOffset - 1, r / 3, SSD1306_WHITE);
  display.fillCircle(cx, cy, r, SSD1306_WHITE);
  display.fillRect(cx - r / 2 - 1, cy + r - 1, 3, 3, SSD1306_WHITE);
  display.fillRect(cx + r / 2 - 1, cy + r - 1, 3, 3, SSD1306_WHITE);

  const int eyeDx = (r + 2) / 3;
  const int eyeY = cy - r / 4;
  for (int side = -1; side <= 1; side += 2) {
    const int ex = cx + side * eyeDx;
    if (face == Face::Sick) {
      display.drawLine(ex - 1, eyeY - 1, ex + 1, eyeY + 1, SSD1306_BLACK);
      display.drawLine(ex - 1, eyeY + 1, ex + 1, eyeY - 1, SSD1306_BLACK);
    } else if (face == Face::Asleep || blink) {
      display.drawFastHLine(ex - 1, eyeY, 3, SSD1306_BLACK);
    } else {
      display.fillRect(ex - 1, eyeY - 1, 2, 3, SSD1306_BLACK);
    }
  }

  const int mouthY = cy + r / 3;
  switch (face) {
    case Face::Happy:
      display.drawPixel(cx - 2, mouthY, SSD1306_BLACK);
      display.drawFastHLine(cx - 1, mouthY + 1, 3, SSD1306_BLACK);
      display.drawPixel(cx + 2, mouthY, SSD1306_BLACK);
      break;
    case Face::Sad:
    case Face::Sick:
      display.drawPixel(cx - 2, mouthY + 1, SSD1306_BLACK);
      display.drawFastHLine(cx - 1, mouthY, 3, SSD1306_BLACK);
      display.drawPixel(cx + 2, mouthY + 1, SSD1306_BLACK);
      break;
    case Face::Chew:
      display.fillRect(cx - 1, mouthY - 1, 3, 3, SSD1306_BLACK);
      break;
    case Face::Asleep:
      display.drawFastHLine(cx - 1, mouthY, 2, SSD1306_BLACK);
      break;
    case Face::Neutral:
      display.drawFastHLine(cx - 2, mouthY, 5, SSD1306_BLACK);
      break;
  }
}

void drawMain(uint32_t now) {
  for (int i = 0; i < MENU_COUNT; i++) {
    const int x = 2 + i * 25;
    if (i == selected) display.fillRoundRect(x, 0, 22, 12, 3, SSD1306_WHITE);
    display.drawBitmap(x + 7, 2, MENU_ICONS[i], 8, 8, i == selected ? SSD1306_BLACK : SSD1306_WHITE);
  }
  display.drawFastHLine(0, GROUND_Y, OLED_WIDTH, SSD1306_WHITE);

  const int elapsed = now - animStartedAt;
  const int r = petRadius();
  int cx = petX;
  int cy = GROUND_Y - r - 2;
  int sweepX = -1;
  Face face = moodFace();

  switch (anim) {
    case Anim::Eat:
      face = (elapsed / 200) % 2 ? Face::Chew : Face::Neutral;
      if (elapsed < animMs * 2 / 3) display.drawBitmap(cx + r + 4, cy - 2, ICON_FEED, 8, 8, SSD1306_WHITE);
      break;
    case Anim::Play: {
      const int t = elapsed % 500;
      const int jump = t * (500 - t) / 6250;
      cy -= jump;
      display.drawBitmap(cx - r - 14, GROUND_Y - 19 + jump, ICON_PLAY, 8, 8, SSD1306_WHITE);
      face = Face::Happy;
      break;
    }
    case Anim::Clean:
      sweepX = OLED_WIDTH * elapsed / animMs;
      display.drawFastVLine(sweepX, 14, GROUND_Y - 14, SSD1306_WHITE);
      break;
    case Anim::Heal:
      if ((elapsed / 150) % 2 == 0) display.drawBitmap(cx - 4, cy - r - 12, ICON_HEAL, 8, 8, SSD1306_WHITE);
      break;
    case Anim::Refuse:
      if (!asleep) cx += (elapsed / 75) % 2 ? 2 : -2;
      break;
    case Anim::None:
      if (!asleep) cy -= (now / 400) % 2;
      break;
  }

  const bool blink = now % 3000 < 150;
  drawPet(cx, cy, r, face, blink);

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  if (asleep) {
    display.setCursor(cx + r + 3, cy - r - ((now / 600) % 2 ? 6 : 2));
    display.print("Zz");
  }
  for (int i = 0; i < poops; i++) {
    const int x = 100 + i * 9;
    if (x > sweepX) display.drawBitmap(x, GROUND_Y - 8, ICON_POOP, 8, 8, SSD1306_WHITE);
  }

  display.setCursor(0, 56);
  display.print(menuName(selected));
  const char* need = currentNeed();
  if (need != nullptr && (now / 500) % 2 == 0) {
    display.setCursor(OLED_WIDTH - strlen(need) * 6, 56);
    display.print(need);
  }
}

void drawBar(int y, const char* label, int value) {
  display.setCursor(0, y);
  display.print(label);
  display.drawRect(44, y, 84, 8, SSD1306_WHITE);
  display.fillRect(46, y + 2, 80 * value / 100, 4, SSD1306_WHITE);
}

void drawStats() {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  drawBar(0, "FOOD", fullness);
  drawBar(12, "HAPPY", happy);
  drawBar(24, "ENERGY", energy);
  drawBar(36, "HEALTH", health);
  display.setCursor(0, 52);
  display.printf("AGE %u min  %s", ageMin, sick ? "SICK" : asleep ? "ASLEEP" : "OK");
}

void drawEgg(uint32_t now) {
  const bool hatching = stage == Stage::Hatching;
  const int cy = 30;
  int cx = OLED_WIDTH / 2;
  if (hatching) {
    cx += ((now - stageStartedAt) / 60) % 2 ? 3 : -3;
  } else {
    cx += (now / 500) % 2 ? 1 : -1;
  }

  display.fillRoundRect(cx - 10, cy - 13, 20, 26, 10, SSD1306_WHITE);
  display.fillCircle(cx - 4, cy - 5, 2, SSD1306_BLACK);
  display.fillCircle(cx + 4, cy + 4, 2, SSD1306_BLACK);
  display.fillCircle(cx - 3, cy + 8, 1, SSD1306_BLACK);
  if (hatching) {
    display.drawLine(cx - 10, cy, cx - 5, cy - 3, SSD1306_BLACK);
    display.drawLine(cx - 5, cy - 3, cx, cy + 2, SSD1306_BLACK);
    display.drawLine(cx, cy + 2, cx + 5, cy - 3, SSD1306_BLACK);
    display.drawLine(cx + 5, cy - 3, cx + 10, cy, SSD1306_BLACK);
  }

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(34, 0);
  display.print("TAMAGOTCHI");
  if (!hatching) {
    display.setCursor(22, 56);
    display.print("Press to hatch");
  }
}

void drawDead() {
  display.fillRoundRect(12, 12, 30, 42, 12, SSD1306_WHITE);
  display.fillRect(12, 40, 30, 14, SSD1306_WHITE);
  display.drawFastHLine(4, 54, 46, SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(18, 28);
  display.print("RIP");
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(56, 12);
  display.print("Lived");
  display.setCursor(56, 22);
  display.printf("%u min", ageMin);
  display.setCursor(56, 44);
  display.print("Press for");
  display.setCursor(56, 54);
  display.print("a new egg");
}

void draw(uint32_t now) {
  display.clearDisplay();
  switch (stage) {
    case Stage::Egg:
    case Stage::Hatching:
      drawEgg(now);
      break;
    case Stage::Alive:
      if (showStats) drawStats(); else drawMain(now);
      break;
    case Stage::Dead:
      drawDead();
      break;
  }
  display.display();
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  initBuzzer();
  Wire.begin(PIN_SDA, PIN_SCL);
  randomSeed(esp_random());

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C) &&
      !display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
    Serial.println("OLED not found at 0x3C or 0x3D");
    while (true) delay(1000);
  }
  Serial.println("Tamagotchi ready");
}

void loop() {
  const uint32_t now = millis();
  const ButtonEvent event = pollButton(now);
  updateSound(now);

  switch (stage) {
    case Stage::Egg:
      if (event != ButtonEvent::None) {
        stage = Stage::Hatching;
        stageStartedAt = now;
        play(MELODY_HATCH);
      }
      break;

    case Stage::Hatching:
      if (now - stageStartedAt >= HATCH_MS) hatch(now);
      break;

    case Stage::Alive:
      updateAlive(now, event);
      break;

    case Stage::Dead:
      if (event != ButtonEvent::None && now - stageStartedAt >= RESTART_LOCK_MS) stage = Stage::Egg;
      break;
  }

  if (now - lastFrameAt >= FRAME_MS) {
    lastFrameAt = now;
    draw(now);
  }

  delay(5);
}
