// IV-18 based clock.
// Full project description: https://github.com/alexander-krotov/IV18-clock

// Use Board: ESP32C3 version

#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <DS3231.h>
#include <WiFiManager.h>
#include <EEPROM.h>
#include <TimeLib.h>
#include <WiFiUdp.h>
#include <GyverPortal.h>
#include <SPI.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

#include "api-keys.h" // timezonedb_api_key

// GPS serial pins - Note: RXD2 is connected to GPS TX, TXD2 is connected to GPS RX (crossed)
static const int RXD2 = 20;
static const int TXD2 = 21;
static const uint32_t GPS_BAUD = 9600;

// MAX6921 pins.
static const int BLANKPin = 1;
static const int CLKPin = 2;
static const int LOADPin = 3;
static const int DINPin = 4;

// DS3231 pins
static const int SDA_PIN = 8;
static const int SCL_PIN = 9;

// GPS data parser - processes NMEA sentences from GPS module
TinyGPSPlus gps;


// Flag indicating GPS has provided valid data at least once
// Used to conditionally display GPS-dependent information
int  gps_info_set;

// Hardware serial connection to GPS module (UART 1)
// GPS module outputs NMEA protocol sentences at 9600 baud
HardwareSerial gpsSerial(1);

// Number of minutes per hour.
const int MINS_PER_HOUR = 60;

// Display configuration - controls clock behavior and display modes
// These values are persisted in EEPROM and configurable via web UI
char ntpServerName[80] = "fi.pool.ntp.org";
int16_t  clock_tz = 2*MINS_PER_HOUR; // Timezone shift (could be negative) in minutes
unsigned char clock_12 = 0;  // If non-zero clock is 12h, otherwise 24h
unsigned char clock_leading_0;  // Show hour leading 0
unsigned char clock_bar_mode = 0;   // bar mode
unsigned char clock_use_ntp = true;  // Use NTP switch
unsigned char clock_use_rtc = true;  // Use RTC switch
unsigned char clock_use_gps = true; // Use GPS time source
unsigned char clock_show_temp = false; // Show the temperature

// Clock EEPROM data address.
const int eeprom_addr=12;

// If we do not have WiFi we wait 60 seconds in the configuration portal.
const int WIFI_MANAGER_TIMEOUT=60;

// Web interface
GyverPortal ui;

// RTC chip.
DS3231 rtc;
// RTC year is one byte. We need to adjust it.
const int RTC_BASE_YEAR = 1900;

// Our fake TZ.
// It does not relly work in ESP32 environment, but still needed for the standard
// functions as a parameter.
struct timezone tz = {0, 0};

// IV-18 display size
const int display_size = 8;
// The actual display bits (in format expected by MAX6921 via SPI).
// We set it in the main loop, but display is running in a dedicated thread
// to smoother display update.
uint32_t display_bits[display_size];

// Global variable for the display task handle
TaskHandle_t displayTaskHandle;

// Characters we can display on 7-segment indicator.
enum display_char {
  CHAR_0, CHAR_1, CHAR_2, CHAR_3, CHAR_4, CHAR_5, CHAR_6, CHAR_7, CHAR_8, CHAR_9,
  CHAR_BLANK, CHAR_MINUS, CHAR_P, CHAR_C, CHAR_L, CHAR_o, CHAR_A, CHAR_E
};

// Initialize the network (Wi-Fi connection).
bool initialize_network()
{
  WiFiManager wm;
  // wm.resetSettings();

  // Automatically connect using saved credentials,
  // if connection fails, it starts an access point with the name "NixieClock".
  wm.setConfigPortalTimeout(WIFI_MANAGER_TIMEOUT);
  bool res = wm.autoConnect("NixieClock");
  wm.stopWebPortal();

  return res;
}

// Read the config data from EEPROM.
void read_eeprom_data()
{
  // Read the EEPROM settings.
  clock_tz = EEPROM.readShort(eeprom_addr);
  clock_12 = EEPROM.read(eeprom_addr+2);
  clock_leading_0 = EEPROM.read(eeprom_addr+3);
  clock_bar_mode = EEPROM.read(eeprom_addr+4);
  clock_use_ntp = EEPROM.read(eeprom_addr+5);
  clock_use_rtc = EEPROM.read(eeprom_addr+6);
  clock_use_gps = EEPROM.read(eeprom_addr+7);
  clock_show_temp = EEPROM.read(eeprom_addr+8);
  EEPROM.readString(eeprom_addr+9, ntpServerName, sizeof(ntpServerName)-1);
}

// Write the config data to EEPROM.
void write_eeprom_data()
{
  EEPROM.writeShort(eeprom_addr, clock_tz);
  EEPROM.write(eeprom_addr+2, clock_12);
  EEPROM.write(eeprom_addr+3, clock_leading_0);
  EEPROM.write(eeprom_addr+4, clock_bar_mode);
  EEPROM.write(eeprom_addr+5, clock_use_ntp);
  EEPROM.write(eeprom_addr+6, clock_use_rtc);
  EEPROM.write(eeprom_addr+7, clock_use_gps);
  EEPROM.write(eeprom_addr+8, clock_show_temp);
  EEPROM.writeString(eeprom_addr+9, ntpServerName);
  EEPROM.commit();
}

// Display string as running display string across the digits
void run_string_on_display(const char *str)
{
    int len = strlen(str);
    // What we show on display
    char display_string[display_size+1] = { 0 };
    // Display digital dots.
    bool dots[display_size];

    log_printf("run_string_on_display: str=%s len=%d\n", str, len);

    // Scroll string over display
    for (int s=-display_size; s<=len; s++) {
      for (int i=0; i<display_size; i++) {
        dots[i] = false;
        display_string[i] = ' ';
        if (i+s<0 || i+s>=len) {
          // Out of string or out of display.
        } else if (str[i+s]>='0' && str[i+s]<='9') {
          // character fits to the display and is between 0 and 9.
          display_string[i] = str[i+s];
        } else if (str[i+s]=='.') {
          dots[i] = true;
        }
      }

      // Scroll the strings on display with 5 char/sec speed.
      update_display_string(display_string, dots);
      delay(200);
    }
}

// Set the string to display (display_string) with decimal dots (dots array).
// Function encodeds the data to display_bits array, and later used in
// show_display_string_task.
void update_display_string(const char display_string[], const bool dots[])
{
  // In this order digits are sent to MAX6921.
  // The order is really all about hardware wiring.
  static const int display_order[] = { 6, 4, 2, 1, 0, 3, 5, 7 };

  for (int i=0; i<display_size; i++) {
    // Take a digit in display order.
    int c = display_string[display_order[i]];

    // Character encoding for 7-segment display.
    int bits = get_char_bits(c);

    // Prepare 20-bit data to send (10 bits char + 1 bit DP + 9 bits digit select)
    uint32_t data = 0;

    // First 10 bits: encoded char bits (highest first)
    data = (bits & 0x3FF) << 10;

    // Next 1 bit: decimal point (DP) bit
    if (dots[display_order[i]]) {
      data |= (1 << 9);
    }

    if (data != 0) {
      // Last 9 bits: digit number (decoded as 8-bit bit mask)
      data |= (1 << (8 - i));
    }

    // Store it in the buffer, used by display thread.
    display_bits[i] = data;
  }
}

// FreeRTOS task - continuously refreshes IV-18 display
// Multiplexes 8 digits with ~2ms refresh rate per digit
// Higher refresh rate dims display, lower causes flicker
// Reads display_bits[] array which is updated by main thread
// Note: Uses simple synchronization - occasional display glitches are acceptable
// Please refer to MAX6921 documentation about how we send the data in 19-bit encoded strings.
void show_display_string_task(void *parameter)
{
  // Enable display output (active LOW on MAX6921)
  digitalWrite(BLANKPin, LOW);

  // Loop through the display digits
  for (int i=0; ; i++) {
    // Wrap around to first digit after showing all 8
    if (i == display_size) {
      i = 0;
    }

    // Read the current encoded digit.
    // Note, this read formally is not thread-safe,
    // but incorrecly shown digit will be corrected in few milliseconds.
    uint32_t data = display_bits[i];

    if (data==0) {
      // Skip display of blank digits (all segments off)
      continue;
    }
    // Send 20-bit digit data to MAX6921 via SPI
    // Format: [8-bit upper][8-bit middle][8-bit lower]
    SPI.transfer((data >> 16) & 0xFF);
    SPI.transfer((data >> 8) & 0xFF);
    SPI.transfer(data & 0xFF);

    // Shift the digit to display.
    digitalWrite(LOADPin, HIGH);
    digitalWrite(LOADPin, LOW);

    // Latch digit into display (rising edge triggers output)
    digitalWrite(LOADPin, HIGH);
    digitalWrite(LOADPin, LOW);

    // Hold for 2ms: empirically determined balance between brightness and flicker
    // <2ms: dimmed display  |  >2ms: noticeable flicker
    // Might nieed adjustments for specific VFD.
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void setup()
{
  // Serial Monitor
  Serial.begin(115200);

  // Communicate with MAX6921 using this pins.
  pinMode(BLANKPin, OUTPUT);
  pinMode(CLKPin, OUTPUT);
  pinMode(LOADPin, OUTPUT);
  pinMode(DINPin, OUTPUT);

  // Wire is used for DS3231.
  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, INPUT_PULLUP);

  Wire.begin(SDA_PIN, SCL_PIN);

  // Initialize SPI
  SPI.begin(CLKPin, -1, DINPin);
  SPI.setFrequency(5000000);  // 5 MHz frequency
  SPI.setDataMode(SPI_MODE0);
  SPI.setBitOrder(MSBFIRST);

  EEPROM.begin(100);

  // Read settings from EEPROM
  read_eeprom_data();

  // Start Serial 2 with the defined RX and TX pins and a baud rate of 9600
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, RXD2, TXD2);
  Serial.println("Serial started at baud rate");

  // Turn off the display
  digitalWrite(BLANKPin, HIGH);

  set_time_from_rtc();

  // Create FreeRTOS task for showing display string
  xTaskCreate(show_display_string_task, "DisplayTask", 2048, NULL, 1, &displayTaskHandle);

  // Initialize network and UI
  if (initialize_network()) {
    IPAddress myIP = WiFi.localIP();
    String ip_addr_str = myIP.toString();
    log_printf("AP IP address: %s\n", ip_addr_str.c_str());
    run_string_on_display(ip_addr_str.c_str());

    // Start server portal
    ui.attachBuild(build);
    ui.attach(action);
    ui.start();
    log_printf("setup ui started\n");

    // Fetch NTP time if enabled
    if (clock_use_ntp) {
      get_ntp_time();
    }
  }

#if 0
  // Setup display refresh timer (4 times/sec).
  // Disabeld for this project because we cannot use localtime() inside
  // ISR timer function.
  // We need localtime to show the date, time could be printed without.
  static hw_timer_t * Timer0_Cfg = timerBegin(1000);
  if (Timer0_Cfg) {
    log_printf("Timer setup\n");
    timerAttachInterrupt(Timer0_Cfg, &Timer0_ISR);
    timerAlarm(Timer0_Cfg, 250, true, 0);
#endif

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

  // Indices match the display_char enum for direct lookup
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

// Set clock time to H:M:S
void set_clock_time(unsigned int h, unsigned int m, unsigned int s)
{
  log_printf("set time: %02u:%02u:%02u\n", h, m, s);

  // Check time sanity. Uninitialized RTC might give strange values.
  if (h<24 && m<60 && s<60) {
    struct timeval tv = {0};
    gettimeofday(&tv, &tz);
    tv.tv_sec -= tv.tv_sec%(24*60*60);
    tv.tv_sec += h*60*60+m*60+s;
    // Set current time
    settimeofday(&tv, &tz);
  }
}

// Read time from rtc and set it to the arduino system time
void set_time_from_rtc()
{
  bool h12Flag;
  bool pmFlag;
  unsigned int h = rtc.getHour(h12Flag, pmFlag);
  unsigned int m = rtc.getMinute();
  unsigned int s = rtc.getSecond();

  log_printf("set time from RTC: %02u:%02u:%02u\n", h, m, s);

  // Check time for sanity.
  if (h<24 && m<60 && s<60) {
    bool century;
    struct tm tm = { .tm_sec=s, .tm_min=m, .tm_hour=h, .tm_mday=rtc.getDate(), .tm_mon=rtc.getMonth(century)-1, .tm_year = rtc.getYear()+RTC_BASE_YEAR };
    log_printf("set date from RTC: %02u-%02u-%04u TZ=%d\n", tm.tm_mday, tm.tm_mon+1, tm.tm_year, clock_tz);

    struct timeval tv = { .tv_sec = mktime(&tm)+clock_tz*60, .tv_usec = 0 };
    settimeofday(&tv, &tz);
  }
}

// Set display_string to show the time from RTC
void display_time(char display_string[], bool dots[])
{
  struct timeval tv;
  gettimeofday(&tv, &tz);
  tm *ttm = localtime(&tv.tv_sec);

  snprintf(display_string, display_size+1, clock_leading_0 ? "%02d %02d %02d": "%2d %02d %02d",
           ttm->tm_hour, ttm->tm_min, ttm->tm_sec);

  if (clock_bar_mode == 0) {
  } else if (clock_bar_mode == 1) {
    if (tv.tv_usec >= 500000) {
      display_string[2] = '-';
    } else {
      display_string[5] = '-';
    }
  } else if ((clock_bar_mode == 2 && tv.tv_usec >= 500000) || clock_bar_mode == 3) {
    display_string[2] = display_string[5] = '-';
  }
}

// Set display_string to show the date from RTC
void display_date(char display_string[], bool dots[])
{
  time_t t = time(NULL);
  tm *ttm = localtime(&t);

  snprintf(display_string, display_size+1, "%2d%02d%4d", ttm->tm_mday, ttm->tm_mon+1, ttm->tm_year);
  dots[1] = dots[3] = true;
}

// Set display_string to show the GPS location.
// Format is "L longitude latitude", including possible - sign.
// Shows the values with one degree precision.
void display_location(char display_string[], bool dots[])
{
  snprintf(display_string, display_size+1, "L %3d %d3", (int)gps.location.lng(), (int)gps.location.lat());
}

// Set display_string to show the temperature.
// DS3231 has a built-in temperature sensor.
void display_temp(char display_string[], bool dots[])
{
  snprintf(display_string, display_size+1, "%4d oC", (int)rtc.getTemperature());
}

// Set display_string to show the GPS altitude.
void display_altitude(char display_string[], bool dots[])
{
  int alt_cm = gps.altitude.meters()*100;

  // Altitude cell part
  int c=alt_cm/100;
  // Altitude fraction part
  int p = alt_cm>0 ? alt_cm%100: (-alt_cm)%100;

  snprintf(display_string, display_size+1, "A %4d%02d", c, p);
  dots[5] = true;
}

// Timer function to update the display string.
// Update display with current information in rotating sequence
// Cycles through: temperature → date → GPS location (if valid) → altitude → time
// Each information shown for ~2 seconds (10 modes × 2 seconds = 20 second cycle)
void update_display()
{
  // Blank everything.
  char display_string[display_size+1] = { 0 };
  bool dots[display_size+1] = { 0 };

  // Display mode.
  // In 20seconds loop show the time, date, temperature, and if available
  // show the location and altitude.
  int mode = (time(NULL)/2)%10;

  // Display rotation sequence:
  // Mode 0:     Temperature (from RTC sensor)
  // Mode 1-2:   Date (shown twice for 4 seconds total)
  // Mode 3:     GPS latitude/longitude (if GPS fix obtained)
  // Mode 4:     GPS altitude (if GPS altitude valid)
  // Mode 5-9:   Time (default fallback)
  if (mode == 0 && clock_show_temp) {
    display_temp(display_string, dots);
  } else if (mode==1 || mode == 2) {
    display_date(display_string, dots);
  } else if (mode==3 && gps_info_set) {
    display_location(display_string, dots);
  } else if (mode==4 && gps.altitude.isValid()) {
    display_altitude(display_string, dots);
  } else {
    display_time(display_string, dots);
  }

  update_display_string(display_string, dots);
}

// Read GPS information.
// Process incoming GPS data stream and validate quality
// Returns true only when GPS has provided 10+ consecutive valid reads
// (Indicates stable position lock and reliable data)
// 
// GPS receivers often output invalid data before achieving lock:
// - Partial NMEA sentences (incomplete data)
// - Invalid coordinates (0,0) or future dates
// 
// Requiring 10 consecutive valid reads ensures data reliability
bool gps_reader()
{
  // Count how many successful rounds we have
  static unsigned int gps_round = 0;

  while (gpsSerial.available() > 0) {
    char c = gpsSerial.read();
    if (gps.encode(c)) {
      // Check if all essential fields are valid (location, time, date)
      // Altitude is NOT required for this check (validated separately)
      if (gps.location.isValid() && gps.time.isValid() && gps.date.isValid()) {
        gps_round++;
      } else {
        // Any invalid field resets the counter - start over
        gps_round = 0;
      }

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
  // Read the GPS data, and set the time when it is available.
  if (gps_reader()) {
    // Sometimes (every few days), get the GPS info to RTC.
    if (gps_info_set % (1024*1024) == 0) {
      print_gps_info();
      set_gps_time(); // Take GPS time to RTC
      update_timezone_from_gps(); // Timezone info (based on GPS coordinates to clock_tz)
      set_time_from_rtc(); // Set the time fomr RTC (that includes timezone correction).
    }
    gps_info_set++;
  }
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

  Serial.print(" Date/Time: ");
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

// Set the GPS date and tiem to RTC.
void set_gps_time()
{
  if (gps.time.isValid()) {
    rtc.setHour(gps.time.hour());
    rtc.setMinute(gps.time.minute());
    rtc.setSecond(gps.time.second());
  }

  if (gps.date.isValid()) {
    rtc.setMonth(gps.date.month());
    rtc.setDate(gps.date.day());
    rtc.setYear(gps.date.year() - RTC_BASE_YEAR);
  }
}

// Periodically query TimezoneDB API to determine timezone from GPS location
// Called approximately every 1 million GPS updates (~24+ hours)
// Updates system timezone if GPS location moves to different zone
// 
// Requires: timezonedb_api_key defined in api-keys.h
//          WiFi connectivity
//          GPS lock with valid coordinates
int getTimezoneFromGPS()
{
  // Check if we have valid GPS coordinates
  if (!gps.location.isValid()) {
    log_printf("GPS location not valid\n");
    return INT_MIN;
  }

  // Check if API key is set
  if (strlen(timezonedb_api_key) == 0) {
    log_printf("TimezoneDB API key not set\n");
    return INT_MIN;
  }

  HTTPClient http;

  // Build the URL with GPS coordinates
  char url[256];
  snprintf(url, sizeof(url),
    "http://api.timezonedb.com/v2.1/get-time-zone?key=%s&format=json&by=position&lat=%f&lng=%f",
    timezonedb_api_key, gps.location.lat(), gps.location.lng());

  log_printf("Requesting timezone from: %s\n", url);

  http.begin(url);
  int httpResponseCode = http.GET();

  if (httpResponseCode != 200) {
    log_printf("HTTP error code: %d\n", httpResponseCode);
    http.end();
    return INT_MIN;
  }

  String payload = http.getString();
  http.end();

  // Parse JSON response
  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    log_printf("JSON parsing failed: %s\n", error.c_str());
    return INT_MIN;
  }

  // Extract timezone offset
  if (doc.containsKey("gmtOffset")) {
    int gmtOffset = doc["gmtOffset"]; // Offset in seconds
    int tzMinutes = gmtOffset / 60;   // Convert to minutes

    log_printf("Timezone offset: %d minutes\n", tzMinutes);
    return tzMinutes;
  }

  log_printf("gmtOffset not found in response\n");
  return INT_MIN;
}

// Periodically update timezone from GPS
void update_timezone_from_gps()
{
  int tz_offset = getTimezoneFromGPS();

  if (tz_offset != INT_MIN) {
    if (tz_offset != clock_tz) {
      log_printf("Updating timezone from %d to %d\n", clock_tz, tz_offset);
      clock_tz = tz_offset;
      write_eeprom_data();

      // Update system time with new timezone
      if (clock_use_rtc) {
        set_time_from_rtc();
      }
    }
  }
}

// Sync system time from RTC approximately every 1000+ seconds (~20 minutes)
// Ensures system clock stays aligned with hardware RTC despite drift
// Counter-intuitive: time(NULL) doesn't start at 0 after every power cycle
void update_time_from_rtc()
{
  static time_t last_update_from_rtc;
  if (last_update_from_rtc==0 || time(NULL) > last_update_from_rtc+1000) {
    last_update_from_rtc = time(NULL);
    set_time_from_rtc();      // Sync system time from RTC (applies timezone)
    print_rtc_time();         // Debug output to serial
  }
}

void loop()
{
  // Process GPS data stream continuously (if GPS mode enabled)
  // Updates gps_info_set flag and may trigger timezone/time updates
  if (clock_use_gps) {
    update_gps_info();
  }

  update_time_from_rtc();

  // Update display every 128 loop iterations
  // With WiFi/UI overhead, this provides ~1-2 Hz display refresh rate
  // Fast enough to appear smooth while not overloading the CPU
  static int i;
  if (i%128==0) {
    update_display();
  }
  i++;

  ui.tick();
}

// Print RTC time for debugging.
void print_rtc_time()
{
  bool century=false;
  bool h12, PM_time;

  Serial.print("RTC time:" );
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

// NTP support code (adapted from TimeNTP sample)
WiFiUDP Udp;
unsigned int localPort = 8888;  // local port to listen for UDP packets
const int NTP_PACKET_SIZE = 48; // NTP time is in the first 48 bytes of message
byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming & outgoing packets

// Get current time from NTP server
time_t get_ntp_time()
{
  IPAddress ntpServerIP; // NTP server's IP address

  while (Udp.parsePacket() > 0) ; // discard any previously received packets
  WiFi.hostByName(ntpServerName, ntpServerIP);
  log_printf("Transmit NTP Request: %s\n", ntpServerName);
  // get a random server from the pool
  sendNTPpacket(ntpServerIP);
  uint32_t beginWait = millis();
  while (millis() - beginWait < 1500) {
    int size = Udp.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      Udp.read(packetBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
      time_t secsSince1900;
      // convert four bytes starting at location 40 to a long integer
      secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
      secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
      secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
      secsSince1900 |= (unsigned long)packetBuffer[43];
      // Convert NTP time to UNIX time.
      secsSince1900 = secsSince1900 - 2208988800UL;

      log_printf("Receive NTP Response %lu\n", (unsigned long)secsSince1900);

      // Update RTC and system time with received NTP time
      tm *ttm = localtime(&secsSince1900);
      rtc.setSecond(ttm->tm_sec);
      rtc.setMinute(ttm->tm_min);
      rtc.setHour(ttm->tm_hour);

      log_printf("Receive NTP Response date: %d-%d-%d\n", ttm->tm_mday, ttm->tm_mon+1, ttm->tm_year);

      rtc.setDate(ttm->tm_mday);    // Day range in tm: 1-31, in RTC it is the same
      rtc.setMonth(ttm->tm_mon+1);  // Month range in tm: 0-11, in RTC: 1-12
      rtc.setYear(ttm->tm_year);    // Year since (RTC_BASE_YEAR)

      // Set the system time from tmm, including timezoene correction.
      struct timeval tv = { .tv_sec = mktime(ttm)+clock_tz*60, .tv_usec = 0 };
      settimeofday(&tv, &tz);

      return secsSince1900;
    }
  }
  log_printf("No NTP Response :-(\n");
  return 0;
}

// Send an NTP request to the time server at the given address
void sendNTPpacket(IPAddress &address)
{
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;
  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  Udp.beginPacket(address, 123); //NTP requests are to port 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
}

// Create a configuration form for the web UI
// Contains two main tabs: Clock config (timezone, modes, time sources)
// and Time (manual time setting)
void build()
{
  log_printf("BUILD\n");

  GP.BUILD_BEGIN();
  GP.PAGE_TITLE("Nixie clock");

  GP.THEME(GP_DARK);
  GP.FORM_BEGIN("/update");

  // Main config block with timezone, brightness, modes, etc.
  GP_MAKE_BLOCK_TAB(
    "Clock config",
    GP_MAKE_BOX(GP.LABEL("TimeZone shift:"); GP.NUMBER("clock_tz", "", clock_tz););
    GP_MAKE_BOX(GP.LABEL("12/24 mode"); GP.SWITCH("clock_12", clock_12 ? true: false););
    GP_MAKE_BOX(GP.LABEL("Leading 0"); GP.SWITCH("clock_leading_0", clock_leading_0 ? true: false, 0););
    GP_MAKE_BOX(GP.LABEL("Bar mode"); GP.SELECT("clock_bar_mode", "Always off, Async, Sync, Always on", clock_bar_mode););
    GP_MAKE_BOX(GP.LABEL("Use NTP"); GP.SWITCH("clock_use_ntp", clock_use_ntp ? true: false););
    GP_MAKE_BOX(GP.LABEL("Use RTC"); GP.SWITCH("clock_use_rtc", clock_use_rtc ? true: false, 0););
    GP_MAKE_BOX(GP.LABEL("Use GPS"); GP.SWITCH("clock_use_gps", clock_use_gps ? true: false, 0););
    GP_MAKE_BOX(GP.LABEL("Show temperatue"); GP.SWITCH("clock_show_temp", clock_show_temp ? true: false, 0););
    GP_MAKE_BOX(GP.LABEL("NTP Server name: "); GP.TEXT("clock_ntp_server", "local NTP server if you have", ntpServerName, "", sizeof(ntpServerName)-1););
  );
  GP.SUBMIT("UPDATE");

  GP.FORM_END();

  // Time setting form
  GP.FORM_BEGIN("/settime");

  // Get current time for the form
  time_t t = time(NULL);
  tm *ttm = localtime(&t);
  rtc.setSecond(ttm->tm_sec);
  rtc.setMinute(ttm->tm_min);
  rtc.setHour(ttm->tm_hour);

  GPtime gptime (ttm->tm_hour, ttm->tm_min, ttm->tm_sec);

  GP_MAKE_BLOCK_TAB(
    "Time",
    GP_MAKE_BOX(GP.LABEL("Time :"); GP.TIME("time", gptime););
  );
  GP.SUBMIT("SET TIME");

  GP.FORM_END();

  GP.BUILD_END();
}

// Handle actions from GyverPortal web UI
void action(GyverPortal& p)
{
  log_printf("ACTION\n");

  // Handle config update form
  if (p.form("/update")) {
    int n;
    bool update_time = false;

    log_printf("ACTION update\n");

    // Read the new values, and check them for sanity.
    n = ui.getInt("clock_tz");
    if (n>=-20*MINS_PER_HOUR && n<=20*MINS_PER_HOUR) {
      if (n!=clock_tz) {
        update_time = true;
      }
      clock_tz = n;
    }

    n = ui.getBool("clock_12");
    if (n>=0 && n<=1) {
      clock_12 = n;
    }

    n = ui.getBool("clock_leading_0");
    if (n>=0 && n<=1) {
      clock_leading_0 = n;
    }

    n = ui.getInt("clock_bar_mode");
    if (n>=0 && n<4) {
      clock_bar_mode = n;
    }

    n = ui.getBool("clock_use_ntp");
    if (n>=0 && n<=1) {
      clock_use_ntp = n;
    }

    n = ui.getBool("clock_use_rtc");
    if (n>=0 && n<=1) {
      clock_use_rtc = n;
    }

    n = ui.getBool("clock_use_gps");
    if (n>=0 && n<=1) {
      clock_use_gps = n;
    }

    n = ui.getBool("clock_show_temp");
    if (n>=0 && n<=1) {
      clock_show_temp = n;
    }

    String s = ui.getString("clock_ntp_server");
    if (s && strcmp(s.c_str(), ntpServerName) != 0) {
      strncpy(ntpServerName, s.c_str(), sizeof(ntpServerName)-1);
    }

    // Save new settings to EEPROM
    write_eeprom_data();

    // If timezone changed, update system time
    if (update_time) {
      if (clock_use_rtc) {
        set_time_from_rtc();
      } else if (clock_use_ntp) {
        get_ntp_time();
      }
    }
  }

  // Handle time setting form
  if (p.form("/settime")) {
    GPtime gptime = ui.getTime("time");
    log_printf("Action Settime: %d:%02d:%02d\n", gptime.hour, gptime.minute, gptime.second);
    set_clock_time(gptime.hour, gptime.minute, gptime.second);
  }
}
