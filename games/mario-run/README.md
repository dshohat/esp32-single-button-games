# Mario Run

Mario runs automatically through a short side-scrolling level. Press to start and tap to jump; hold the button for a higher, longer jump. Land on pipes and blocks, hit question blocks from below to collect coins, stomp enemies from above, and jump across pits. Reach the flagpole to win.

You have three lives. After losing a life, press to restart the level. The buzzer plays an original single-note intro and sound effects; it does not play Nintendo's original theme. The OLED is monochrome, and one-button automatic movement is different from the original game's directional controls.

The course takes inspiration from [World 1-1's layout](https://www.mariowiki.com/World_1-1_(Super_Mario_Bros.)) and [Super Mario Bros. gameplay](https://en.wikipedia.org/wiki/Super_Mario_Bros.), but is adapted for the kit.

The platform physics can also be tested on a computer with a C++11 compiler:

```powershell
g++ -O2 -std=c++11 games/mario-run/test/world_test.cpp -o "$env:TEMP\mario-world-test.exe"
& "$env:TEMP\mario-world-test.exe"
```

Build and upload from the repository root, replacing `COMx` with the kit's serial port:

```powershell
pio run -d games/mario-run
pio run -d games/mario-run -t upload --upload-port COMx
```

Uses the standard kit wiring: button GPIO 4, buzzer GPIO 23, and SSD1306 OLED on GPIO 21/22.