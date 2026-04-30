# IV-18 clock

DIY clock with IV-18 VFD display (ИВ-18 in Russian).

Hardware schematics is published https://oshwlab.com/alexander.krotov/iv-18-clock XXX

Key components:
- IV-18 display: https://www.radiomuseum.org/tubes/tube_iv-18.html (better spec in Russian: https://radioizba.ru/cat/PIC/605Q0503400.pdf )
- ESP32C3 Super-Mini module: https://www.espboards.dev/esp32/esp32-c3-super-mini/
- Neo-8M GPS module: https://www.u-blox.com/en/product/neo-6-series
- DS3231 RTC: https://www.analog.com/en/products/ds3231.html
- MAX6921 display driver: https://www.analog.com/en/products/max6921.html

![clock text](https://github.com/alexander-krotov/IV18-clock/blob/main/picture.jpg?raw=true)

Final clock video: https://www.youtube.com/watch?v=CX6nEgpd1cs

# Short user's manual

Clock is powered from a usb-c connector, placed on ESP32C3 Super-Mini module. Same usb-c port is used to flash the firmware.

Once turned on it connects to known WiFi network. If available it runs the assigned IP address on the display, otherwise it starts its own access point NixieClock, and for 1 minute waits for configuration settings.

Once the time is set the clock keeps it even if powered off, if there is a backup battery inserted.

Clock shows the current date in dd.mm.yyyy format, current time in hh-mm-ss format, temperature (measured by one of the
black board chips). If GPS location is known it prints the clock location (in "L ww nn" format, with one degree precision),
and altitude (in "A mmm" format, in meters).

Clock can use both NTP and GPS as the time source. Both provide UTC time, without adjusting to the current timezone,
and do not do that silly DST changes. Local timezone could be configured in the clocks Web-UI, if needed.

If both GPS position and WiFi network are available the clock automatically finds the local timezone and adjusts to
the timezone automatically.
