# Tetris

Falling blocks with one button.

## How to play

- The falling piece slides left and right by itself.
- Tap to rotate the piece.
- Hold (about a quarter of a second) to drop it straight down.
- Clear lines to score. Every 10 lines the level goes up and pieces fall faster. Your best score is saved.

## Hardware

Standard kit: SSD1306 OLED (I2C 0x3C), button on GPIO 4, passive buzzer on GPIO 23.

## Build and upload

```powershell
pio run -d games/tetris -t upload --upload-port COMx
```
