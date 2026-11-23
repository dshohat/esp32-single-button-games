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

static constexpr int TILE = 8;
static constexpr int GROUND_Y = 55;
static constexpr int CUBE_SCREEN_X = 24;
static constexpr uint32_t STEP_US = 4000;
static constexpr float STEP_S = STEP_US / 1000000.0f;
static constexpr float SPEED = 64.0f;
static constexpr float GRAVITY = 640.0f;
static constexpr float JUMP_VELOCITY = 160.0f;
static constexpr float SPIN_SPEED = 2.0f * PI;
static constexpr float STEP_UP_TOLERANCE = 4.0f;
static constexpr float SPIKE_HIT_HEIGHT = 5.0f;
static constexpr uint16_t CRASH_MS = 700;
static constexpr uint16_t NOTE_GAP_MS = 12;

// One character per 8 px column: '.' floor, '^' spike, '1'/'2' block stack,
// 'A'/'B' block stack of 1/2 with a spike on top.
static const char LEVEL[] =
    "................"
    "^.......^.......^^......"
    "1.......11......^...1......."
    "^^....^^....^......."
    "111...111......."
    "111222......^......."
    "111^^^111......."
    "^^..^^......A......."
    "1^^.......2222......"
    "^..^^.....11A11....."
    "111.222.111....."
    "^^....^^....^^......"
    "................";
static constexpr int LEVEL_COLS = sizeof(LEVEL) - 1;
static constexpr float LEVEL_END_X = LEVEL_COLS * TILE;

enum : uint16_t {
  NOTE_B4 = 494,
  NOTE_C5 = 523,
  NOTE_D5 = 587,
  NOTE_E5 = 659,
  NOTE_F5 = 698,
  NOTE_G5 = 784,
  NOTE_A5 = 880,
  NOTE_B5 = 988,
  NOTE_C6 = 1047,
  NOTE_D6 = 1175,
};

// Note length is in units of the track's unitMs.
struct Note {
  uint16_t freq;
  uint8_t units;
};
struct Track {
  const Note* notes;
  size_t count;
  uint16_t unitMs;
  bool loops;
};

// Eight bars of sixteen sixteenth-notes each.
static const Note MUSIC[] = {
    {NOTE_E5, 2}, {NOTE_E5, 1}, {NOTE_A5, 3}, {NOTE_G5, 2}, {NOTE_E5, 2}, {NOTE_C5, 2}, {NOTE_D5, 2}, {NOTE_E5, 2},
    {NOTE_F5, 2}, {NOTE_F5, 1}, {NOTE_A5, 3}, {NOTE_G5, 2}, {NOTE_F5, 2}, {NOTE_C5, 2}, {NOTE_D5, 2}, {NOTE_C5, 2},
    {NOTE_E5, 2}, {NOTE_E5, 1}, {NOTE_G5, 3}, {NOTE_C6, 2}, {NOTE_B5, 2}, {NOTE_G5, 2}, {NOTE_E5, 2}, {NOTE_G5, 2},
    {NOTE_D5, 2}, {NOTE_D5, 1}, {NOTE_G5, 3}, {NOTE_B5, 2}, {NOTE_A5, 2}, {NOTE_G5, 2}, {NOTE_D5, 2}, {NOTE_B4, 2},
    {NOTE_A5, 3}, {NOTE_E5, 3}, {NOTE_A5, 2}, {NOTE_C6, 3}, {NOTE_B5, 3}, {NOTE_A5, 2},
    {NOTE_A5, 3}, {NOTE_F5, 3}, {NOTE_A5, 2}, {NOTE_C6, 3}, {NOTE_A5, 3}, {NOTE_F5, 2},
    {NOTE_G5, 3}, {NOTE_E5, 3}, {NOTE_G5, 2}, {NOTE_C6, 3}, {NOTE_G5, 3}, {NOTE_E5, 2},
    {NOTE_B5, 3}, {NOTE_G5, 3}, {NOTE_D5, 2}, {NOTE_G5, 2}, {NOTE_A5, 2}, {NOTE_B5, 2}, {NOTE_D6, 2},
};
static const Note SFX_CRASH[] = {{400, 3}, {300, 3}, {200, 4}, {120, 8}};
static const Note SFX_COMPLETE[] = {{523, 5}, {659, 5}, {784, 5}, {1047, 5}, {1319, 5}, {1568, 15}};

enum class Sound : uint8_t { Silence, Music, Crash, Complete };
static const Track TRACKS[] = {
    {nullptr, 0, 0, false},
    {MUSIC, sizeof(MUSIC) / sizeof(MUSIC[0]), 110, true},
    {SFX_CRASH, sizeof(SFX_CRASH) / sizeof(SFX_CRASH[0]), 20, false},
    {SFX_COMPLETE, sizeof(SFX_COMPLETE) / sizeof(SFX_COMPLETE[0]), 20, false},
};

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
enum class GameState { Title, Playing, Crashed, Complete };
GameState gameState = GameState::Title;

uint32_t stepCount = 0;
float cubeX = 0;
float cubeY = 0;
float velY = 0;
float angle = 0;
bool onGround = true;
uint16_t attempt = 1;
uint32_t stateStartedAt = 0;
uint32_t lastMicros = 0;
uint32_t stepBudgetUs = 0;

bool rawButtonState = HIGH;
bool stableButtonState = HIGH;
uint32_t buttonChangedAt = 0;

TaskHandle_t audioTask = nullptr;
volatile Sound requestedSound = Sound::Silence;

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

void playSound(Sound sound) {
  requestedSound = sound;
  xTaskNotifyGive(audioTask);
}

// Sound runs in its own task so note timing does not depend on the display frame rate.
void audioTaskMain(void*) {
  for (;;) {
    const Track& track = TRACKS[static_cast<uint8_t>(requestedSound)];
    bool interrupted = false;
    size_t i = 0;
    while (i < track.count) {
      const uint32_t noteMs = track.notes[i].units * track.unitMs;
      writeTone(track.notes[i].freq);
      interrupted = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(noteMs - NOTE_GAP_MS)) != 0;
      if (interrupted) break;
      writeTone(0);
      interrupted = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(NOTE_GAP_MS)) != 0;
      if (interrupted) break;
      if (++i == track.count && track.loops) i = 0;
    }
    writeTone(0);
    if (!interrupted) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
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

char tileAt(int col) {
  return (col < 0 || col >= LEVEL_COLS) ? '.' : LEVEL[col];
}

int tileHeight(char tile) {
  switch (tile) {
    case '1':
    case 'A':
      return 1;
    case '2':
    case 'B':
      return 2;
    default:
      return 0;
  }
}

bool tileHasSpike(char tile) {
  return tile == '^' || tile == 'A' || tile == 'B';
}

void startAttempt(uint32_t now) {
  stepCount = 0;
  cubeX = 0;
  cubeY = 0;
  velY = 0;
  angle = 0;
  onGround = true;
  stepBudgetUs = 0;
  lastMicros = micros();
  stateStartedAt = now;
  gameState = GameState::Playing;
  playSound(Sound::Music);
}

// Advances the cube one fixed step; returns false if it crashed.
bool stepPhysics(bool held) {
  stepCount++;
  cubeX = stepCount * (SPEED * STEP_S);
  if (onGround && held) velY = JUMP_VELOCITY;
  velY -= GRAVITY * STEP_S;
  cubeY += velY * STEP_S;

  const float left = cubeX + 1;
  const float right = cubeX + TILE - 1;
  const int firstCol = static_cast<int>(left / TILE);
  const int lastCol = static_cast<int>(right / TILE);

  float ground = 0;
  for (int col = firstCol; col <= lastCol; col++) {
    ground = max(ground, static_cast<float>(tileHeight(tileAt(col)) * TILE));
  }
  if (ground - cubeY > STEP_UP_TOLERANCE) return false;
  if (cubeY <= ground) {
    cubeY = ground;
    velY = 0;
    angle = 0;
    onGround = true;
  } else {
    angle += SPIN_SPEED * STEP_S;
    onGround = false;
  }

  for (int col = firstCol; col <= lastCol; col++) {
    const char tile = tileAt(col);
    if (!tileHasSpike(tile)) continue;
    const float spikeX = col * TILE;
    const float spikeBase = tileHeight(tile) * TILE;
    if (right > spikeX + 2 && left < spikeX + 6 && cubeY < spikeBase + SPIKE_HIT_HEIGHT) return false;
  }
  return true;
}

void updatePlaying(uint32_t now, bool held) {
  const uint32_t nowMicros = micros();
  stepBudgetUs += nowMicros - lastMicros;
  lastMicros = nowMicros;
  if (stepBudgetUs > 50000) stepBudgetUs = 50000;

  while (stepBudgetUs >= STEP_US) {
    stepBudgetUs -= STEP_US;
    if (!stepPhysics(held)) {
      gameState = GameState::Crashed;
      stateStartedAt = now;
      playSound(Sound::Crash);
      Serial.printf("Attempt %u crashed at %d%%\n", attempt, static_cast<int>(100 * cubeX / LEVEL_END_X));
      return;
    }
    if (cubeX >= LEVEL_END_X) {
      gameState = GameState::Complete;
      stateStartedAt = now;
      playSound(Sound::Complete);
      Serial.printf("Level complete on attempt %u\n", attempt);
      return;
    }
  }
}

void drawCube() {
  const int top = GROUND_Y - static_cast<int>(lroundf(cubeY)) - TILE;
  if (onGround) {
    display.fillRect(CUBE_SCREEN_X, top, TILE, TILE, SSD1306_WHITE);
    display.drawRect(CUBE_SCREEN_X + 2, top + 2, 4, 4, SSD1306_BLACK);
    return;
  }
  int cornerX[4];
  int cornerY[4];
  for (int i = 0; i < 4; i++) {
    const float cornerAngle = angle + PI / 4 + i * PI / 2;
    cornerX[i] = lroundf(CUBE_SCREEN_X + 3.5f + 5.0f * cosf(cornerAngle));
    cornerY[i] = lroundf(top + 3.5f + 5.0f * sinf(cornerAngle));
  }
  display.fillTriangle(cornerX[0], cornerY[0], cornerX[1], cornerY[1], cornerX[2], cornerY[2], SSD1306_WHITE);
  display.fillTriangle(cornerX[0], cornerY[0], cornerX[2], cornerY[2], cornerX[3], cornerY[3], SSD1306_WHITE);
  display.fillRect(CUBE_SCREEN_X + 3, top + 3, 2, 2, SSD1306_BLACK);
}

void drawCrash(uint32_t now) {
  const float spread = (now - stateStartedAt) * 0.04f;
  const int cx = CUBE_SCREEN_X + 3;
  const int cy = GROUND_Y - static_cast<int>(lroundf(cubeY)) - 5;
  for (int i = 0; i < 8; i++) {
    const float dir = i * PI / 4;
    display.fillRect(cx + lroundf(spread * cosf(dir)), cy + lroundf(spread * sinf(dir)), 2, 2, SSD1306_WHITE);
  }
}

void drawWorld(uint32_t now) {
  const int camX = static_cast<int>(cubeX) - CUBE_SCREEN_X;

  display.drawFastHLine(0, GROUND_Y, OLED_WIDTH, SSD1306_WHITE);
  for (int x = -(((camX % 16) + 16) % 16); x < OLED_WIDTH; x += 16) {
    display.drawLine(x + 4, GROUND_Y + 2, x, OLED_HEIGHT - 1, SSD1306_WHITE);
  }

  const int firstCol = camX / TILE - 1;
  for (int col = firstCol; col <= firstCol + OLED_WIDTH / TILE + 2; col++) {
    const int x = col * TILE - camX;
    const char tile = tileAt(col);
    const int height = tileHeight(tile);
    for (int level = 1; level <= height; level++) {
      display.drawRect(x, GROUND_Y - level * TILE, TILE, TILE, SSD1306_WHITE);
    }
    if (tileHasSpike(tile)) {
      const int base = GROUND_Y - height * TILE - 1;
      display.fillTriangle(x, base, x + 7, base, x + 3, base - 6, SSD1306_WHITE);
      display.fillTriangle(x, base, x + 7, base, x + 4, base - 6, SSD1306_WHITE);
    }
    if (col == LEVEL_COLS) display.drawFastVLine(x, 12, GROUND_Y - 12, SSD1306_WHITE);
  }

  if (gameState == GameState::Crashed) drawCrash(now); else drawCube();

  const int percent = min(100, static_cast<int>(100 * cubeX / LEVEL_END_X));
  display.fillRect(0, 0, OLED_WIDTH * percent / 100, 2, SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(104, 4);
  display.printf("%3d%%", percent);
  if (gameState == GameState::Playing && now - stateStartedAt < 1500) {
    display.setCursor(40, 16);
    display.printf("ATTEMPT %u", attempt);
  }
}

void drawTitle() {
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(16, 4);
  display.print("GEOMETRY");
  display.setCursor(40, 22);
  display.print("DASH");
  display.setTextSize(1);
  display.setCursor(10, 44);
  display.print("Hold or tap = jump");
  display.setCursor(22, 55);
  display.print("Press to start");
}

void drawComplete() {
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(34, 0);
  display.print("LEVEL");
  display.setCursor(10, 18);
  display.print("COMPLETE!");
  display.setTextSize(1);
  display.setCursor(28, 40);
  display.printf("Attempts: %u", attempt);
  display.setCursor(7, 55);
  display.print("Press to play again");
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  initBuzzer();
  xTaskCreate(audioTaskMain, "audio", 3072, nullptr, 2, &audioTask);
  Wire.begin(PIN_SDA, PIN_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C) &&
      !display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
    Serial.println("OLED not found at 0x3C or 0x3D");
    while (true) delay(1000);
  }
  Serial.println("Geometry Dash ready");
}

void loop() {
  const uint32_t now = millis();
  const bool pressed = wasPressed();
  const bool held = digitalRead(PIN_BUTTON) == LOW;

  switch (gameState) {
    case GameState::Title:
      if (pressed) startAttempt(now);
      break;

    case GameState::Playing:
      updatePlaying(now, held);
      break;

    case GameState::Crashed:
      if (now - stateStartedAt >= CRASH_MS) {
        attempt++;
        startAttempt(now);
      }
      break;

    case GameState::Complete:
      if (pressed && now - stateStartedAt >= 1000) {
        attempt = 1;
        startAttempt(now);
      }
      break;
  }

  display.clearDisplay();
  switch (gameState) {
    case GameState::Title:
      drawTitle();
      break;
    case GameState::Playing:
    case GameState::Crashed:
      drawWorld(now);
      break;
    case GameState::Complete:
      drawComplete();
      break;
  }
  display.display();
}
