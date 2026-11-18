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

1. Confirm the repository is open as the VS Code workspace and PlatformIO IDE/Core is available. In PowerShell, use `pio` if it is on PATH; otherwise set `$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"` and invoke every command as `& $pio ...`. Do not locate the bundled executable and then continue issuing `pio` commands. If neither is available, guide the volunteer through installing the PlatformIO IDE extension and waiting for Core setup; do not install unrelated tools.
2. Ask the volunteer to connect the kit by USB. Run `pio device list` using the resolved command; if multiple likely ports appear, ask which is the kit. Never assume COM5 or select an ambiguous port. A listed USB serial bridge confirms enumeration, not that the ESP32 chip responds. If no kit port appears, follow the Windows USB driver procedure below; do not select an Intel AMT or Bluetooth port as a substitute.
3. Build the diagnostic project with `pio run -d hardware-test`. On a fresh setup, this downloads the ESP32 platform, compiler, framework, and tools including `esptool`; let it finish. Building does not require the board or change its firmware. Do not stop at "esptool is not installed yet": complete this setup before diagnosing chip communication. Fix build errors before attempting upload; for download failures, follow the network troubleshooting below.
4. The board currently has factory MicroPython firmware. Before the first Arduino upload only, explain that a full flash erase removes the factory firmware and stored files, identify the selected port, and ask for explicit confirmation. Do not run erase until the volunteer confirms.
5. After confirmation, run `pio run -d hardware-test -t erase --upload-port <PORT>`, then upload with `pio run -d hardware-test -t upload --upload-port <PORT>`. Do not erase for subsequent games unless the user specifically requests recovery and confirms again.
6. Open serial at 115200 baud. Interpret the diagnostic honestly: I2C scan proves a device responded at an address; a button event proves GPIO4 changed; firmware cannot prove OLED pixels are visible or the buzzer is audible. Ask the volunteer to confirm the screen and chirp in person. If checks fail, power off and inspect booklet wiring, then retest.

### Windows: no kit COM port / CP210x driver

PlatformIO installs the ESP32 build/upload tools, not the Windows USB-to-UART driver. If the scan lists only unrelated ports such as Intel AMT or Bluetooth, the kit's serial connection is not available yet; this is not evidence that the ESP32 chip is faulty.

1. Ask the volunteer to open Device Manager and unplug/reconnect the board, watching both **Ports (COM & LPT)** and **Other devices**. A CP210x device can appear as `CP2102 USB to UART Bridge Controller` with a warning icon instead of a COM port. Identify the changing device by its name or hardware IDs; do not assume every unknown USB device is CP210x.
2. For a Silicon Labs CP210x bridge with a missing driver, direct the volunteer to the official [CP210x Windows driver ZIP](https://www.silabs.com/documents/public/software/CP210x_Windows_Drivers.zip). Ask them to extract it and follow its installation instructions for their Windows version and architecture. If installation is INF-based, use Device Manager > Update driver > Browse my computer for drivers, select the extracted folder, and include subfolders. If the ZIP is blocked or does not support their system, use the [Silicon Labs VCP driver downloads page](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers) for a supported Windows package. Do not fetch drivers from third-party sites or disable driver-signature checks.
3. Driver installation may require administrator approval. Have the volunteer complete any Windows permission prompts themselves; if corporate policy blocks it, refer them to IT. Do not launch an unattended elevated installer or request their password.
4. Ask the volunteer to reconnect the board after installation, confirm that a Silicon Labs CP210x USB-to-UART device now appears under Ports with a COM number and no warning icon, then rerun `pio device list` using the resolved command. Use only the newly confirmed kit port and continue kit verification. Do not claim the driver fixed the problem until the port is actually present; chip communication still requires the separate check below or a successful upload.
5. If no device appears or disappears anywhere in Device Manager when reconnecting, first try a known USB data cable and another USB port; LEDs alone prove power, not a data connection. If the bridge is not CP210x, use the actual bridge manufacturer's driver instead. If the device remains unrecognized, report its exact Device Manager status or error code rather than repeatedly installing drivers, building firmware, or erasing flash.

### Check chip communication without flashing

If the volunteer asks whether the chip responds, or wants to check it before consenting to erase/upload, complete steps 1-3 above, then:

1. Close serial monitors and confirm the selected port belongs to the kit.
2. Run `pio system info` using the resolved PlatformIO command. Use its **Python Executable** and **PlatformIO Core Directory** to locate the bundled interpreter and `packages/tool-esptoolpy/esptool.py`. Do not assume the system Python, `esptool` on PATH, or a particular Core directory. Do not install a separate copy with `pip`.
3. Invoke the bundled tool's `--help` to verify its command syntax. With the standard Windows installation and esptool v4, the commands are:

	```powershell
	& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py" --help
	& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py" --port <PORT> chip_id
	```

	Replace the paths with those discovered above and `<PORT>` with the confirmed port. If the installed tool uses different names or layout, follow its help rather than guessing. This identification command does not erase or write flash, but can reset the board and interrupt its running game.
4. Report chip communication as confirmed only if the command succeeds and identifies the ESP32. A timeout is a connection problem, not proof of a dead chip: close port users and follow the BOOT/reset connection guidance. If setup is blocked, report the exact download/tool error and leave chip communication unverified. Do not erase or upload just to establish that the chip responds; keep the explicit erase confirmation requirement above.

## Create a new game

1. Ask for the game idea and clarify its controls, screen behavior, and sound if the request is underspecified.
2. Choose a lowercase hyphenated slug and create a fresh self-contained PlatformIO project at `games/<slug>/`, using `templates/game/platformio.ini` and `templates/game/src/main.cpp` as the starting point.
3. Never overwrite an existing game directory. If the slug exists, ask for a different name or a deliberate new version folder.
4. Implement the game in C++ for the Arduino framework. Preserve the hardware contract above, use non-blocking timing for gameplay, debounce the button, and keep the display responsive. Add libraries to that game's `platformio.ini` only when needed.
5. Build it with `pio run -d games/<slug>`. Resolve compile errors before upload.
6. Confirm the selected port still belongs to the kit, then upload with `pio run -d games/<slug> -t upload --upload-port <PORT>`. Do not run the erase target during normal game uploads.
7. Use `pio device monitor -p <PORT> -b 115200` when serial output helps, then ask the volunteer to try the game and report what they see/hear. Make requested changes only within that game's folder.

## Branch rules

- Games live only on the `community-games` branch. `main` is the clean starter: under `games/` it holds only `games/README.md`.
- Check the branch with `git status --short --branch` before committing a game. If the checkout is on `main`, stop and ask before committing anything under `games/`.
- When asked to commit or share a game from a checkout of `community-games`, commit there and push to `community-games` directly. Do not create a feature branch or a pull request unless the user asks for one.
- A volunteer without push access shares a game through a fork and a pull request whose base is `community-games`, as described in the README. Set the base explicitly; GitHub defaults it to `main`.
- Never open a pull request into `main` from `community-games` or from any branch that contains games, and never merge `community-games` into `main`. A required check on `main` rejects pull requests that add or change files under `games/`.
- Starter changes (README, templates, hardware test, workflows, this skill) go to `main` through a pull request; direct pushes to `main` are blocked. Bring them into `community-games` by merging `main` into `community-games`, and confirm the game folders are still present before pushing.

## Troubleshooting

- No kit serial device (including scans showing only Intel AMT/Bluetooth): follow the Windows CP210x driver procedure above, then rescan. A missing COM port is a USB connection/driver issue to investigate, not an `esptool` installation or flash-erase problem.
- `esptool` missing: complete `pio run -d hardware-test` first, then use PlatformIO's bundled tool as described above. A missing command on PATH is not a chip or driver failure. If the build succeeds but the bundled tool is still absent, report the actual package/path problem rather than installing an unrelated copy.
- Upload cannot connect: close other serial monitors; retry and follow the uploader's BOOT-button prompt. Do not erase as a generic upload fix.
- Dependency download fails: report the PlatformIO network error and ask whether a corporate proxy is required. Do not repeatedly retry downloads or silently change library versions.
- OLED missing: check power and SDA/SCL routing, then rerun the I2C scan. Report both `0x3C` and `0x3D` as supported probe addresses; do not claim a display works just because I2C ACKs.
- Button never registers: verify the switch spans the intended breadboard rows and connects GPIO4 to GND when pressed. It should read HIGH when released and LOW when pressed.
- Buzzer silent: verify polarity and GPIO23/GND wiring; ensure the code uses PWM/tone suitable for a passive buzzer. Ask for human confirmation after the test.
