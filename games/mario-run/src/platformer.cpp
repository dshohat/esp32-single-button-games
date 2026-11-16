#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <Wire.h>

#include "world.h"

static constexpr uint8_t PIN_BUTTON = 4;
static constexpr uint8_t PIN_BUZZER = 23;
static constexpr uint8_t PIN_SDA = 21;
static constexpr uint8_t PIN_SCL = 22;
static constexpr uint32_t FRAME_MS = 30;

Adafruit_SSD1306 display(128, 64, &Wire, -1);
MarioRun::World world;

enum class GameState { Ready, Playing, Retry, GameOver, Won };
GameState gameState = GameState::Ready;
uint8_t lives = 3;
uint32_t lastFrameAt = 0;
uint32_t stateChangedAt = 0;
uint32_t soundEndsAt = 0;
uint32_t noteEndsAt = 0;
uint8_t noteIndex = 0;
bool introPlaying = false;
bool soundPlaying = false;
bool rawButtonState = HIGH;
bool stableButtonState = HIGH;
bool jumpPending = false;
uint32_t buttonChangedAt = 0;

struct Note {
  uint16_t frequency;
  uint16_t duration;
};

const Note intro[] = {
    {523, 120}, {659, 120}, {784, 160}, {0, 60},
    {659, 120}, {880, 120}, {784, 180}, {0, 60},
    {587, 120}, {698, 120}, {988, 160}, {880, 120},
    {784, 120}, {659, 120}, {523, 240},
};

void setTone(uint16_t frequency) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(PIN_BUZZER, frequency);
#else
  ledcWriteTone(0, frequency);
#endif
}

void playTone(uint16_t frequency, uint16_t duration, uint32_t now) {
  introPlaying = false;
  soundPlaying = true;
  soundEndsAt = now + duration;
  setTone(frequency);
}

void updateSound(uint32_t now) {
  if (introPlaying && static_cast<int32_t>(now - noteEndsAt) >= 0) {
    if (noteIndex < sizeof(intro) / sizeof(intro[0])) {
      const Note &note = intro[noteIndex++];
      setTone(note.frequency);
      noteEndsAt = now + note.duration;
    } else {
      introPlaying = false;
      setTone(0);
    }
  }
  if (soundPlaying && static_cast<int32_t>(now - soundEndsAt) >= 0) {
    soundPlaying = false;
    setTone(0);
  }
}

bool updateButton(uint32_t now) {
  const bool reading = digitalRead(PIN_BUTTON);
  if (reading != rawButtonState) {
    rawButtonState = reading;
    buttonChangedAt = now;
  }
  if (now - buttonChangedAt >= 20 && stableButtonState != rawButtonState) {
    stableButtonState = rawButtonState;
    return stableButtonState == LOW;
  }
  return false;
}

void startLevel(uint32_t now) {
  world = MarioRun::World();
  gameState = GameState::Playing;
  jumpPending = false;
  lastFrameAt = now;
  introPlaying = false;
  soundPlaying = false;
  setTone(0);
  Serial.printf("Level started, lives: %u\n", lives);
}

void drawMario(int x, int y) {
  display.fillRect(x + 2, y, 6, 2, SSD1306_WHITE);
  display.fillRect(x + 1, y + 2, 8, 1, SSD1306_WHITE);
  display.fillRect(x + 2, y + 3, 5, 3, SSD1306_WHITE);
  display.drawPixel(x + 5, y + 3, SSD1306_BLACK);
  display.fillRect(x + 1, y + 6, 7, 3, SSD1306_WHITE);
  display.drawPixel(x + 3, y + 7, SSD1306_BLACK);
  display.drawPixel(x + 6, y + 7, SSD1306_BLACK);
  const bool running = world.grounded && static_cast<int>(world.x / 5) % 2;
  display.fillRect(x + (running ? 0 : 1), y + 9, 3, 3, SSD1306_WHITE);
  display.fillRect(x + (running ? 6 : 5), y + 9, 3, 3, SSD1306_WHITE);
}

void drawBlock(const MarioRun::Block &block, unsigned index, int camera) {
  const int x = block.x - camera;
  if (x + block.width < 0 || x >= 128) return;
  if (block.type == MarioRun::BlockType::Ground) {
    display.drawFastHLine(x, block.y, block.width, SSD1306_WHITE);
    for (int tile = (camera / 8) * 8; tile < camera + 136; tile += 8) {
      if (tile >= block.x && tile < block.x + block.width) {
        display.drawFastVLine(tile - camera, block.y + 1, 3, SSD1306_WHITE);
        display.drawFastVLine(tile + 4 - camera, block.y + 5, 4, SSD1306_WHITE);
      }
    }
    display.drawFastHLine(x, block.y + 4, block.width, SSD1306_WHITE);
  } else if (block.type == MarioRun::BlockType::Pipe) {
    display.drawRect(x + 1, block.y + 3, block.width - 2, block.height - 3, SSD1306_WHITE);
    display.drawRect(x, block.y, block.width, 4, SSD1306_WHITE);
    display.drawFastVLine(x + 3, block.y + 5, block.height - 6, SSD1306_WHITE);
  } else {
    display.drawRect(x, block.y, block.width, block.height, SSD1306_WHITE);
    if (block.type == MarioRun::BlockType::Question && !(world.usedBlocks & (1UL << index))) {
      display.setCursor(x + 1, block.y);
      display.print('?');
    } else if (block.type == MarioRun::BlockType::Brick) {
      for (int row = block.y + 4; row < block.y + block.height; row += 5) {
        display.drawFastHLine(x + 1, row, block.width - 2, SSD1306_WHITE);
      }
      for (int column = x + 5; column < x + block.width - 1; column += 8) {
        display.drawFastVLine(column, block.y + 1, block.height - 2, SSD1306_WHITE);
      }
    }
  }
}

void drawGame() {
  const int camera = world.x > 25 ? static_cast<int>(world.x) - 25 : 0;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.printf("M %u  C %u  1-1", lives, world.coins);
  for (int cloud = 20 - (camera / 3) % 60; cloud < 128; cloud += 60) {
    display.drawFastHLine(cloud, 16, 13, SSD1306_WHITE);
    display.drawFastHLine(cloud + 3, 13, 7, SSD1306_WHITE);
    display.drawPixel(cloud + 2, 15, SSD1306_WHITE);
    display.drawPixel(cloud + 10, 15, SSD1306_WHITE);
  }
  for (unsigned index = 0; index < MarioRun::BLOCK_COUNT; ++index) {
    drawBlock(MarioRun::BLOCKS[index], index, camera);
  }
  for (const MarioRun::Enemy &enemy : world.enemies) {
    if (!enemy.alive) continue;
    const int x = static_cast<int>(enemy.x) - camera;
    if (x < -9 || x >= 128) continue;
    display.fillRoundRect(x, MarioRun::GROUND_Y - 8, 9, 8, 2, SSD1306_WHITE);
    display.drawPixel(x + 2, MarioRun::GROUND_Y - 6, SSD1306_BLACK);
    display.drawPixel(x + 6, MarioRun::GROUND_Y - 6, SSD1306_BLACK);
    display.drawFastHLine(x + 2, MarioRun::GROUND_Y - 3, 5, SSD1306_BLACK);
  }
  const int flagX = MarioRun::FLAG_X - camera;
  if (flagX >= -8 && flagX < 128) {
    display.drawFastVLine(flagX, 15, 40, SSD1306_WHITE);
    display.fillTriangle(flagX, 16, flagX - 8, 20, flagX, 24, SSD1306_WHITE);
  }
  drawMario(static_cast<int>(world.x) - camera, static_cast<int>(world.y));
  display.display();
}

void drawMenu() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(10, 5);
  if (gameState == GameState::Ready) display.print("MARIO RUN");
  else if (gameState == GameState::Won) display.print("YOU WIN!");
  else if (gameState == GameState::Retry) display.print("TRY AGAIN");
  else display.print("GAME OVER");
  display.setTextSize(1);
  display.setCursor(4, 30);
  if (gameState == GameState::Ready) display.print("TAP JUMP  HOLD HIGH");
  else display.printf("LIVES %u  COINS %u", lives, world.coins);
  display.setCursor(16, 50);
  display.print(gameState == GameState::Ready ? "PRESS TO START" : "PRESS TO PLAY");
  display.display();
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_BUZZER, 2000, 8);
#else
  ledcSetup(0, 2000, 8);
  ledcAttachPin(PIN_BUZZER, 0);
#endif
  Wire.begin(PIN_SDA, PIN_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C) &&
      !display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
    Serial.println("OLED not found at 0x3C or 0x3D");
    while (true) delay(1000);
  }
  Wire.setClock(400000);
  rawButtonState = digitalRead(PIN_BUTTON);
  stableButtonState = rawButtonState;
  buttonChangedAt = millis();
  introPlaying = true;
  noteEndsAt = millis();
  Serial.println("Mario Run platformer ready");
  drawMenu();
}

void loop() {
  const uint32_t now = millis();
  const bool pressed = updateButton(now);
  updateSound(now);
  if (pressed) {
    if (gameState == GameState::Playing) jumpPending = true;
    else if (gameState == GameState::Ready || now - stateChangedAt >= 600) {
      if (gameState == GameState::GameOver || gameState == GameState::Won) lives = 3;
      startLevel(now);
    }
  }
  if (gameState == GameState::Playing && now - lastFrameAt >= FRAME_MS) {
    lastFrameAt = now;
    const bool jumped = jumpPending && world.grounded;
    const uint16_t oldCoins = world.coins;
    const uint16_t oldStomps = world.stomps;
    world.step(jumpPending, stableButtonState == LOW);
    jumpPending = false;
    if (world.coins != oldCoins) playTone(1800, 100, now);
    else if (world.stomps != oldStomps) playTone(650, 90, now);
    else if (jumped) playTone(950, 80, now);
    if (world.status == MarioRun::Status::Lost) {
      --lives;
      gameState = lives ? GameState::Retry : GameState::GameOver;
      stateChangedAt = now;
      playTone(180, 350, now);
      Serial.printf("Lost a life, %u remaining\n", lives);
      drawMenu();
    } else if (world.status == MarioRun::Status::Won) {
      gameState = GameState::Won;
      stateChangedAt = now;
      playTone(1200, 300, now);
      Serial.println("Level complete");
      drawMenu();
    } else {
      drawGame();
    }
  }
  delay(1);
}