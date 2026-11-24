# Pokemon

Catch a wild Pokemon with one throw, then battle a training dummy. On-screen text is in Hebrew.

## How to play

- **Catch:** a ring shrinks and grows around the wild Pokemon. Press to throw when the ring is smallest for the best chance to catch it.
- **Battle:** press as fast as you can to attack. Faster presses deal more damage. Beat the dummy before it beats you.
- After a battle, a new wild Pokemon appears.

## Hardware

Standard kit: SSD1306 OLED (I2C 0x3C), button on GPIO 4, passive buzzer on GPIO 23. Uses the U8g2 library for Hebrew text.

## Build and upload

```powershell
pio run -d games/pokemon -t upload --upload-port COMx
```
