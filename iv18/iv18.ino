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

// The TinyGPSPlus object
TinyGPSPlus gps;

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

// Defult timezone is Finland. We will change it later, based on the GPS position.
Timezone *my_tz = &tz_fi;

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

  // Set serial GPS module read speed.
  ss.begin(GPSBaud);

  // Wire is used for DS3231.
  Wire.begin();

  // Turn on the display
  digitalWrite(BLANKPin, LOW);

  Serial.println("IV-18.ino started");
}

// Read GPS information.
// Return true if we have reliable information (all the data is valid for 10 rounds in a row)
bool gps_reader()
{
  // Count how many successful rounds we have
  static int gps_round = 0;

  if (ss.available() > 0) {
    if (gps.encode(ss.read())) {
      if (gps.location.isValid() && gps.time.isValid() && gps.date.isValid()) {
        gps_round++;
      } else {
        gps_round = 0;
      }

      print_gps_info();

      if (gps_round > 10) {
        // We beleive the GPS data is reiable.
        return true;
      }
    }
  }

  if (millis() > 5000 && gps.charsProcessed() < 10) {
    Serial.println("No GPS detected: check wiring.");
  }
  return false;
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

// What we show on display
char display_string[8] = "01234567 ";
// Display digital dots.
bool dots[8];

// Turn off all the dots.
void clear_dots()
{
  for (int i=0; i<8; i++) {
    dots[i] = false;
  }
}

// Show the contents of display_string on IV-18 display.
// Please reffer to MAX6921 documentation about how we send the data in 19-bit encoded strings.
void show_display_string()
{
  // In this order digits are sent to MAX6921.
  // The order is really all about hardware wiring.
  int display_order[] = { 6, 4, 2, 1, 0, 3, 5, 7 };

  // Loop through the display digits
  for (int i=0; i<8; i++) {
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

    // Next is the digit numer (decoded as 8-bit bit mask).
    for (int j=0; j<9; j++) {
      digitalWrite(CLKPin, LOW);
      digitalWrite(DINPin, j==i ? HIGH: LOW);
      digitalWrite(CLKPin, HIGH);
    }
    // Do not leave the CLK pin high.
    digitalWrite(CLKPin, LOW);

    // Shift the digit to dispaly.
    digitalWrite(LOADPin, HIGH);
    digitalWrite(LOADPin, LOW);

    // Delay to show the digit on display for short time.
    delay(1);
  }
}

// Set display_string to show the time from RTC
void display_time()
{
  bool h12, PM_time;
  int hour = rtc.getHour(h12, PM_time);

  if (hour>=10) {
    display_string[0] = hour/10+'0';
  } else {
    display_string[0] = 0; // Blank
  }
  display_string[1] = hour%10+'0';

  display_string[2] = '-';

  int minute = rtc.getMinute();
  display_string[3] = minute/10+'0';
  display_string[4] = minute%10+'0';

  display_string[5] = '-';

  int second = rtc.getSecond();
  display_string[6] = second/10+'0';
  display_string[7] = second%10+'0';
}

// Set display_string to show the time from RTC
void display_date()
{
  int date = rtc.getDate();
  if (date >= 10) {
    display_string[0] = date/10+'0';
  } else {
    display_string[0] = 0;
  }
  display_string[1] = date%10+'0';
  dots[1] = true;

  bool century=false;
  int month = rtc.getMonth(century);
  display_string[2] = month/10+'0';
  display_string[3] = month%10+'0';
  dots[3] = true;

  int year = rtc.getYear()+RTC_BASE_YEAR;
  display_string[4] = year/1000+'0';
  display_string[5] = (year/100)%10+'0';
  display_string[6] = (year/10)%10+'0';
  display_string[7] = year%10+'0';
}

// Set display_string to show the time location.
// Format is "L longitude latitude"
void display_location()
{
  display_string[0] = 'L';
  display_string[1] = ' ';

  int lng = gps.location.lng();
  int p = 2;

  if (lng<0) {
    // Show - sign befor the longitude.
    display_string[p] = '-';
    p++;
    lng = -lng;
  }

  if (lng>100) {
     display_string[p] = '1';
     p++;
     lng -= 100;
  }

  display_string[p] = lng/10+'0';
  p++;
  display_string[p] = lng%10+'0';
  p++;

  display_string[p] = ' ';
  p++;

  int lat=gps.location.lat();

  if (lat<0) {
    // Show - sign befor the longitude.
    display_string[p] = '-';
    p++;
    lng = -lng;
  }

  if (p<7) {
    display_string[p] = lng/10+'0';
    p++;
  }
  if (p<7) {
    display_string[p] = lng%10+'0';
    p++;
  }

  for (;p<8;p++) {
    display_string[p] = ' ';
  }
}

void display_temp()
{
  int temp = rtc.getTemperature();
  int p = 0;

  display_string[p] = ' ';
  p++;

  if (temp<0) {
    temp = -temp;
    display_string[p] = '-';
    p++;
  }
  display_string[p] = temp/10+'0';
  p++;
  display_string[p] = temp%10+'0';
  p++;

  display_string[p] = ' ';
  p++;
  display_string[p] = 'o';
  p++;
  display_string[p] = 'C';
  p++;

  for (;p<8;p++) {
    display_string[p] = ' ';
  }
}

void display_altitude()
{
  display_string[0] = 'A';
  display_string[1] = ' ';

  for (int i=2; i<8; i++) {
    display_string[i] = ' ';
  }

  int alt = gps.location.lng();
  sprintf(&display_string[2], "%4d", alt);
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

  // Save the fime to rtc.
  rtc.setYear(year()-RTC_BASE_YEAR);  // RTC year is one byte. Adjust it.
  rtc.setMonth(month());
  rtc.setDate(day());
  rtc.setHour(hour());
  rtc.setMinute(minute());
  rtc.setSecond(second());
}

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

void update_display()
{
  for (int i=0; i<8; i++) {
    display_string[i] = 0;
    dots[i] = false;
  }
  switch ((now()/2)%10) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
        display_time();
        break;
    case 5:
    case 6:
        display_date();
        break;
    case 7:
        display_temp();
        break;
    case 8:
        display_location();
        break;
    case 9:
        display_altitude();
  }
}

void loop()
{
  static bool time_set = false;

  // If the time is not set yet - read the GPS data, and set the time when it is available.
  if (!time_set) {
    if (gps_reader()) {
      if (gps.location.lng()<0) {
        // Assume we are in Seattle.
        my_tz = &tz_us;
      }

      set_rtc_time();
      get_rtc_time();
      time_set = true;

      // Interrupts initiated from GPS cause display to balnk for short time.
      detachInterrupt(RXPin);
      detachInterrupt(TXPin);
    }  
  }

  if (millis()%100<10) {
    update_display();
  }

  // Update the dispplay
  show_display_string();
}

void print_gps_info()
{
  Serial.print("Location: ");
  if (gps.location.isValid()) {
    Serial.print(gps.location.lat(), 6);
    Serial.print(F(","));
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
  Serial.println();
}
