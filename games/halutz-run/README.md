# Halutz Run

A one-button pioneer runner through Israeli landscapes, with Hatikva and Hava Nagila playing in the background.

## How to play

- Press the button to jump.
- Jump over rocks, sabras and birds, and collect stars and gems for bonus points.
- The run speeds up as you go. Your best score is saved.

## Hardware

Standard kit: SSD1306 OLED (I2C 0x3C), button on GPIO 4, passive buzzer on GPIO 23.

## Build and upload

```powershell
pio run -d games/halutz-run -t upload --upload-port COMx
```
