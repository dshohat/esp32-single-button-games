# Penalty

Penalty kick practice with a moving goal.

## How to play

- The goal slides left and right while an aim arrow sweeps across the top.
- Press to shoot where the arrow points. If the ball goes between the posts, it's a goal.
- Press after the result to take another shot.

## Hardware

Standard kit: SSD1306 OLED (I2C 0x3C), button on GPIO 4, passive buzzer on GPIO 23.

## Build and upload

```powershell
pio run -d games/penalty -t upload --upload-port COMx
```
