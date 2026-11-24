# Lane Racer

Dodge oncoming traffic on a two-lane road.

## How to play

- Press to switch lanes.
- Avoid cars and trucks. Collect coins for +25 points each.
- Traffic gets faster over time. Your best score is saved.

## Hardware

Standard kit: SSD1306 OLED (I2C 0x3C), button on GPIO 4, passive buzzer on GPIO 23.

## Build and upload

```powershell
pio run -d games/lane-racer -t upload --upload-port COMx
```
