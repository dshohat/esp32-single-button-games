# Pet Care

Look after a cat, dog, bunny, bear or mouse. On-screen text is in Hebrew.

## How to play

- The pet shows and sounds what it needs: sleep, food, medicine or petting.
- The care icons light up in turn. Press when the matching icon is highlighted.
- The right choice earns hearts. Wrong choices make the reward smaller.
- After a happy pet, a new animal arrives with a new need.

## Hardware

Standard kit: SSD1306 OLED (I2C 0x3C), button on GPIO 4, passive buzzer on GPIO 23. Uses the U8g2 library for Hebrew text.

## Build and upload

```powershell
pio run -d games/pet-care -t upload --upload-port COMx
```
