---
name: esp32-single-button-game
description: "Create, build, troubleshoot, and upload Arduino C++ games for the ESP32 single-button kit using PlatformIO in VS Code. Use when a volunteer asks to make a game, test kit wiring, connect to the ESP32, flash firmware, or debug the OLED, button, buzzer, serial port, or upload."
---

# ESP32 Single-Button Game Workflow

Use this skill for the ESP32 single-button kit in this repository. The kit's factory firmware is MicroPython/Flappy; projects here use PlatformIO with the Arduino framework and C++.

## Hardware contract

- Board: classic ESP32-WROOM development board, PlatformIO board id `esp32dev`.
- OLED: SSD1306 128x64 I2C; SDA GPIO21, SCL GPIO22; expected address `0x3C` (also probe `0x3D`); VCC 3V3 and GND.
- Button: GPIO4 to GND, configured `INPUT_PULLUP` (pressed reads LOW).
- Passive buzzer: positive to GPIO23, negative to GND; use PWM/tone, not a constant HIGH as the only sound test.
- The kit booklet is authoritative for physical placement and polarity. Disconnect USB before changing wiring.

## Start with kit verification

1. Confirm the repository is open as the VS Code workspace and PlatformIO IDE/Core is available. If `pio` is not on PATH, check `$env:USERPROFILE\.platformio\penv\Scripts\pio.exe` and invoke it with PowerShell's call operator (`&`). If neither exists, guide the volunteer through installing the PlatformIO IDE extension and waiting for Core setup; do not install unrelated tools.
2. Ask the volunteer to connect the kit by USB. Run `pio device list` (or the discovered Core executable); if multiple likely ports appear, ask which is the kit. Never assume COM5 or select an ambiguous port.
3. Build the diagnostic project with `pio run -d hardware-test`. Fix build errors before attempting upload.
4. The board currently has factory MicroPython firmware. Before the first Arduino upload only, explain that a full flash erase removes the factory firmware and stored files, identify the selected port, and ask for explicit confirmation. Do not run erase until the volunteer confirms.
5. After confirmation, run `pio run -d hardware-test -t erase --upload-port <PORT>`, then upload with `pio run -d hardware-test -t upload --upload-port <PORT>`. Do not erase for subsequent games unless the user specifically requests recovery and confirms again.
6. Open serial at 115200 baud. Interpret the diagnostic honestly: I2C scan proves a device responded at an address; a button event proves GPIO4 changed; firmware cannot prove OLED pixels are visible or the buzzer is audible. Ask the volunteer to confirm the screen and chirp in person. If checks fail, power off and inspect booklet wiring, then retest.

## Create a new game

1. Ask for the game idea and clarify its controls, screen behavior, and sound if the request is underspecified.
2. Choose a lowercase hyphenated slug and create a fresh self-contained PlatformIO project at `games/<slug>/`, using `templates/game/platformio.ini` and `templates/game/src/main.cpp` as the starting point.
3. Never overwrite an existing game directory. If the slug exists, ask for a different name or a deliberate new version folder.
4. Implement the game in C++ for the Arduino framework. Preserve the hardware contract above, use non-blocking timing for gameplay, debounce the button, and keep the display responsive. Add libraries to that game's `platformio.ini` only when needed.
5. Build it with `pio run -d games/<slug>`. Resolve compile errors before upload.
6. Confirm the selected port still belongs to the kit, then upload with `pio run -d games/<slug> -t upload --upload-port <PORT>`. Do not run the erase target during normal game uploads.
7. Use `pio device monitor -p <PORT> -b 115200` when serial output helps, then ask the volunteer to try the game and report what they see/hear. Make requested changes only within that game's folder.

## Troubleshooting

- No serial device: try a known USB data cable and another USB port; check Device Manager and install the CP210x VCP driver if this board uses that bridge.
- Upload cannot connect: close other serial monitors; retry and follow the uploader's BOOT-button prompt. Do not erase as a generic upload fix.
- Dependency download fails: report the PlatformIO network error and ask whether a corporate proxy is required. Do not repeatedly retry downloads or silently change library versions.
- OLED missing: check power and SDA/SCL routing, then rerun the I2C scan. Report both `0x3C` and `0x3D` as supported probe addresses; do not claim a display works just because I2C ACKs.
- Button never registers: verify the switch spans the intended breadboard rows and connects GPIO4 to GND when pressed. It should read HIGH when released and LOW when pressed.
- Buzzer silent: verify polarity and GPIO23/GND wiring; ensure the code uses PWM/tone suitable for a passive buzzer. Ask for human confirmation after the test.
