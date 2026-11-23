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

static constexpr int16_t GROUND_Y = 60;
static constexpr int16_t BOW_X = 19;
static constexpr int16_t BOW_Y = 45;
static constexpr float GRAVITY = 150.0f;
// Arc peaks near the top of the screen and comes down at the right edge.
static constexpr float ARROW_VX = 75.0f;
static constexpr float ARROW_VY = -105.0f;
static constexpr uint16_t RELOAD_MS = 400;
static constexpr uint8_t MAX_ARROWS = 3;
static constexpr uint8_t MAX_BIRDS = 4;
static constexpr uint8_t START_LIVES = 3;
static constexpr uint8_t HIT_POINTS = 10;
static constexpr uint8_t MAX_STREAK_MULTIPLIER = 5;

enum class State { Title, Play, GameOver };
struct Arrow {
  bool active;
  float x, y, vx, vy;
};
struct Bird {
  bool active;
  bool falling;
  float x, y, vx, vy;
};

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
State state = State::Title;
Arrow arrows[MAX_ARROWS];
Bird birds[MAX_BIRDS];

bool buttonDown = false;
bool pressedEdge = false;
uint32_t buttonChangedAt = 0;

bool toneTimed = false;
uint32_t toneOffAt = 0;

uint8_t lives = START_LIVES;
uint8_t streak = 0;
uint16_t hits = 0;
uint32_t score = 0;
uint32_t bestScore = 0;

uint32_t lastShotAt = 0;
uint32_t nextSpawnAt = 0;
uint32_t deadline = 0;
uint32_t lastFrameAt = 0;

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
  const bool down = digitalRead(PIN_BUTTON) == LOW;
  // React on the first edge, then ignore bounce for 30 ms.
  if (down != buttonDown && now - buttonChangedAt >= 30) {
    buttonDown = down;
    buttonChangedAt = now;
    pressedEdge = down;
  }
}

float difficulty() {
  const float scale = 1.0f + hits * 0.04f;
  return scale > 2.2f ? 2.2f : scale;
}

void newGame(uint32_t now) {
  memset(arrows, 0, sizeof(arrows));
  memset(birds, 0, sizeof(birds));
  lives = START_LIVES;
  streak = 0;
  hits = 0;
  score = 0;
  nextSpawnAt = now + 1000;
  state = State::Play;
}

void shoot(uint32_t now) {
  if (now - lastShotAt < RELOAD_MS) return;
  for (Arrow& arrow : arrows) {
    if (arrow.active) continue;
    arrow = {true, BOW_X, BOW_Y, ARROW_VX, ARROW_VY};
    lastShotAt = now;
    playTone(900, 30);
    return;
  }
}

void spawnBird(uint32_t now) {
  for (Bird& bird : birds) {
    if (bird.active) continue;
    bird = {true, false, OLED_WIDTH + 5.0f, static_cast<float>(random(12, 41)),
            -static_cast<float>(random(28, 46)) * difficulty(), 0.0f};
    break;
  }
  nextSpawnAt = now + static_cast<uint32_t>(random(1500, 3500) / difficulty());
}

void updateBirds(uint32_t now, float dt) {
  for (Bird& bird : birds) {
    if (!bird.active) continue;
    bird.x += bird.vx * dt;
    if (bird.falling) {
      bird.vy += GRAVITY * dt;
      bird.y += bird.vy * dt;
      if (bird.y >= GROUND_Y - 2) bird.active = false;
    } else if (bird.x < -6.0f) {
      bird.active = false;
      streak = 0;
      lives--;
      playTone(150, 300);
      if (lives == 0) {
        if (score > bestScore) bestScore = score;
        state = State::GameOver;
        deadline = now + 700;
        Serial.printf("Game over, score %lu\n", static_cast<unsigned long>(score));
        return;
      }
    }
  }
}

void updateArrows(float dt) {
  for (Arrow& arrow : arrows) {
    if (!arrow.active) continue;
    arrow.vy += GRAVITY * dt;
    arrow.x += arrow.vx * dt;
    arrow.y += arrow.vy * dt;

    if (arrow.x > OLED_WIDTH + 6 || arrow.y >= GROUND_Y) {
      arrow.active = false;
      streak = 0;
      continue;
    }
    for (Bird& bird : birds) {
      if (!bird.active || bird.falling) continue;
      if (fabsf(arrow.x - bird.x) > 5.0f || fabsf(arrow.y - bird.y) > 4.0f) continue;
      bird.falling = true;
      bird.vx *= 0.3f;
      arrow.active = false;
      hits++;
      if (streak < 255) streak++;
      // Consecutive hits without a miss multiply the points, up to x5.
      score += HIT_POINTS * (streak > MAX_STREAK_MULTIPLIER ? MAX_STREAK_MULTIPLIER : streak);
      playTone(1600 + 100 * (streak > 5 ? 5 : streak), 70);
      break;
    }
  }
}

void drawCentered(const char* text, int16_t y, uint8_t size) {
  display.setTextSize(size);
  display.setCursor((OLED_WIDTH - static_cast<int16_t>(strlen(text)) * 6 * size) / 2, y);
  display.print(text);
}

void drawArcher() {
  display.drawCircle(12, 43, 2, SSD1306_WHITE);
  display.drawFastVLine(12, 46, 8, SSD1306_WHITE);
  display.drawLine(12, 54, 9, 59, SSD1306_WHITE);
  display.drawLine(12, 54, 15, 59, SSD1306_WHITE);
  display.drawLine(12, 48, 17, 46, SSD1306_WHITE);
  display.drawLine(16, 40, 20, 45, SSD1306_WHITE);
  display.drawLine(20, 45, 19, 51, SSD1306_WHITE);
  display.drawLine(16, 40, 19, 51, SSD1306_WHITE);
}

void drawBird(const Bird& bird, bool wingsUp) {
  const int16_t x = lroundf(bird.x);
  const int16_t y = lroundf(bird.y);
  if (bird.falling) {
    display.drawLine(x - 2, y - 2, x + 2, y + 2, SSD1306_WHITE);
    display.drawLine(x - 2, y + 2, x + 2, y - 2, SSD1306_WHITE);
    return;
  }
  const int16_t wingY = wingsUp ? y - 3 : y + 2;
  display.drawLine(x - 4, wingY, x, y, SSD1306_WHITE);
  display.drawLine(x + 4, wingY, x, y, SSD1306_WHITE);
  display.drawFastHLine(x - 2, y, 4, SSD1306_WHITE);
}

void drawArrow(const Arrow& arrow) {
  const float speed = sqrtf(arrow.vx * arrow.vx + arrow.vy * arrow.vy);
  const int16_t x = lroundf(arrow.x);
  const int16_t y = lroundf(arrow.y);
  display.drawLine(x, y, x - lroundf(arrow.vx / speed * 6.0f), y - lroundf(arrow.vy / speed * 6.0f),
                   SSD1306_WHITE);
}

void drawGame(uint32_t now) {
  char text[12];
  display.setTextSize(1);
  for (uint8_t i = 0; i < lives; i++) display.fillCircle(3 + i * 7, 3, 2, SSD1306_WHITE);
  if (streak >= 2) {
    display.setCursor(56, 0);
    display.printf("x%u", streak > MAX_STREAK_MULTIPLIER ? MAX_STREAK_MULTIPLIER : streak);
  }
  snprintf(text, sizeof(text), "%lu", static_cast<unsigned long>(score));
  display.setCursor(OLED_WIDTH - strlen(text) * 6, 0);
  display.print(text);

  display.drawFastHLine(0, GROUND_Y, OLED_WIDTH, SSD1306_WHITE);
  drawArcher();
  for (uint8_t i = 0; i < MAX_BIRDS; i++) {
    if (birds[i].active) drawBird(birds[i], (now / 150 + i) % 2 == 0);
  }
  for (const Arrow& arrow : arrows) {
    if (arrow.active) drawArrow(arrow);
  }
}

void render(uint32_t now) {
  display.clearDisplay();
  if (state == State::Title) {
    drawCentered("BIRD HUNT", 10, 2);
    drawCentered("Press to shoot", 44, 1);
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
  Serial.println("Bird hunt ready");
  lastFrameAt = millis();
}

void loop() {
  const uint32_t now = millis();
  float dt = (now - lastFrameAt) / 1000.0f;
  if (dt > 0.05f) dt = 0.05f;
  lastFrameAt = now;

  updateButton(now);
  updateTone(now);

  switch (state) {
    case State::Title:
      if (pressedEdge) newGame(now);
      break;

    case State::Play:
      if (pressedEdge) shoot(now);
      if (static_cast<int32_t>(now - nextSpawnAt) >= 0) spawnBird(now);
      updateArrows(dt);
      updateBirds(now, dt);
      break;

    case State::GameOver:
      if (pressedEdge && static_cast<int32_t>(now - deadline) >= 0) newGame(now);
      break;
  }

  render(now);
}
