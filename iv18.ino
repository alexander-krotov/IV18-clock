#include <TinyGPSPlus.h>
#include <SoftwareSerial.h>
#include <Timezone.h>

static const int RXPin = 2, TXPin = 3;
static const uint32_t GPSBaud = 9600;

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

Timezone *my_tz = &tz_fi;

void setup()
{
  Serial.begin(115200);
  ss.begin(GPSBaud);

  Serial.println("IV-18.ino started");
}

// Read GPS information.
// Return true if we have reliable information (all the data is valid for 10 rounds in a row)
bool gps_reader()
{
  // Ho many successful rounds we have
  static int gps_round = 0;

  while (ss.available() > 0) {
    if (gps.encode(ss.read())) {
      if (gps.location.isValid() && gps.time.isValid() && gps.date.isValid()) {
        gps_round++;
        print_info();
      } else {
        gps_round = 0;
      }

      if (gps_round > 10) {
        // We beleive the dta is reiable.
        return true;
      }
    }
  }

  if (millis() > 5000 && gps.charsProcessed() < 10) {
    Serial.println("No GPS detected: check wiring.");
  }
  return false;
}

void loop()
{
  static bool time_set = false;

  if (!time_set) {
    if (gps_reader()) {
      if (gps.location.lng()<0) {
        // Assume we are in Seattle.
        my_tz = &tz_us;
      }
      get_my_time();
      time_set = true;
    }
  }

  // get_my_time();
}

void get_my_time()
{
  // There must be some easier way to set the time form GPS.
  setTime(gps.time.hour(), gps.time.minute(), gps.time.second(), gps.date.day(), gps.date.month(), gps.date.year());
  time_t utc_time = now();
  time_t my_local_time = my_tz->toLocal(utc_time);
  setTime(my_local_time);

  Serial.print("LOCAL TIME:" );
  Serial.print(hour());
  Serial.print(":");
  Serial.print(minute());
  Serial.print(":");
  Serial.print(second());
  Serial.print(" ");
  Serial.print(day());
  Serial.print(" ");
  Serial.print(month());
  Serial.print(" ");
  Serial.print(year()); 
  Serial.println(); 
}

void print_info()
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
