// IV-18 based clock. 
// Full project description: https://github.com/alexander-krotov/IV18-clock

// Use Board: "Arduino Nano", and "ATMega328P (Old bootloader)" when flashing.

#include <TinyGPSPlus.h>
#include <SoftwareSerial.h>
#include <Timezone.h>
#include <DS3231.h>
#include <Wire.h>

// RTC chip.
DS3231 rtc;
// RTC year is one byte. We need to adjust it.
const int RTC_BASE_YEAR = 2000;

// GPS module communication pins
static const int RXPin = 2, TXPin = 3;
static const uint32_t GPSBaud = 9600;

// Pins used with MAX6921
static const int DINPin = A0, LOADPin = A1, CLKPin = A2, BLANKPin = A3;

// Push button pin.
static const int KEYPin = A7;

// The TinyGPSPlus object
TinyGPSPlus gps;
bool gps_info_set = false;  // Time was set from GPS at some point.

// The serial connection to the GPS device
SoftwareSerial ss(RXPin, TXPin);

// US Seattle time zone
TimeChangeRule usEDT = {"EDT", Second, Sun, Mar, 2, -240};  //UTC - 4 hours
TimeChangeRule usEST = {"EST", First, Sun, Nov, 2, -300};   //UTC - 5 hours
Timezone tz_us(usEDT, usEST);

// Eastern European Time (Finland)
TimeChangeRule rEST = {"EST", Last, Sun, Mar, 1, 180};      //Eastern European Time
TimeChangeRule rEET = {"EET", Last, Sun, Oct, 1, 120};      //Eastern European Summer Time
Timezone tz_fi(rEST, rEET);

// Default timezone is Finland. We will change it later, based on the GPS position.
Timezone *my_tz = &tz_fi;

// IV-18 display size
const int display_size = 8;
// What we show on display
char display_string[display_size+1];
// Display digital dots.
bool dots[display_size];

// Characters we can display on 7-segment indicator.
enum display_char {
  CHAR_0, CHAR_1, CHAR_2, CHAR_3, CHAR_4, CHAR_5, CHAR_6, CHAR_7, CHAR_8, CHAR_9, 
  CHAR_BLANK, CHAR_MINUS, CHAR_P, CHAR_C, CHAR_L, CHAR_o, CHAR_A, CHAR_E
};

void setup()
{
  Serial.begin(115200);

  // Communicate with MAX6921 using this pins.
  pinMode(BLANKPin, OUTPUT);
  pinMode(CLKPin, OUTPUT);
  pinMode(LOADPin, OUTPUT);
  pinMode(DINPin, OUTPUT);

  // Key press pin
  pinMode(KEYPin, INPUT);

  // Wire is used for DS3231.
  Wire.begin();

  // Turn off the display
  digitalWrite(BLANKPin, HIGH);

  // Year was set earlier. We assume it was set from GPS, and we do not need to
  // immediatelly go to GPS reading.
  if (rtc.getYear()>0) {
    gps_info_set = true;
  } else {
    Serial.println("Time not set");
    ss.begin(GPSBaud);
  }
  set_time_from_rtc();

  Serial.println("IV-18.ino started");
}

// 7-segment indicator bits.
//   1
// 2   4
//   8
// 16  32
//   64
int display_char_bits[] = {
  1+2+4+16+32+64,   // CHAR_0,
  4+32,             // CHAR_1
  1+4+8+16+64,      // CHAR_2,
  1+4+8+32+64,      // CHAR_3
  2+4+8+32,         // CHAR_4
  1+2+8+32+64,      // CHAR_5
  1+2+8+16+32+64,   // CHAR_6
  1+4+32,           // CHAR_7
  1+2+4+8+16+32+64, // CHAR_8
  1+2+4+8+32+64,    // CHAR_9, 
  0,                // CHAR_BLANK
  8,                // CHAR_MINUS
  1+2+4+8+16,       // CHAR_P
  1+2+16+64,        // CHAR_C
  2+16+64,          // CHAR_L,
  1+2+4+8,          // CHAR_o
  1+2+4+8+16+32,    // CHAR_A
  1+2+8+16+64       // CHAR_E
};

// Get the bits for character (default is BLANK = all 0).
int get_char_bits(char c)
{
  int bits = 0;

  // Find the bits for character (default is BLANK = all 0).
  switch (c) {
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
      bits = display_char_bits[c-'0'];
      break;
    case '-':
      bits = display_char_bits[CHAR_MINUS];
      break;
    case 'P':
      bits = display_char_bits[CHAR_P];
      break;
    case 'C':
      bits = display_char_bits[CHAR_C];
      break;
    case 'L':
      bits = display_char_bits[CHAR_L];
      break;
    case 'o':
      bits = display_char_bits[CHAR_o];
      break;
    case 'A':
      bits = display_char_bits[CHAR_A];
      break;
    case 'E':
      bits = display_char_bits[CHAR_E];
      break;
    }

    return bits;
}

// Show the contents of display_string on IV-18 display.
// Please refer to MAX6921 documentation about how we send the data in 19-bit encoded strings.
void show_display_string()
{
  // In this order digits are sent to MAX6921.
  // The order is really all about hardware wiring.
  int display_order[] = { 6, 4, 2, 1, 0, 3, 5, 7 };

  // Turn on the display
  digitalWrite(BLANKPin, LOW);

  // Loop through the display digits
  for (int i=0; i<display_size; i++) {
    // Take a digit in display order.
    int c = display_string[display_order[i]];

    // Character encoding for 7-segment display.
    int bits = get_char_bits(c);

    // First we send the encoded char bits (highest first).
    for (int j=0; j<10; j++) {
      digitalWrite(CLKPin, LOW);
      digitalWrite(DINPin, (bits&(1<<(9-j))) ? HIGH: LOW);
      digitalWrite(CLKPin, HIGH);
    }

    // Next is the decimal point (DP) bit.
    digitalWrite(CLKPin, LOW);
    digitalWrite(DINPin, dots[display_order[i]] ? HIGH: LOW);
    digitalWrite(CLKPin, HIGH);

    // Next is the digit number (decoded as 8-bit bit mask).
    for (int j=0; j<9; j++) {
      digitalWrite(CLKPin, LOW);
      digitalWrite(DINPin, j==i ? HIGH: LOW);
      digitalWrite(CLKPin, HIGH);
    }
    // Do not leave the CLK pin high.
    digitalWrite(CLKPin, LOW);

    // Shift the digit to display.
    digitalWrite(LOADPin, HIGH);
    digitalWrite(LOADPin, LOW);

    // Delay to show the digit on display for short time.
    if (gps_info_set) {
      // 1ms is sort of magic value: less - and the digits are dimmed,
      // more - and it starts to flicker.
      delay(1);
    }
  }

  // Turn off the display (the last digit was flickering a bit).
  digitalWrite(BLANKPin, HIGH);
}

// Read time from rtc and set it to the arduino system time
void set_time_from_rtc()
{
  bool h12, PM_time, century;
  setTime(rtc.getHour(h12, PM_time), rtc.getMinute(), rtc.getSecond(), rtc.getDate(), rtc.getMonth(century), rtc.getYear()+RTC_BASE_YEAR);
}

// Set display_string to show the time from RTC
void display_time()
{
  static int last_update_from_rtc;

  // Periodically set the time from rtc.
  if (now() > last_update_from_rtc+1000) {
    set_time_from_rtc();
    last_update_from_rtc = now();
  }

  snprintf(display_string, sizeof(display_string), "%2d-%02d-%02d", hour(), minute(), second());
}

// Set display_string to show the date from RTC
void display_date()
{
  snprintf(display_string, sizeof(display_string), "%2d%02d%4d", day(), month(), year());
  dots[1] = dots[3] = true;
}

// Set display_string to show the GPS location.
// Format is "L longitude latitude", including possible - sign.
// Shows the values with one degree precision.
void display_location()
{
  snprintf(display_string, sizeof(display_string), "L %3d %d3", (int)gps.location.lng(), (int)gps.location.lat());
}

// Set display_string to show the temperature.
// DS3231 has a built-int temperature sensor.
void display_temp()
{
  snprintf(display_string, sizeof(display_string), "%4d oC", (int)rtc.getTemperature());
}

// Set display_string to show the GPS altitude.
void display_altitude()
{
  int alt_cm = gps.altitude.meters()*100;

  // Altitude cell part
  int c=alt_cm/100;
  // Altitude fraction part
  int p = alt_cm>0 ? alt_cm%100: (-alt_cm)%100;
 
  snprintf(display_string, sizeof(display_string), "A %4d%02d", c, p);
  dots[5] = true;
}

// Set RTC time from gps.
// RTC time is set according to the local time zone.
void set_rtc_time()
{
  // There must be some easier way to set the time form GPS.
  setTime(gps.time.hour(), gps.time.minute(), gps.time.second(), gps.date.day(), gps.date.month(), gps.date.year());
  time_t utc_time = now();
  time_t my_local_time = my_tz->toLocal(utc_time);
  setTime(my_local_time);

  rtc.setClockMode(false);  // set to 24h

  // Save the time to rtc.
  rtc.setYear(year()-RTC_BASE_YEAR);  // RTC year is one byte. Adjust it.
  rtc.setMonth(month());
  rtc.setDate(day());
  rtc.setHour(hour());
  rtc.setMinute(minute());
  rtc.setSecond(second());
}

// Update the display string.
void update_display()
{
  // Blank everything.
  for (int i=0; i<display_size; i++) {
    display_string[i] = 0;
    dots[i] = false;
  }

  // Display mode.
  // In 10seconds loop show the time, date, temperature, and if available
  // show the location and altitude.
  int mode = (now()/2)%10;
  
  // In a loop show what we know: date, time, gps location, altitude, temperature.
  if (mode==0) {
    display_temp();
  } else if (mode==1 || mode == 2) {
    display_date();
  } else if (!gps.location.isValid()) {
    display_time();
  } else if (mode==3) {
    display_location();
  } else if (mode==4 && gps.altitude.isValid()) {
    display_altitude();
  } else {
    display_time();
  }
}

// Read GPS information.
// Return true if we have reliable information (all the data is valid for 10 rounds in a row)
bool gps_reader()
{
  // Count how many successful rounds we have
  static int gps_round = 0;

  while (ss.available() > 0) {
    char c = ss.read();
    // Serial.print(c);
    if (gps.encode(c)) {
      if (gps.location.isValid() && gps.time.isValid() && gps.date.isValid()) {
        gps_round++;
      } else {
        Serial.println("GPS data lost");
        gps_round = 0;
      }

      print_gps_info();

      if (gps_round > 10 && gps.altitude.isValid()) {
        // We believe the GPS data is reliable.
        return true;
      }
    }
  }

  return false;
}

// Handle GPS communication.
void update_gps_info()
{
  // If the time is not set yet - read the GPS data, and set the time when it is available.
  if (gps_info_set) {
    if (analogRead(KEYPin) < 100) {
      // Key is pressed, we start to read GPS data to set the time
      gps_info_set = false;
      // Set serial GPS module read speed.
      ss.begin(GPSBaud);
    }
  } else if (gps_reader()) {
    if (gps.location.lng()<0) {
      // Assume we are in Seattle.
      my_tz = &tz_us;
    }

    set_rtc_time();
    get_rtc_time();
    gps_info_set = true;
    ss.end();
  }
}

void loop()
{
  update_gps_info();

  if (millis()%100<10) {
    // Update the display string.
    update_display();
  }

  // Show the display digits.
  show_display_string();
}

// Print GPS info for debugging.
void print_gps_info()
{
  Serial.print("Location: ");
  if (gps.location.isValid()) {
    Serial.print(gps.location.lat(), 6);
    Serial.print(",");
    Serial.print(gps.location.lng(), 6);
  } else {
    Serial.print("INVALID");
  }

  Serial.print("Date/Time: ");
  if (gps.date.isValid()) {
    Serial.print(gps.date.month());
    Serial.print("/");
    Serial.print(gps.date.day());
    Serial.print("/");
    Serial.print(gps.date.year());
  } else {
    Serial.print("INVALID");
  }

  Serial.print(" ");
  if (gps.time.isValid()) {
    Serial.print(gps.time.hour());
    Serial.print(":");
    Serial.print(gps.time.minute());
    Serial.print(":");
    Serial.print(gps.time.second());
    Serial.print(".");
  } else {
    Serial.print("INVALID");
  }

  Serial.print(" ALT=");
  if (gps.altitude.isValid()) {
    Serial.print(gps.altitude.meters());
  } else {
    Serial.print("INVALID");
  }
  Serial.println();
}

// Print RTC time for debugging.
void get_rtc_time()
{
  bool century=false;
  bool h12, PM_time;

  Serial.print("LOCAL TIME:" );
  Serial.print(rtc.getHour(h12, PM_time));
  Serial.print(":");
  Serial.print(rtc.getMinute());
  Serial.print(":");
  Serial.print(rtc.getSecond());
  Serial.print(" ");
  Serial.print(rtc.getDate());
  Serial.print(" ");
  Serial.print(rtc.getMonth(century));
  Serial.print(" ");
  Serial.print(rtc.getYear()+RTC_BASE_YEAR);  // RTC year is one byte. Adjust it
  Serial.println();
}
