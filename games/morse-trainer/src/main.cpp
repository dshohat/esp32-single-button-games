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

static constexpr uint16_t DASH_MS = 250;
static constexpr uint16_t LETTER_GAP_MS = 700;
static constexpr uint16_t KEY_TONE_HZ = 650;
static constexpr uint8_t MAX_TYPED = 5;
static constexpr uint8_t START_LIVES = 3;
static constexpr uint8_t TARGETS_PER_LEVEL = 5;
static constexpr uint8_t LETTER_POINTS = 10;
static constexpr uint16_t SPEED_BONUS_WINDOW_MS = 5000;

static const char* const MORSE[26] = {
    ".-",   "-...", "-.-.", "-..",  ".",    "..-.", "--.",  "....", "..",
    ".---", "-.-",  ".-..", "--",   "-.",   "---",  ".--.", "--.-", ".-.",
    "...",  "-",    "..-",  "...-", ".--",  "-..-", "-.--", "--.."};

static const char* const WORDS[] = {"SOS",  "CAT",  "DOG",  "SUN",  "TEA",  "ANT",  "NET",
                                    "TEN",  "MAN",  "RED",  "CODE", "GAME", "TIME", "NOTE",
                                    "STAR", "MOON", "WIFI", "CHIP", "BEEP", "INTEL"};
static constexpr uint8_t WORD_COUNT = sizeof(WORDS) / sizeof(WORDS[0]);

enum class State { Title, Play, Wrong, GameOver };

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
State state = State::Title;

bool buttonDown = false;
bool pressedEdge = false;
bool releasedEdge = false;
uint32_t buttonChangedAt = 0;

bool toneTimed = false;
uint32_t toneOffAt = 0;

uint8_t level = 1;
uint8_t lives = START_LIVES;
uint8_t streak = 0;
uint8_t targetsDone = 0;
uint32_t score = 0;
uint32_t bestScore = 0;

char target[6] = "";
char previousTarget[6] = "";
uint8_t pos = 0;
char typed[MAX_TYPED + 1] = "";
uint8_t typedLen = 0;
bool pressActive = false;
uint32_t pressStartedAt = 0;
uint32_t lastReleaseAt = 0;
uint32_t letterStartedAt = 0;

uint8_t lastPoints = 0;
uint32_t pointsShownUntil = 0;
uint32_t deadline = 0;

void initBuzzer() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_BUZZER, 2000, 8);
#else
  ledcSetup(0, 2000, 8);
  ledcAttachPin(PIN_BUZZER, 0);
#endif
}

void setTone(uint16_t frequency) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(PIN_BUZZER, frequency);
#else
  ledcWriteTone(0, frequency);
#endif
}

void playTone(uint16_t frequency, uint16_t durationMs) {
  setTone(frequency);
  toneOffAt = millis() + durationMs;
  toneTimed = true;
}

void updateTone(uint32_t now) {
  if (toneTimed && static_cast<int32_t>(now - toneOffAt) >= 0) {
    setTone(0);
    toneTimed = false;
  }
}

void updateButton(uint32_t now) {
  pressedEdge = false;
  releasedEdge = false;
  const bool down = digitalRead(PIN_BUTTON) == LOW;
  // React on the first edge, then ignore bounce for 30 ms.
  if (down != buttonDown && now - buttonChangedAt >= 30) {
    buttonDown = down;
    buttonChangedAt = now;
    pressedEdge = down;
    releasedEdge = !down;
  }
}

bool hintVisible() { return level <= 3 || level == 5; }

void pickTarget(uint32_t now) {
  strcpy(previousTarget, target);
  do {
    if (level >= 5) {
      strcpy(target, WORDS[random(WORD_COUNT)]);
    } else {
      const char* pool = level == 1   ? "ETAINM"
                         : level == 2 ? "ETAINMSORDUKWG"
                                      : "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
      target[0] = pool[random(strlen(pool))];
      target[1] = '\0';
    }
  } while (strcmp(target, previousTarget) == 0);
  pos = 0;
  typedLen = 0;
  letterStartedAt = now;
}

void newGame(uint32_t now) {
  score = 0;
  level = 1;
  lives = START_LIVES;
  streak = 0;
  targetsDone = 0;
  pressActive = false;
  pointsShownUntil = now;
  state = State::Play;
  pickTarget(now);
}

void commitLetter(uint32_t now) {
  typed[typedLen] = '\0';
  if (strcmp(typed, MORSE[target[pos] - 'A']) != 0) {
    lives--;
    streak = 0;
    state = State::Wrong;
    deadline = now + 2000;
    display.invertDisplay(true);
    playTone(150, 400);
    return;
  }

  if (streak < 255) streak++;
  const uint32_t elapsed = lastReleaseAt - letterStartedAt;
  const uint8_t speedBonus =
      elapsed < SPEED_BONUS_WINDOW_MS ? (SPEED_BONUS_WINDOW_MS - elapsed) / 500 : 0;
  const uint8_t streakBonus = 2 * (streak > 10 ? 10 : streak - 1);
  lastPoints = LETTER_POINTS + speedBonus + streakBonus;
  score += lastPoints;
  pointsShownUntil = now + 900;

  pos++;
  typedLen = 0;
  letterStartedAt = now;
  if (target[pos] != '\0') {
    playTone(1400, 50);
    return;
  }
  if (++targetsDone % TARGETS_PER_LEVEL == 0) {
    level++;
    playTone(1900, 200);
  } else {
    playTone(1400, 50);
  }
  pickTarget(now);
}

void updatePlay(uint32_t now) {
  if (pressedEdge) {
    pressActive = true;
    pressStartedAt = now;
    toneTimed = false;
    setTone(KEY_TONE_HZ);
  }
  // pressActive skips the release of the press that started the game.
  if (releasedEdge && pressActive) {
    pressActive = false;
    setTone(0);
    if (typedLen < MAX_TYPED) typed[typedLen++] = now - pressStartedAt >= DASH_MS ? '-' : '.';
    lastReleaseAt = now;
  }
  if (!buttonDown && typedLen > 0 && now - lastReleaseAt >= LETTER_GAP_MS) commitLetter(now);
}

void drawCentered(const char* text, int16_t y, uint8_t size) {
  display.setTextSize(size);
  display.setCursor((OLED_WIDTH - static_cast<int16_t>(strlen(text)) * 6 * size) / 2, y);
  display.print(text);
}

int16_t patternWidth(const char* pattern) {
  int16_t width = -4;
  for (; *pattern; pattern++) width += (*pattern == '-' ? 12 : 4) + 4;
  return width;
}

void drawPattern(const char* pattern, int16_t x, int16_t y, bool filled) {
  for (; *pattern; pattern++) {
    const int16_t width = *pattern == '-' ? 12 : 4;
    if (filled) {
      display.fillRect(x, y, width, 4, SSD1306_WHITE);
    } else {
      display.drawRect(x, y, width, 4, SSD1306_WHITE);
    }
    x += width + 4;
  }
}

void drawGame(uint32_t now) {
  char text[12];
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.printf("L%u", level);
  for (uint8_t i = 0; i < lives; i++) display.fillCircle(24 + i * 7, 3, 2, SSD1306_WHITE);
  snprintf(text, sizeof(text), "%lu", static_cast<unsigned long>(score));
  display.setCursor(OLED_WIDTH - strlen(text) * 6, 0);
  display.print(text);

  const int16_t length = strlen(target);
  const int16_t wordX = (OLED_WIDTH - (length * 18 - 3)) / 2;
  display.setTextSize(3);
  display.setCursor(wordX, 11);
  display.print(target);
  if (length > 1) display.fillRect(wordX + pos * 18, 35, 15, 2, SSD1306_WHITE);

  // Outlined row is the answer, filled row is what the player keyed.
  const char* answer = MORSE[target[pos] - 'A'];
  const int16_t patternX = (OLED_WIDTH - patternWidth(answer)) / 2;
  if (state == State::Wrong || hintVisible()) drawPattern(answer, patternX, 41, false);

  char shown[MAX_TYPED + 2];
  uint8_t shownLen = typedLen;
  memcpy(shown, typed, typedLen);
  if (pressActive && shownLen < MAX_TYPED) {
    shown[shownLen++] = now - pressStartedAt >= DASH_MS ? '-' : '.';
  }
  shown[shownLen] = '\0';
  drawPattern(shown, patternX, 50, true);

  display.setTextSize(1);
  if (streak >= 2) {
    display.setCursor(0, 56);
    display.printf("x%u", streak);
  }
  if (static_cast<int32_t>(now - pointsShownUntil) < 0) {
    display.setCursor(110, 56);
    display.printf("+%u", lastPoints);
  }
}

void render(uint32_t now) {
  display.clearDisplay();
  if (state == State::Title) {
    drawCentered("MORSE", 2, 2);
    drawCentered("Tap = dot", 24, 1);
    drawCentered("Hold = dash", 34, 1);
    drawCentered("Press to start", 52, 1);
  } else if (state == State::GameOver) {
    char text[20];
    drawCentered("GAME OVER", 2, 2);
    snprintf(text, sizeof(text), "Score %lu", static_cast<unsigned long>(score));
    drawCentered(text, 26, 1);
    snprintf(text, sizeof(text), "Best %lu", static_cast<unsigned long>(bestScore));
    drawCentered(text, 38, 1);
    drawCentered("Press to play", 54, 1);
  } else {
    drawGame(now);
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
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);
  Serial.println("Morse trainer ready");
}

void loop() {
  const uint32_t now = millis();
  updateButton(now);
  updateTone(now);
  const bool timeUp = static_cast<int32_t>(now - deadline) >= 0;

  switch (state) {
    case State::Title:
      if (pressedEdge) newGame(now);
      break;

    case State::Play:
      updatePlay(now);
      break;

    case State::Wrong:
      if (!timeUp) break;
      display.invertDisplay(false);
      if (lives == 0) {
        if (score > bestScore) bestScore = score;
        state = State::GameOver;
        deadline = now + 700;
        Serial.printf("Game over, score %lu\n", static_cast<unsigned long>(score));
      } else {
        state = State::Play;
        pickTarget(now);
      }
      break;

    case State::GameOver:
      if (pressedEdge && timeUp) newGame(now);
      break;
  }

  render(now);
}
