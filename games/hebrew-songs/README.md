# Hebrew Songs

A one-button player for five Hebrew songs: Maoz Tzur, Yemei HaHanukkah, Sevivon Sov Sov Sov, Mi Yemalel and Hava Nerima. Maoz Tzur shows its lyrics line by line. The others show only their titles.

## How to use

- Short press: go to the next song.
- Hold (0.6 s): play the selected song.
- Press during playback: stop and go back to the menu.

## Hardware

Standard kit: SSD1306 OLED (I2C 0x3C), button on GPIO 4, passive buzzer on GPIO 23. Uses the U8g2 library for Hebrew text.

## Build and upload

```powershell
pio run -d games/hebrew-songs -t upload --upload-port COMx
```
