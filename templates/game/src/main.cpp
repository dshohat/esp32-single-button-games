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

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
enum class GameState { Ready, Waiting, Timing, Result, FalseStart };
GameState gameState = GameState::Ready;
bool rawButtonState = HIGH;
bool stableButtonState = HIGH;
uint32_t buttonChangedAt = 0;
uint32_t deadline = 0;
uint32_t reactionStartedAt = 0;
uint32_t reactionMs = 0;

void initBuzzer() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_BUZZER, 2000, 8);
#else
  ledcSetup(0, 2000, 8);
  ledcAttachPin(PIN_BUZZER, 0);
#endif
}

void beep(uint16_t frequency, uint16_t durationMs) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(PIN_BUZZER, frequency);
#else
  ledcWriteTone(0, frequency);
#endif
  delay(durationMs);
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(PIN_BUZZER, 0);
#else
  ledcWriteTone(0, 0);
#endif
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

void startRound() {
  gameState = GameState::Waiting;
  deadline = millis() + random(1500, 4500);
  showMessage("WAIT...", "Press when GO!");
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
  Serial.println("Reaction game ready");
  showMessage("REACTION", "Press to start");
}

void loop() {
  const uint32_t now = millis();
  const bool pressed = wasPressed();

  switch (gameState) {
    case GameState::Ready:
      if (pressed) startRound();
      break;

    case GameState::Waiting:
      if (static_cast<int32_t>(now - deadline) >= 0) {
        gameState = GameState::Timing;
        reactionStartedAt = now;
        showMessage("GO!", "Press now!");
        beep(1200, 70);
        reactionStartedAt = millis();
      } else if (pressed) {
        gameState = GameState::FalseStart;
        deadline = now + 1200;
        showMessage("TOO SOON", "Wait for GO");
      }
      break;

    case GameState::Timing:
      if (pressed) {
        reactionMs = now - reactionStartedAt;
        gameState = GameState::Result;
        deadline = now + 2200;
        display.clearDisplay();
        display.setTextColor(SSD1306_WHITE);
        display.setTextSize(2);
        display.setCursor(0, 8);
        display.println("TIME");
        display.setTextSize(2);
        display.setCursor(0, 36);
        display.printf("%lu ms", static_cast<unsigned long>(reactionMs));
        display.display();
        beep(1500, 90);
        Serial.printf("Reaction time: %lu ms\n", static_cast<unsigned long>(reactionMs));
      }
      break;

    case GameState::Result:
      if (pressed || static_cast<int32_t>(now - deadline) >= 0) {
        gameState = GameState::Ready;
        showMessage("REACTION", "Press to start");
      }
      break;

    case GameState::FalseStart:
      if (static_cast<int32_t>(now - deadline) >= 0) {
        gameState = GameState::Ready;
        showMessage("REACTION", "Press to start");
      }
      break;
  }

  delay(5);
}
