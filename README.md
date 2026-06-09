# IV-18 clock

DIY clock with IV-18 VFD display (ИВ-18 in Russian).

Hardware schematics is published https://oshwlab.com/alexander.krotov/iv-18-clock_copy_copy_copy

It should work with any GPS module supported by TinyGPS library (tested with NEO-8m and NEO-6 only).

Key components:
- IV-18 display: https://www.radiomuseum.org/tubes/tube_iv-18.html (better spec in Russian: https://radioizba.ru/cat/PIC/605Q0503400.pdf )
- ESP32C3 Super-Mini module: https://www.espboards.dev/esp32/esp32-c3-super-mini/
- Neo-8M GPS module: https://www.u-blox.com/en/product/neo-6-series
- DS3231 RTC: https://www.analog.com/en/products/ds3231.html
- MAX6921 display driver: https://www.analog.com/en/products/max6921.html

![clock text](https://github.com/alexander-krotov/IV18-clock/blob/esp32/picture.jpg?raw=true)

Final clock video: https://www.youtube.com/watch?v=4OeplCNZRic

# Short user's manual

## Clock Operation

The clock is powered via a USB-C connector located on the ESP32-C3 Super Mini module. The same USB-C port is also used for firmware flashing.

When powered on, the clock attempts to connect to a known Wi-Fi network. If a connection is established, it displays the assigned IP address. Otherwise, it starts its own access point named **NixieClock** and waits for configuration settings for one minute.
Network setup is very similar to https://github.com/alexander-krotov/apollo-clock/blob/main/setup.md

Once the time has been set, the clock retains it even when powered off, provided that a backup battery is installed.

## Displayed Information

The clock displays:

* Current date in **dd.mm.yyyy** format
* Current time in **hh-mm-ss** format
* Temperature measured by one of the onboard sensors

If a GPS location is available, the clock also displays:

* Location in **`L ww nn`** format (latitude and longitude with one-degree precision)
* Altitude in **`A mmm`** format (meters above sea level)

## Time Sources

The clock can use either NTP or GPS as its time source. Both provide UTC time without applying time zone offsets or daylight saving time (DST) adjustments. A local time zone can be configured through the clock's web interface if required.

If both a GPS position and a Wi-Fi connection are available, the clock automatically determines the local time zone and applies the correct offset.

## Temperature display note

One of the board transistor significantly heats and affects the temperatue sensor on the clock borad, so the temperature display could be very incorrect. It could be turned off in the clock Web UI.



