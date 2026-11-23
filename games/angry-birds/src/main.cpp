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

static constexpr uint8_t COLS = 9;
static constexpr uint8_t ROWS = 6;
static constexpr uint8_t CELL = 6;
static constexpr int16_t GRID_X = OLED_WIDTH - COLS * CELL;
static constexpr int16_t GROUND_Y = 60;
static constexpr int16_t SLING_X = 10;
static constexpr int16_t SLING_Y = 46;

static constexpr float GRAVITY = 130.0f;
static constexpr float MIN_SPEED = 45.0f;
static constexpr float MAX_SPEED = 135.0f;
static constexpr float HIT_SLOWDOWN = 0.65f;
static constexpr float STOP_SPEED = 20.0f;
static constexpr float MIN_ANGLE = 10.0f;
static constexpr float MAX_ANGLE = 80.0f;
static constexpr uint16_t SWEEP_MS = 1200;
static constexpr uint16_t CHARGE_MS = 1500;
static constexpr uint8_t BIRDS_PER_LEVEL = 3;
static constexpr uint8_t BLOCK_POINTS = 10;
static constexpr uint8_t PIG_POINTS = 30;
static constexpr uint8_t TRAIL_MAX = 24;

enum class Cell : uint8_t { Empty, Block, Pig };
enum class State { Title, Aim, PowerWait, Charging, Flight, Result, LevelClear, GameOver };
struct Point {
  int16_t x;
  int16_t y;
};

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
State state = State::Title;
Cell grid[COLS][ROWS];  // row 0 rests on the ground

bool buttonDown = false;
bool pressedEdge = false;
bool releasedEdge = false;
uint32_t buttonChangedAt = 0;

bool toneTimed = false;
uint32_t toneOffAt = 0;

uint8_t level = 1;
uint8_t birdsLeft = 0;
uint8_t pigsLeft = 0;
uint32_t score = 0;
uint32_t bestScore = 0;

float angleDeg = MIN_ANGLE;
float power = 0.0f;
uint32_t aimStartedAt = 0;
uint32_t chargeStartedAt = 0;

float birdX = 0, birdY = 0, birdVx = 0, birdVy = 0;
uint8_t shotHits = 0;
uint16_t shotPoints = 0;
Point trail[TRAIL_MAX];
uint8_t trailCount = 0;
uint32_t lastTrailAt = 0;

uint32_t deadline = 0;
uint32_t lastSettleAt = 0;
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

void buildLevel() {
  memset(grid, 0, sizeof(grid));
  trailCount = 0;

  uint8_t maxHeight = 2 + level;
  if (maxHeight > ROWS - 1) maxHeight = ROWS - 1;
  uint8_t height[COLS];
  for (uint8_t c = 0; c < COLS; c++) {
    height[c] = random(0, maxHeight + 1);
    for (uint8_t r = 0; r < height[c]; r++) grid[c][r] = Cell::Block;
  }

  uint8_t pigs = 1 + level / 2;
  if (pigs > 4) pigs = 4;
  pigsLeft = 0;
  while (pigsLeft < pigs) {
    const uint8_t c = random(COLS);
    if (grid[c][height[c]] == Cell::Pig) continue;
    grid[c][height[c]] = Cell::Pig;
    pigsLeft++;
    if (height[c] + 1 < ROWS && random(3) == 0) grid[c][height[c] + 1] = Cell::Block;
  }
  birdsLeft = BIRDS_PER_LEVEL;
}

void startAim(uint32_t now) {
  state = State::Aim;
  aimStartedAt = now;
  power = 0.0f;
}

void newGame(uint32_t now) {
  score = 0;
  level = 1;
  buildLevel();
  startAim(now);
}

void launch(uint32_t now) {
  const float rad = angleDeg * DEG_TO_RAD;
  const float speed = MIN_SPEED + power * (MAX_SPEED - MIN_SPEED);
  birdX = SLING_X;
  birdY = SLING_Y;
  birdVx = cosf(rad) * speed;
  birdVy = -sinf(rad) * speed;
  shotHits = 0;
  shotPoints = 0;
  trailCount = 0;
  lastTrailAt = now;
  birdsLeft--;
  state = State::Flight;
  playTone(1500, 60);
}

void hitCell(int16_t x, int16_t y) {
  if (x < GRID_X || x >= OLED_WIDTH || y >= GROUND_Y || y < GROUND_Y - ROWS * CELL) return;
  Cell& cell = grid[(x - GRID_X) / CELL][(GROUND_Y - 1 - y) / CELL];
  if (cell == Cell::Empty) return;

  const bool pig = cell == Cell::Pig;
  cell = Cell::Empty;
  shotHits++;
  // Every extra object hit in the same shot is worth more.
  const uint16_t points = (pig ? PIG_POINTS : BLOCK_POINTS) * shotHits;
  shotPoints += points;
  score += points;
  if (pig) pigsLeft--;
  birdVx *= HIT_SLOWDOWN;
  birdVy *= HIT_SLOWDOWN;
  playTone(pig ? 1900 : 500 + 120 * shotHits, pig ? 90 : 35);
}

void settleStep() {
  for (uint8_t c = 0; c < COLS; c++) {
    for (uint8_t r = 1; r < ROWS; r++) {
      if (grid[c][r] != Cell::Empty && grid[c][r - 1] == Cell::Empty) {
        grid[c][r - 1] = grid[c][r];
        grid[c][r] = Cell::Empty;
      }
    }
  }
}

void updateFlight(uint32_t now, float dt) {
  birdVy += GRAVITY * dt;
  birdX += birdVx * dt;
  birdY += birdVy * dt;

  const int16_t x = lroundf(birdX);
  const int16_t y = lroundf(birdY);
  if (trailCount < TRAIL_MAX && now - lastTrailAt >= 60) {
    trail[trailCount++] = {x, y};
    lastTrailAt = now;
  }

  const uint8_t hitsBefore = shotHits;
  for (int8_t dx = -2; dx <= 2; dx += 4) {
    for (int8_t dy = -2; dy <= 2; dy += 4) hitCell(x + dx, y + dy);
  }

  const bool stopped = shotHits != hitsBefore &&
                       birdVx * birdVx + birdVy * birdVy < STOP_SPEED * STOP_SPEED;
  const bool landed = birdY >= GROUND_Y - 2;
  if (landed) playTone(120, 80);
  if (stopped || landed || birdX > OLED_WIDTH + 3 || birdX < -3) {
    state = State::Result;
    deadline = now + 1300;
  }
}

void drawCentered(const char* text, int16_t y, uint8_t size) {
  display.setTextSize(size);
  display.setCursor((OLED_WIDTH - static_cast<int16_t>(strlen(text)) * 6 * size) / 2, y);
  display.print(text);
}

void drawBird(int16_t x, int16_t y) {
  display.fillCircle(x, y, 2, SSD1306_WHITE);
  display.drawPixel(x + 3, y, SSD1306_WHITE);
  display.drawPixel(x + 1, y - 1, SSD1306_BLACK);
}

void drawWorld(uint32_t now) {
  char text[20];
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.printf("L%u", level);
  for (uint8_t i = 0; i < birdsLeft; i++) display.fillCircle(24 + i * 7, 3, 2, SSD1306_WHITE);
  snprintf(text, sizeof(text), "%lu", static_cast<unsigned long>(score));
  display.setCursor(OLED_WIDTH - strlen(text) * 6, 0);
  display.print(text);

  display.drawFastHLine(0, GROUND_Y, OLED_WIDTH, SSD1306_WHITE);
  display.drawFastVLine(SLING_X, SLING_Y + 6, GROUND_Y - SLING_Y - 6, SSD1306_WHITE);
  display.drawLine(SLING_X, SLING_Y + 6, SLING_X - 3, SLING_Y + 2, SSD1306_WHITE);
  display.drawLine(SLING_X, SLING_Y + 6, SLING_X + 3, SLING_Y + 2, SSD1306_WHITE);

  for (uint8_t c = 0; c < COLS; c++) {
    for (uint8_t r = 0; r < ROWS; r++) {
      const int16_t x = GRID_X + c * CELL;
      const int16_t y = GROUND_Y - (r + 1) * CELL;
      if (grid[c][r] == Cell::Block) {
        display.drawRect(x, y, CELL, CELL, SSD1306_WHITE);
      } else if (grid[c][r] == Cell::Pig) {
        display.fillCircle(x + 3, y + 3, 2, SSD1306_WHITE);
        display.drawPixel(x + 2, y + 2, SSD1306_BLACK);
        display.drawPixel(x + 4, y + 2, SSD1306_BLACK);
      }
    }
  }

  for (uint8_t i = 0; i < trailCount; i++) display.drawPixel(trail[i].x, trail[i].y, SSD1306_WHITE);

  switch (state) {
    case State::Aim:
    case State::PowerWait:
    case State::Charging: {
      drawBird(SLING_X, SLING_Y);
      const float rad = angleDeg * DEG_TO_RAD;
      const float length = 12.0f + power * 14.0f;
      for (float d = 5.0f; d <= length; d += 3.0f) {
        display.drawPixel(SLING_X + lroundf(cosf(rad) * d), SLING_Y - lroundf(sinf(rad) * d),
                          SSD1306_WHITE);
      }
      display.setCursor(8, 10);
      display.print(static_cast<int>(lroundf(angleDeg)));
      display.write(0xF8);

      const int16_t fill = lroundf(power * 44.0f);
      display.drawRect(0, 12, 4, 46, SSD1306_WHITE);
      display.fillRect(1, 57 - fill, 2, fill, SSD1306_WHITE);

      display.setCursor(34, 10);
      if (state == State::Aim) {
        display.print("AIM");
      } else if (state == State::Charging) {
        display.print("POWER");
      } else if ((now / 300) % 2 == 0) {
        display.print("HOLD");
      }
      break;
    }

    case State::Flight:
      drawBird(lroundf(birdX), lroundf(birdY));
      break;

    case State::Result:
      display.setCursor(34, 10);
      if (shotHits == 0) {
        display.print("MISS");
      } else {
        display.printf("%u HIT%s +%u", shotHits, shotHits == 1 ? "" : "S", shotPoints);
      }
      break;

    case State::LevelClear:
      drawCentered("LEVEL CLEAR!", 10, 1);
      break;

    default:
      break;
  }
}

void render(uint32_t now) {
  display.clearDisplay();
  if (state == State::Title) {
    drawCentered("ANGRY BIRD", 10, 2);
    drawCentered("Press to start", 44, 1);
  } else if (state == State::GameOver) {
    char text[20];
    drawCentered("GAME OVER", 2, 2);
    snprintf(text, sizeof(text), "Score %lu", static_cast<unsigned long>(score));
    drawCentered(text, 26, 1);
    snprintf(text, sizeof(text), "Best %lu", static_cast<unsigned long>(bestScore));
    drawCentered(text, 38, 1);
    drawCentered("Press to play", 54, 1);
  } else {
    drawWorld(now);
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
  display.cp437(true);
  Serial.println("Angry Bird ready");
  lastFrameAt = millis();
}

void loop() {
  const uint32_t now = millis();
  float dt = (now - lastFrameAt) / 1000.0f;
  if (dt > 0.05f) dt = 0.05f;
  lastFrameAt = now;

  updateButton(now);
  updateTone(now);
  const bool timeUp = static_cast<int32_t>(now - deadline) >= 0;

  if ((state == State::Flight || state == State::Result) && now - lastSettleAt >= 70) {
    settleStep();
    lastSettleAt = now;
  }

  switch (state) {
    case State::Title:
      if (pressedEdge) newGame(now);
      break;

    case State::Aim: {
      const uint32_t phase = (now - aimStartedAt) % (2 * SWEEP_MS);
      const float t = phase < SWEEP_MS ? phase / static_cast<float>(SWEEP_MS)
                                       : 2.0f - phase / static_cast<float>(SWEEP_MS);
      angleDeg = MIN_ANGLE + t * (MAX_ANGLE - MIN_ANGLE);
      if (pressedEdge) {
        state = State::PowerWait;
        playTone(900, 40);
      }
      break;
    }

    case State::PowerWait:
      if (pressedEdge) {
        state = State::Charging;
        chargeStartedAt = now;
      }
      break;

    case State::Charging:
      power = (now - chargeStartedAt) / static_cast<float>(CHARGE_MS);
      if (power > 1.0f) power = 1.0f;
      if (releasedEdge) {
        launch(now);
      } else {
        toneTimed = false;
        setTone(300 + static_cast<uint16_t>(power * 900.0f));
      }
      break;

    case State::Flight:
      updateFlight(now, dt);
      break;

    case State::Result:
      if (!timeUp) break;
      if (pigsLeft == 0) {
        state = State::LevelClear;
        deadline = now + 1500;
        playTone(1800, 300);
      } else if (birdsLeft == 0) {
        if (score > bestScore) bestScore = score;
        state = State::GameOver;
        deadline = now + 700;
        playTone(150, 500);
        Serial.printf("Game over, score %lu\n", static_cast<unsigned long>(score));
      } else {
        startAim(now);
      }
      break;

    case State::LevelClear:
      if (timeUp) {
        level++;
        buildLevel();
        startAim(now);
      }
      break;

    case State::GameOver:
      if (pressedEdge && timeUp) newGame(now);
      break;
  }

  render(now);
}
