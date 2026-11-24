# Whack-a-Mole

Three holes, three buttons. Hit the moles before they hide.

> This game needs extra parts beyond the single-button kit: two more buttons and two LEDs.

## How to play

- Each hole has its own button: left, middle and right.
- Press a hole's button while its mole is up to whack it.
- Missing a mole costs a life. Later in the game bombs appear too, and hitting one also costs a life.
- You have 3 lives. Moles get faster as your score rises.

## Hardware

| Part | Pin |
| --- | --- |
| Left button | GPIO 18 to GND |
| Middle button | GPIO 4 to GND (the kit button) |
| Right button | GPIO 19 to GND |
| Green LED | GPIO 25 |
| Red LED | GPIO 26 |
| Passive buzzer | GPIO 23 |
| SSD1306 OLED | I2C 0x3C |

The LEDs are driven at the lowest drive strength. Use a series resistor if you have one.

## Build and upload

```powershell
pio run -d games/whack-a-mole -t upload --upload-port COMx
```
