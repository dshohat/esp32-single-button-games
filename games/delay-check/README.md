# Delay Check

A one-button timing game for the ESP32 kit. The OLED shows a random target duration from 0.5 to 10.0 seconds in 0.1-second increments. Press and hold the button for that duration, then release.

Each round scores `max(0, 10 - errorSeconds * 10)`, where `errorSeconds` is the absolute difference between the target and actual hold time. Scores are shown to one decimal place: an exact match earns 10.0 points; an error of 1.0 seconds or more earns 0.0. The running total is retained until the board resets.

After each result, the next target appears automatically. Hardware uses the kit wiring: OLED SDA=21/SCL=22, button GPIO4-to-GND, and passive buzzer GPIO23-to-GND.

Build from the repository root with `pio run -d games/delay-check`; upload with `pio run -d games/delay-check -t upload --upload-port COMx` after identifying the correct board port.
