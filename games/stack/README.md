# Stack

Build the tallest tower you can.

## How to play

- A block slides back and forth above the tower. Press to drop it.
- Any overhang is cut off, so the next block gets narrower.
- Land within 2 pixels for a perfect drop. Three perfect drops in a row restore some width.
- Miss the tower completely and the game is over. Your best height is saved.

## Hardware

Standard kit: SSD1306 OLED (I2C 0x3C), button on GPIO 4, passive buzzer on GPIO 23.

## Build and upload

```powershell
pio run -d games/stack -t upload --upload-port COMx
```
