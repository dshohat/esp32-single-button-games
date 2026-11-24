# Dino Run

An endless desert runner inspired by the browser's offline dinosaur.

## How to play

- Tap for a small jump.
- Hold for a big jump. Release early to cut the jump short.
- Jump over cacti and avoid the birds. The game speeds up and switches between day and night as you go.
- Your best score is saved.

## Hardware

Standard kit: SSD1306 OLED (I2C 0x3C), button on GPIO 4, passive buzzer on GPIO 23.

## Build and upload

```powershell
pio run -d games/dino-run -t upload --upload-port COMx
```
