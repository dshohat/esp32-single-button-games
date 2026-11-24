# Naomi's Balloon (הבלון של נעמי)

Help Naomi float on her balloon, collect stars and dodge bees. On-screen text is in Hebrew.

## How to play

- Hold the button to float up. Release it to sink.
- Collect stars. Every 10 stars earns a cheer.
- Grab hearts for extra lives and avoid the bees.
- Your best star count is saved.

## Hardware

Standard kit: SSD1306 OLED (I2C 0x3C), button on GPIO 4, passive buzzer on GPIO 23. Uses the U8g2 library for Hebrew text.

## Build and upload

```powershell
pio run -d games/naomi-balloon -t upload --upload-port COMx
```
