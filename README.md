# IV-18 clock

DIY clock with IV-18 VFD display (ИВ-18 in Russian).

Hardware schematics is published https://oshwlab.com/alexander.krotov/iv-18-clock

Key components:
- IV-18 display: https://www.radiomuseum.org/tubes/tube_iv-18.html (better spec in Russian: https://radioizba.ru/cat/PIC/605Q0503400.pdf )
- Arduino Nano: https://store.arduino.cc/products/arduino-nano
- Neo-6M GPS module: https://www.u-blox.com/en/product/neo-6-series
- DS3231 RTC: https://www.analog.com/en/products/ds3231.html
- MAX6921 display driver: https://www.analog.com/en/products/max6921.html

For the future (if I ever get back to this project): MAX7221 looks much better solution for similar design.

![clock text](https://github.com/alexander-krotov/IV18-clock/blob/main/picture.jpg?raw=true)

Final clock video: https://www.youtube.com/watch?v=CX6nEgpd1cs

# Short user's manual

Clock is powered by a micro-usb connector. There is a mini-usb connector on one of the onborad modules, it is not
to power the clock, only for flashing a new firmware. 

Clock has a very minimalistic user interface: it uses onboard GPS to set the correct time. It reads the time from GPS 
after the very first boot, if the backup battery was replaced, or if one presses a button on the clock board. It might take
some time (minutes) for the onboard GPS module to get the time from the satellites. Indoors it might take forever,
place the clock closer to a window to help the clock to set the time.

Once the time is set the clock keeps it even if powered off, if there is a backup battery inserted.

At the moment only two time zones are supported: EET (Helsinki) and PDT (Seattle).

Clock shows the current date in dd.mm.yyyy format, current time in hh-mm-ss format, temperature (measured by one of the
black board chips). If GPS location is known it prints the clock location (in "L ww nn" format, with one degree precision),
and altitude (in "A mmm" format, in meters).
