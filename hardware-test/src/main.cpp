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

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
bool oledResponded = false;
uint8_t oledAddress = 0;
uint32_t buttonPresses = 0;
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

void playTone(uint16_t frequency, uint16_t durationMs) {
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

bool probeAddress(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

void drawChecklist() {
  if (!oledResponded) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("ESP32 KIT CHECK");
  display.println();
  display.print("OLED I2C: 0x");
  display.println(oledAddress, HEX);
  display.println("Button: press GPIO4");
  display.println("Buzzer: listen for beep");
  display.print("Presses: ");
  display.println(buttonPresses);
  display.display();
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  initBuzzer();
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);

  Serial.println("ESP32 single-button kit diagnostic");
  Serial.println("Expected wiring: OLED SDA=21 SCL=22; button GPIO4-to-GND; buzzer GPIO23-to-GND");
  Serial.println("I2C scan:");
  for (uint8_t address = 1; address < 127; ++address) {
    if (probeAddress(address)) {
      Serial.printf("  device found at 0x%02X\n", address);
    }
  }

  if (probeAddress(0x3C)) oledAddress = 0x3C;
  else if (probeAddress(0x3D)) oledAddress = 0x3D;

  if (oledAddress != 0) {
    oledResponded = display.begin(SSD1306_SWITCHCAPVCC, oledAddress);
  }

  if (oledResponded) {
    Serial.printf("OLED: I2C response and SSD1306 init at 0x%02X. Visually confirm the screen.\n", oledAddress);
  } else {
    Serial.println("OLED: no SSD1306 response at 0x3C or 0x3D. Check wiring/address.");
  }

  drawChecklist();
  Serial.println("BUZZER: playing startup test. Confirm that you hear it.");
  playTone(880, 120);
  delay(80);
  playTone(1320, 120);
  Serial.println("BUTTON: press the kit button; each detected press is reported and chirps.");
  Serial.println("Human check required: confirm the OLED is visible and both buzzer chirps are audible.");
}

void loop() {
  const bool reading = digitalRead(PIN_BUTTON);
  const uint32_t now = millis();

  if (reading != rawButtonState) {
    rawButtonState = reading;
    buttonChangedAt = now;
  }

  if (now - buttonChangedAt >= 35 && stableButtonState != rawButtonState) {
    stableButtonState = rawButtonState;
    if (stableButtonState == LOW) {
      ++buttonPresses;
      Serial.printf("BUTTON: press detected (%lu)\n", static_cast<unsigned long>(buttonPresses));
      playTone(1100, 90);
      drawChecklist();
    }
  }

  delay(5);
}
