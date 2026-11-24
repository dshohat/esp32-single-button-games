# Rhythm Reader

Read a bar of music and tap it back in time.

## How to play

- A bar of notes is shown. After a four-click count-in, play it with the button.
- Press at the start of each note.
- Hold half and whole notes for their full length, then release.
- Timing and note length are scored. Pass a bar to move up a level. There are 8 levels, and later ones add syncopated sixteenth notes.

## Hardware

Standard kit: SSD1306 OLED (I2C 0x3C), button on GPIO 4, passive buzzer on GPIO 23.

## Build and upload

```powershell
pio run -d games/rhythm-reader -t upload --upload-port COMx
```
