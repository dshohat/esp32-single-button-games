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
static constexpr uint32_t RESULT_DISPLAY_MS = 3000;
static constexpr uint32_t HOLD_REFRESH_MS = 75;

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

enum class GameState { Ready, Holding, Result };
GameState gameState = GameState::Ready;
uint16_t targetTenths = 5;
uint16_t roundNumber = 0;
uint32_t totalScoreTenths = 0;
uint32_t holdStartedAt = 0;
uint32_t resultShownAt = 0;
uint32_t lastHoldDrawAt = 0;
bool rawButtonState = HIGH;
bool stableButtonState = HIGH;
uint32_t buttonChangedAt = 0;

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

void printTenths(uint16_t tenths) {
  display.print(tenths / 10);
  display.print('.');
  display.print(tenths % 10);
  display.print('s');
}

void printPoints(uint32_t pointsTenths) {
  display.print(pointsTenths / 10);
  display.print('.');
  display.print(pointsTenths % 10);
}

void showReadyScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("DELAY CHECK");
  display.setCursor(0, 16);
  display.println("Hold for this long:");
  display.setTextSize(3);
  display.setCursor(28, 29);
  printTenths(targetTenths);
  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print("Press, hold, release");
  display.display();
}

void showHoldingScreen(uint32_t now) {
  const uint32_t elapsedMs = now - holdStartedAt;
  const uint16_t elapsedTenths = static_cast<uint16_t>((elapsedMs + 50) / 100);

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("TARGET ");
  printTenths(targetTenths);
  display.setTextSize(2);
  display.setCursor(0, 22);
  display.print("HELD ");
  printTenths(elapsedTenths);
  display.setTextSize(1);
  display.setCursor(0, 54);
  display.print("Release at the target");
  display.display();
}

void showResultScreen(uint32_t heldMs, uint32_t errorMs, uint32_t scoreTenths) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("R");
  display.print(roundNumber);
  display.print(" SCORE ");
  printPoints(scoreTenths);

  display.setCursor(0, 13);
  display.print("Target: ");
  printTenths(targetTenths);
  display.setCursor(0, 23);
  display.print("Held:   ");
  printTenths(static_cast<uint16_t>((heldMs + 50) / 100));
  display.setCursor(0, 33);
  display.print("Error:  ");
  printTenths(static_cast<uint16_t>((errorMs + 50) / 100));
  display.setCursor(0, 43);
  display.print("Total:  ");
  printPoints(totalScoreTenths);
  display.println(" pts");
  display.setCursor(0, 55);
  display.print("New target soon");
  display.display();
}

bool updateButton() {
  const bool reading = digitalRead(PIN_BUTTON);
  const uint32_t now = millis();

  if (reading != rawButtonState) {
    rawButtonState = reading;
    buttonChangedAt = now;
  }
  if (now - buttonChangedAt >= 30 && stableButtonState != rawButtonState) {
    stableButtonState = rawButtonState;
    return true;
  }
  return false;
}

void beginHold(uint32_t now) {
  gameState = GameState::Holding;
  holdStartedAt = now;
  lastHoldDrawAt = 0;
  Serial.printf("Round %u: target %u.%u seconds\n", roundNumber + 1,
                targetTenths / 10, targetTenths % 10);
  showHoldingScreen(now);
}

void finishHold(uint32_t now) {
  const uint32_t heldMs = now - holdStartedAt;
  const uint32_t targetMs = static_cast<uint32_t>(targetTenths) * 100;
  const uint32_t errorMs = heldMs > targetMs ? heldMs - targetMs : targetMs - heldMs;
  const uint32_t scoreTenths = errorMs >= 1000 ? 0 : 100 - ((errorMs + 5) / 10);

  ++roundNumber;
  totalScoreTenths += scoreTenths;
  gameState = GameState::Result;
  resultShownAt = now;
  showResultScreen(heldMs, errorMs, scoreTenths);
  Serial.printf("Held %lu ms; error %lu ms; score %lu.%lu; total %lu.%lu\n",
                static_cast<unsigned long>(heldMs),
                static_cast<unsigned long>(errorMs),
                static_cast<unsigned long>(scoreTenths / 10),
                static_cast<unsigned long>(scoreTenths % 10),
                static_cast<unsigned long>(totalScoreTenths / 10),
                static_cast<unsigned long>(totalScoreTenths % 10));
  beep(1200, 90);
}

void startNextRound() {
  targetTenths = static_cast<uint16_t>(random(5, 101));
  gameState = GameState::Ready;
  showReadyScreen();
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

  rawButtonState = digitalRead(PIN_BUTTON);
  stableButtonState = rawButtonState;
  buttonChangedAt = millis();
  targetTenths = static_cast<uint16_t>(random(5, 101));
  Serial.println("Delay Check ready. Press and hold for the displayed target, then release.");
  showReadyScreen();
}

void loop() {
  const uint32_t now = millis();
  if (updateButton()) {
    if (stableButtonState == LOW && gameState == GameState::Ready) {
      beginHold(now);
    } else if (stableButtonState == HIGH && gameState == GameState::Holding) {
      finishHold(now);
    }
  }

  if (gameState == GameState::Holding && now - lastHoldDrawAt >= HOLD_REFRESH_MS) {
    lastHoldDrawAt = now;
    showHoldingScreen(now);
  } else if (gameState == GameState::Result && now - resultShownAt >= RESULT_DISPLAY_MS) {
    startNextRound();
  }

  delay(5);
}
