#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <Wire.h>

static constexpr uint8_t PIN_BUTTON = 4;
static constexpr uint8_t PIN_BUZZER = 23;
static constexpr uint8_t PIN_SDA = 21;
static constexpr uint8_t PIN_SCL = 22;
static constexpr uint8_t OLED_WIDTH = 128;
static constexpr uint8_t OLED_HEIGHT = 64;

static constexpr int COLS = 7;
static constexpr int ROWS = 6;
static constexpr int CELL = 9;
static constexpr int PIECE_RADIUS = 3;
static constexpr int PANEL_X = 70;
static constexpr int HOVER_ROW = -1;
static constexpr uint8_t HOLLOW = 1;
static constexpr uint8_t FILLED = 2;
static constexpr uint16_t TURN_MS = 5000;
static constexpr uint16_t MOVE_INTERVAL_MS = 350;
static constexpr uint16_t DROP_STEP_MS = 50;
static constexpr uint16_t FRAME_MS = 50;
static constexpr uint16_t BLINK_MS = 300;

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
enum class GameState { Ready, Aiming, Dropping, Over };
GameState gameState = GameState::Ready;

uint8_t board[ROWS][COLS];
uint8_t player = HOLLOW;
uint8_t winner = 0;
uint8_t movesPlayed = 0;
int cursorCol = COLS / 2;
int dropRow = HOVER_ROW;
int dropTarget = 0;
int winCells[4][2];
uint32_t turnStartedAt = 0;
uint32_t lastMoveAt = 0;
uint32_t lastDropStepAt = 0;
uint32_t lastFrameAt = 0;
uint32_t overAt = 0;

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

void drawPiece(int cx, int cy, int radius, uint8_t owner) {
  if (owner == FILLED) {
    display.fillCircle(cx, cy, radius, SSD1306_WHITE);
  } else {
    display.drawCircle(cx, cy, radius, SSD1306_WHITE);
  }
}

int cellX(int col) { return 5 + col * CELL; }
int cellY(int row) { return 4 + (row + 1) * CELL; }

bool isWinCell(int row, int col) {
  for (const auto& cell : winCells) {
    if (cell[0] == row && cell[1] == col) return true;
  }
  return false;
}

void drawGame(uint32_t now) {
  const bool over = gameState == GameState::Over;
  const bool hideWin = over && winner != 0 && ((now - overAt) / BLINK_MS) % 2 == 1;

  display.clearDisplay();
  display.drawRect(0, CELL, COLS * CELL + 2, ROWS * CELL + 1, SSD1306_WHITE);
  for (int row = 0; row < ROWS; row++) {
    for (int col = 0; col < COLS; col++) {
      if (board[row][col] == 0) {
        display.drawPixel(cellX(col), cellY(row), SSD1306_WHITE);
      } else if (!(hideWin && isWinCell(row, col))) {
        drawPiece(cellX(col), cellY(row), PIECE_RADIUS, board[row][col]);
      }
    }
  }
  if (gameState == GameState::Aiming) drawPiece(cellX(cursorCol), cellY(HOVER_ROW), PIECE_RADIUS, player);
  if (gameState == GameState::Dropping) drawPiece(cellX(cursorCol), cellY(dropRow), PIECE_RADIUS, player);

  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(PANEL_X, 0);
  if (!over) {
    display.print("TURN");
    drawPiece(PANEL_X + 28, 26, 9, player);
    uint32_t elapsed = gameState == GameState::Aiming ? now - turnStartedAt : TURN_MS;
    if (elapsed > TURN_MS) elapsed = TURN_MS;
    display.drawRect(PANEL_X, 44, 58, 7, SSD1306_WHITE);
    display.fillRect(PANEL_X, 44, 58 * (TURN_MS - elapsed) / TURN_MS, 7, SSD1306_WHITE);
  } else {
    if (winner != 0) {
      display.print("WINNER");
      drawPiece(PANEL_X + 28, 26, 9, winner);
    } else {
      display.setTextSize(2);
      display.setCursor(PANEL_X, 18);
      display.print("DRAW");
      display.setTextSize(1);
    }
    display.setCursor(PANEL_X, 56);
    display.print("Press btn");
  }
  display.display();
}

void startTurn(uint32_t now) {
  cursorCol = COLS / 2;
  turnStartedAt = now;
  lastMoveAt = now;
  gameState = GameState::Aiming;
}

void startGame(uint32_t now) {
  memset(board, 0, sizeof(board));
  player = HOLLOW;
  winner = 0;
  movesPlayed = 0;
  startTurn(now);
}

bool inBoard(int row, int col) {
  return row >= 0 && row < ROWS && col >= 0 && col < COLS;
}

bool ownedByPlayer(int row, int col) {
  return inBoard(row, col) && board[row][col] == player;
}

bool checkWin(int row, int col) {
  static const int dirs[4][2] = {{0, 1}, {1, 0}, {1, 1}, {1, -1}};
  for (const auto& dir : dirs) {
    int r = row;
    int c = col;
    while (ownedByPlayer(r - dir[0], c - dir[1])) {
      r -= dir[0];
      c -= dir[1];
    }
    int count = 0;
    while (ownedByPlayer(r + count * dir[0], c + count * dir[1])) count++;
    if (count >= 4) {
      for (int i = 0; i < 4; i++) {
        winCells[i][0] = r + i * dir[0];
        winCells[i][1] = c + i * dir[1];
      }
      return true;
    }
  }
  return false;
}

void endGame(uint32_t now, uint8_t result) {
  winner = result;
  overAt = now;
  gameState = GameState::Over;
  startTone(winner != 0 ? 1600 : 300, 350);
  Serial.println(winner == HOLLOW ? "Hollow circle wins" : winner == FILLED ? "Filled circle wins" : "Draw");
}

void landPiece(uint32_t now) {
  board[dropTarget][cursorCol] = player;
  movesPlayed++;
  if (checkWin(dropTarget, cursorCol)) {
    endGame(now, player);
  } else if (movesPlayed == ROWS * COLS) {
    endGame(now, 0);
  } else {
    startTone(player == HOLLOW ? 700 : 1000, 40);
    player = player == HOLLOW ? FILLED : HOLLOW;
    startTurn(now);
  }
}

void updateAiming(uint32_t now) {
  if (now - lastMoveAt >= MOVE_INTERVAL_MS) {
    lastMoveAt = now;
    const int target = cursorCol + (stableButtonState == LOW ? 1 : -1);
    if (target >= 0 && target < COLS) cursorCol = target;
  }

  // On timeout the piece drops, but over a full column it waits for a free one.
  if (now - turnStartedAt >= TURN_MS && board[0][cursorCol] == 0) {
    dropTarget = ROWS - 1;
    while (board[dropTarget][cursorCol] != 0) dropTarget--;
    dropRow = HOVER_ROW;
    lastDropStepAt = now;
    gameState = GameState::Dropping;
  }
}

void updateDropping(uint32_t now) {
  if (now - lastDropStepAt < DROP_STEP_MS) return;
  lastDropStepAt = now;
  dropRow++;
  if (dropRow >= dropTarget) landPiece(now);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  initBuzzer();
  Wire.begin(PIN_SDA, PIN_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C) &&
      !display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
    Serial.println("OLED not found at 0x3C or 0x3D");
    while (true) delay(1000);
  }
  Serial.println("Four in a Row ready");
  showMessage("4 IN A ROW", "Hold = right\nRelease = left");
}

void loop() {
  const uint32_t now = millis();
  const bool pressed = wasPressed();
  updateTone(now);

  switch (gameState) {
    case GameState::Ready:
      if (pressed) startGame(now);
      break;

    case GameState::Aiming:
      updateAiming(now);
      break;

    case GameState::Dropping:
      updateDropping(now);
      break;

    case GameState::Over:
      if (pressed && now - overAt >= 1000) startGame(now);
      break;
  }

  if (gameState != GameState::Ready && now - lastFrameAt >= FRAME_MS) {
    lastFrameAt = now;
    drawGame(now);
  }

  delay(5);
}
