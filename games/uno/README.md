# UNO

The classic card game against 1 to 3 computer players.

## How to play

- On the title screen, tap to change the number of computer players and hold to start.
- On your turn, tap to move to the next playable card (or the draw option) and hold to play it.
- After a wild card, tap to cycle the color and hold to choose it.
- Skip, Reverse, Draw Two, Wild and Wild Draw Four all work as usual. First to empty their hand wins.
- Your wins and games played are saved.

## Hardware

Standard kit: SSD1306 OLED (I2C 0x3C), button on GPIO 4, passive buzzer on GPIO 23.

## Build and upload

```powershell
pio run -d games/uno -t upload --upload-port COMx
```
