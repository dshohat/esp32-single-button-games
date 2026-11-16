# ESP32 Single-Button Games

A Windows-first PlatformIO starter for building small games for the ESP32 single-button kit. The original kit ships with MicroPython and Flappy Bird; this repository uses Arduino-framework C++ and replaces that firmware when you upload a project.

## Kit wiring

| Part | Connection |
| --- | --- |
| 0.96-inch SSD1306 OLED | SDA to GPIO 21, SCL to GPIO 22, VCC to 3V3, GND to GND |
| Push button | Between GPIO 4 and GND; firmware uses `INPUT_PULLUP` |
| Passive buzzer | Positive lead to GPIO 23, negative lead to GND; firmware drives PWM |

Use the kit booklet for breadboard placement and buzzer polarity. Disconnect USB before changing wiring.

## One-time setup

1. Install VS Code and the **PlatformIO IDE** extension.
2. Connect the ESP32 with a USB data cable. If Windows does not show a Silicon Labs CP210x serial device in Device Manager, install the CP210x VCP driver.
3. Open this folder in VS Code and wait for PlatformIO to finish installing its toolchain.
4. Open the PlatformIO terminal and run `pio device list` to identify the ESP32 port. Do not assume a fixed COM number.

   - If PowerShell says `pio` is not recognized, replace `pio` in the commands below with `& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"`, or use the PlatformIO Project Tasks in VS Code.

5. Build the diagnostic firmware:

   ```powershell
   pio run -d hardware-test
   ```

6. The factory board runs MicroPython. To start clean, erase its flash once. This deletes the factory firmware and files. Only run this after confirming the selected port is the kit:

   ```powershell
   pio run -d hardware-test -t erase --upload-port COMx
   ```

7. Upload the diagnostic:

   ```powershell
   pio run -d hardware-test -t upload --upload-port COMx
   pio device monitor -p COMx -b 115200
   ```

Replace `COMx` with the port reported on your computer. If upload cannot connect, follow the PlatformIO prompt and hold the ESP32 BOOT button while resetting/uploading.

## Verify the kit

The diagnostic reports whether an OLED responded at I2C address `0x3C` or `0x3D`, asks you to check the displayed checklist, and emits a short buzzer chirp at startup. Press the kit button: a detected press is reported over serial and triggers another chirp.

Firmware can detect the OLED response and button transitions, but it cannot sense whether pixels are visible or a buzzer is audible. Confirm both yourself. If a part fails, power off and check its wiring against the booklet before continuing.

## Build a game

Use the repository Copilot skill `esp32-single-button-game` by asking Copilot to create a game for this kit. It will create a new PlatformIO project under `games/<game-slug>/`, build it, and upload only after confirming the target board and port. Each game is an independent project. The starter example is in `templates/game/`.

Manual build and upload from the repository root:

```powershell
pio run -d games/<game-slug>
pio run -d games/<game-slug> -t upload --upload-port COMx
pio device monitor -p COMx -b 115200
```

A full erase is not part of normal game uploads.

## Share a game

The default `main` branch is the starter. Reviewed community games live on the `community-games` branch.

**Important:** GitHub defaults pull requests to `main`. For game submissions, change the base branch to `community-games`. Do not merge `community-games` into `main`, or the starter will include all shared games.

To share a game:

1. If you have not already cloned this repository, clone it and enter the directory:

   ```powershell
   git clone https://github.com/dshohat/esp32-single-button-games.git
   cd esp32-single-button-games
   ```

2. Create a branch from `main`. You can do this before or after creating your game; uncommitted game files will remain in your working directory:

   ```powershell
   git switch -c my-game
   ```

   Replace `my-game` with a descriptive branch name.

3. Create your game under `games/<game-slug>/`, including its `platformio.ini`, source code, and a README explaining how to play and build it. Build the game and test it on the kit if possible.
4. Fork this repository on GitHub. Add your fork as a remote, then commit the game folder and push your branch to it:

   ```powershell
   git remote add fork https://github.com/<your-username>/esp32-single-button-games.git
   git add games/<game-slug>
   git commit -m "Add my game"
   git push -u fork my-game
   ```

   Replace `<your-username>` with your GitHub username. If you already cloned your fork instead, push to `origin` and skip adding the `fork` remote.

5. Open a pull request on GitHub from your fork's branch to this repository's **`community-games`** branch. In the pull request form, explicitly select `community-games` as the **base** branch. Check that the pull request includes only your intended changes and describe any hardware testing you completed.

To explore shared games, switch to the `community-games` branch on GitHub or clone it with:

```powershell
git clone --branch community-games https://github.com/dshohat/esp32-single-button-games.git
```

Maintainers should periodically merge `main` into `community-games` to bring starter improvements into the collection.
