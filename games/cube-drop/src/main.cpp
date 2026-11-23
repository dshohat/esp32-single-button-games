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

static constexpr int8_t COLS = 10;
static constexpr int8_t ROWS = 10;
static constexpr uint8_t CELL = 6;
static constexpr uint8_t FIELD_X = 2;
static constexpr uint8_t FIELD_Y = 2;
static constexpr uint8_t PANEL_X = 70;
static constexpr uint16_t MOVE_INTERVAL_MS = 140;
static constexpr uint16_t FALL_START_MS = 350;
static constexpr uint16_t FALL_MIN_MS = 100;

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
enum class GameState { Ready, Playing, GameOver };
GameState gameState = GameState::Ready;

bool grid[ROWS][COLS];
int8_t cubeCol = 0;
int8_t cubeRow = 0;
uint16_t score = 0;
uint16_t lines = 0;
uint32_t lastFallAt = 0;
uint32_t lastMoveAt = 0;
uint32_t gameOverAt = 0;
bool dirty = true;

bool rawButtonState = HIGH;
bool stableButtonState = HIGH;
uint32_t buttonChangedAt = 0;

bool toneActive = false;
uint32_t toneOffAt = 0;

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

void startTone(uint16_t frequency, uint16_t durationMs) {
  writeTone(frequency);
  toneActive = true;
  toneOffAt = millis() + durationMs;
}

void updateTone(uint32_t now) {
  if (toneActive && static_cast<int32_t>(now - toneOffAt) >= 0) {
    writeTone(0);
    toneActive = false;
  }
}

// Returns true once per debounced press; stableButtonState holds the held state.
bool wasPressed() {
  const bool reading = digitalRead(PIN_BUTTON);
  const uint32_t now = millis();
  bool pressed = false;

  if (reading != rawButtonState) {
    rawButtonState = reading;
    buttonChangedAt = now;
  }
  if (now - buttonChangedAt >= 30 && stableButtonState != rawButtonState) {
    stableButtonState = rawButtonState;
    pressed = stableButtonState == LOW;
  }
  return pressed;
}

void showMessage(const char* title, const char* detail = "") {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 8);
  display.println(title);
  display.setTextSize(1);
  display.setCursor(0, 42);
  display.println(detail);
  display.display();
}

void drawCube(int8_t col, int8_t row) {
  display.fillRect(FIELD_X + col * CELL, FIELD_Y + row * CELL, CELL - 1, CELL - 1, SSD1306_WHITE);
}

void drawGame() {
  display.clearDisplay();
  display.drawRect(0, 0, COLS * CELL + 3, ROWS * CELL + 3, SSD1306_WHITE);
  for (int8_t row = 0; row < ROWS; row++) {
    for (int8_t col = 0; col < COLS; col++) {
      if (grid[row][col]) drawCube(col, row);
    }
  }
  drawCube(cubeCol, cubeRow);

  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(PANEL_X, 2);
  display.print("SCORE");
  display.setCursor(PANEL_X, 34);
  display.print("LINES");
  display.setTextSize(2);
  display.setCursor(PANEL_X, 13);
  display.print(score);
  display.setCursor(PANEL_X, 45);
  display.print(lines);
  display.display();
}

uint16_t fallInterval() {
  const int32_t interval = FALL_START_MS - static_cast<int32_t>(lines) * 20;
  return interval < FALL_MIN_MS ? FALL_MIN_MS : interval;
}

void endGame(uint32_t now) {
  gameState = GameState::GameOver;
  gameOverAt = now;
  char detail[32];
  snprintf(detail, sizeof(detail), "Score %u  Lines %u", score, lines);
  showMessage("GAME OVER", detail);
  startTone(200, 400);
  Serial.printf("Game over: score %u, lines %u\n", score, lines);
}

void spawnCube(uint32_t now) {
  cubeRow = 0;
  cubeCol = random(COLS);
  lastFallAt = now;
  if (grid[cubeRow][cubeCol]) endGame(now);
}

void startGame(uint32_t now) {
  memset(grid, 0, sizeof(grid));
  score = 0;
  lines = 0;
  lastMoveAt = now;
  gameState = GameState::Playing;
  spawnCube(now);
  dirty = true;
}

void lockCube(uint32_t now) {
  grid[cubeRow][cubeCol] = true;

  bool full = true;
  for (int8_t col = 0; col < COLS; col++) {
    if (!grid[cubeRow][col]) full = false;
  }

  if (full) {
    for (int8_t row = cubeRow; row > 0; row--) {
      memcpy(grid[row], grid[row - 1], sizeof(grid[row]));
    }
    memset(grid[0], 0, sizeof(grid[0]));
    lines++;
    score += 10;
    startTone(1600, 120);
  } else {
    score++;
    startTone(500, 25);
  }
  spawnCube(now);
}

void updateGame(uint32_t now) {
  if (now - lastMoveAt >= MOVE_INTERVAL_MS) {
    lastMoveAt = now;
    const int8_t target = cubeCol + (stableButtonState == LOW ? 1 : -1);
    if (target >= 0 && target < COLS && !grid[cubeRow][target]) {
      cubeCol = target;
      dirty = true;
    }
  }

  if (now - lastFallAt >= fallInterval()) {
    lastFallAt = now;
    if (cubeRow + 1 < ROWS && !grid[cubeRow + 1][cubeCol]) {
      cubeRow++;
    } else {
      lockCube(now);
    }
    dirty = true;
  }

  if (dirty && gameState == GameState::Playing) {
    drawGame();
    dirty = false;
  }
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
  Serial.println("Cube Drop ready");
  showMessage("CUBE DROP", "Hold = right\nRelease = left");
}

void loop() {
  const uint32_t now = millis();
  const bool pressed = wasPressed();
  updateTone(now);

  switch (gameState) {
    case GameState::Ready:
      if (pressed) startGame(now);
      break;

    case GameState::Playing:
      updateGame(now);
      break;

    case GameState::GameOver:
      if (pressed && now - gameOverAt >= 800) startGame(now);
      break;
  }

  delay(5);
}
