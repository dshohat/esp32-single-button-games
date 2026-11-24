# Alien Pet

Look after a small alien and help it grow up.

## How to play

- Tap to move between FEED, SLEEP and BATH.
- Hold for 3 seconds to do the highlighted action.
- Keep your alien happy. If it is neglected for too long, it dies.
- With good care it grows from baby to kid to adult. Your best score is saved.

## Hardware

Standard kit: SSD1306 OLED (I2C 0x3C), button on GPIO 4, passive buzzer on GPIO 23.

## Build and upload

```powershell
pio run -d games/alien-pet -t upload --upload-port COMx
```
