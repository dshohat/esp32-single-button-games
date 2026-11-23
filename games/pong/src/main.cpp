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

static constexpr uint32_t STEP_US = 4000;
static constexpr float STEP_S = STEP_US / 1000000.0f;
static constexpr float PADDLE_W = 3;
static constexpr float PADDLE_H = 14;
static constexpr float BALL_SIZE = 3;
static constexpr float PLAYER_X = 3;
static constexpr float CPU_X = OLED_WIDTH - 3 - PADDLE_W;
static constexpr float PLAYER_SPEED = 70;
static constexpr float CPU_SPEED = 45;
static constexpr float BALL_START_SPEED = 55;
static constexpr float BALL_MAX_SPEED = 120;
static constexpr float BALL_SPEEDUP = 1.06f;
static constexpr float MAX_BOUNCE_ANGLE = 55 * DEG_TO_RAD;
static constexpr uint8_t WIN_SCORE = 5;
static constexpr uint16_t SERVE_PAUSE_MS = 800;

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
enum class GameState { Title, Serve, Playing, GameOver };
GameState gameState = GameState::Title;

float playerY = 0;
float cpuY = 0;
float cpuAimOffset = 0;
float ballX = 0;
float ballY = 0;
float ballVX = 0;
float ballVY = 0;
float ballSpeed = 0;
uint8_t playerScore = 0;
uint8_t cpuScore = 0;
uint32_t stateStartedAt = 0;
uint32_t lastMicros = 0;
uint32_t stepBudgetUs = 0;

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

// direction: +1 serves toward the computer, -1 toward the player.
void serve(int direction, uint32_t now) {
  const float angle = random(-30, 31) * DEG_TO_RAD;
  ballSpeed = BALL_START_SPEED;
  ballX = (OLED_WIDTH - BALL_SIZE) / 2;
  ballY = random(10, OLED_HEIGHT - 10);
  ballVX = direction * ballSpeed * cosf(angle);
  ballVY = ballSpeed * sinf(angle);
  cpuAimOffset = random(-5, 6);
  stateStartedAt = now;
  gameState = GameState::Serve;
}

void startGame(uint32_t now) {
  playerScore = 0;
  cpuScore = 0;
  playerY = (OLED_HEIGHT - PADDLE_H) / 2;
  cpuY = playerY;
  stepBudgetUs = 0;
  lastMicros = micros();
  serve(1, now);
}

void bounceOffPaddle(float paddleY, int direction) {
  float hit = (ballY + BALL_SIZE / 2 - (paddleY + PADDLE_H / 2)) / ((PADDLE_H + BALL_SIZE) / 2);
  hit = constrain(hit, -1.0f, 1.0f);
  ballSpeed = min(ballSpeed * BALL_SPEEDUP, BALL_MAX_SPEED);
  ballVX = direction * ballSpeed * cosf(hit * MAX_BOUNCE_ANGLE);
  ballVY = ballSpeed * sinf(hit * MAX_BOUNCE_ANGLE);
  cpuAimOffset = random(-5, 6);
  startTone(900, 25);
}

void scorePoint(bool playerScored, uint32_t now) {
  if (playerScored) playerScore++; else cpuScore++;
  Serial.printf("Score: player %u, computer %u\n", playerScore, cpuScore);

  if (playerScore >= WIN_SCORE || cpuScore >= WIN_SCORE) {
    gameState = GameState::GameOver;
    stateStartedAt = now;
    startTone(playerScored ? 1500 : 200, 500);
  } else {
    startTone(playerScored ? 1200 : 250, 120);
    serve(playerScored ? -1 : 1, now);
  }
}

void stepPhysics(bool held, uint32_t now) {
  playerY += (held ? -PLAYER_SPEED : PLAYER_SPEED) * STEP_S;
  playerY = constrain(playerY, 0.0f, OLED_HEIGHT - PADDLE_H);

  // The computer chases the ball only while it approaches, at a capped speed, so it can be beaten.
  const float cpuTarget = (ballVX > 0 ? ballY + BALL_SIZE / 2 + cpuAimOffset : OLED_HEIGHT / 2.0f) - PADDLE_H / 2;
  cpuY += constrain(cpuTarget - cpuY, -CPU_SPEED * STEP_S, CPU_SPEED * STEP_S);
  cpuY = constrain(cpuY, 0.0f, OLED_HEIGHT - PADDLE_H);

  if (gameState != GameState::Playing) return;
  ballX += ballVX * STEP_S;
  ballY += ballVY * STEP_S;

  if (ballY < 0) {
    ballY = 0;
    ballVY = -ballVY;
    startTone(500, 15);
  } else if (ballY > OLED_HEIGHT - BALL_SIZE) {
    ballY = OLED_HEIGHT - BALL_SIZE;
    ballVY = -ballVY;
    startTone(500, 15);
  }

  if (ballVX < 0 && ballX <= PLAYER_X + PADDLE_W && ballX + BALL_SIZE >= PLAYER_X &&
      ballY + BALL_SIZE > playerY && ballY < playerY + PADDLE_H) {
    ballX = PLAYER_X + PADDLE_W;
    bounceOffPaddle(playerY, 1);
  } else if (ballVX > 0 && ballX + BALL_SIZE >= CPU_X && ballX <= CPU_X + PADDLE_W &&
             ballY + BALL_SIZE > cpuY && ballY < cpuY + PADDLE_H) {
    ballX = CPU_X - BALL_SIZE;
    bounceOffPaddle(cpuY, -1);
  }

  if (ballX + BALL_SIZE < 0) {
    scorePoint(false, now);
  } else if (ballX > OLED_WIDTH) {
    scorePoint(true, now);
  }
}

void updateCourt(uint32_t now, bool held) {
  const uint32_t nowMicros = micros();
  stepBudgetUs += nowMicros - lastMicros;
  lastMicros = nowMicros;
  if (stepBudgetUs > 50000) stepBudgetUs = 50000;

  while (stepBudgetUs >= STEP_US && gameState != GameState::GameOver) {
    stepBudgetUs -= STEP_US;
    stepPhysics(held, now);
  }
}

void drawCourt() {
  for (int y = 0; y < OLED_HEIGHT; y += 6) {
    display.drawFastVLine(OLED_WIDTH / 2 - 1, y, 3, SSD1306_WHITE);
  }
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(40, 2);
  display.print(playerScore);
  display.setCursor(78, 2);
  display.print(cpuScore);

  display.fillRect(PLAYER_X, lroundf(playerY), PADDLE_W, PADDLE_H, SSD1306_WHITE);
  display.fillRect(CPU_X, lroundf(cpuY), PADDLE_W, PADDLE_H, SSD1306_WHITE);
  display.fillRect(lroundf(ballX), lroundf(ballY), BALL_SIZE, BALL_SIZE, SSD1306_WHITE);
}

void drawTitle() {
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(3);
  display.setCursor(28, 0);
  display.print("PONG");
  display.setTextSize(1);
  display.setCursor(13, 30);
  display.print("Hold = paddle up");
  display.setCursor(1, 41);
  display.print("Release = paddle down");
  display.setCursor(22, 55);
  display.print("Press to start");
}

void drawGameOver() {
  const bool playerWon = playerScore > cpuScore;
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(playerWon ? 16 : 10, 4);
  display.print(playerWon ? "YOU WIN!" : "YOU LOSE!");
  display.setCursor(46, 26);
  display.printf("%u-%u", playerScore, cpuScore);
  display.setTextSize(1);
  display.setCursor(7, 55);
  display.print("Press to play again");
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
  Serial.println("Pong ready");
}

void loop() {
  const uint32_t now = millis();
  const bool pressed = wasPressed();
  const bool held = digitalRead(PIN_BUTTON) == LOW;
  updateTone(now);

  switch (gameState) {
    case GameState::Title:
      if (pressed) startGame(now);
      break;

    case GameState::Serve:
      if (now - stateStartedAt >= SERVE_PAUSE_MS) gameState = GameState::Playing;
      updateCourt(now, held);
      break;

    case GameState::Playing:
      updateCourt(now, held);
      break;

    case GameState::GameOver:
      if (pressed && now - stateStartedAt >= 1000) startGame(now);
      break;
  }

  display.clearDisplay();
  switch (gameState) {
    case GameState::Title:
      drawTitle();
      break;
    case GameState::Serve:
    case GameState::Playing:
      drawCourt();
      break;
    case GameState::GameOver:
      drawGameOver();
      break;
  }
  display.display();
}
