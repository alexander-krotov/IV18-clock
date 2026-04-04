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

// GPS serial pins
static const int RXD2 = 21; // To TX
static const int TXD2 = 20; // To RX 
static const uint32_t GPS_BAUD = 9600;

// MAX6921 pins.
static const int BLANKPin = 1;
static const int CLKPin = 2;
static const int LOADPin = 3;
static const int DINPin = 4;

// DS3231 pins
static const int SDA_PIN = 8;
static const int SCL_PIN = 9;

// The TinyGPSPlus object
TinyGPSPlus gps;

// GPS info succesfluly read at lest once.
bool gps_info_set;

// The serial connection to the GPS device
HardwareSerial  gpsSerial(1);

// Clock global configuration.
char ntpServerName[80] = "fi.pool.ntp.org";
signed char clock_tz = 2; // Timezone shift (could be negative)
unsigned char clock_12 = 0;  // If non-zero clock is 12h, otherwise 24h
unsigned char clock_leading_0;  // Show hour leading 0
unsigned char clock_bar_mode = 0;   // bar mode
unsigned char clock_use_ntp = true;  // Use NTP switch
unsigned char clock_use_rtc = true;  // Use RTC switch
unsigned char clock_use_gps = true; // Use GPS time source 
unsigned char clock_show_sec = true; // Shiw the seconds (or keep 2 last digits blank) 

// Clock EEPROM data address.
const int eeprom_addr=12;

// If we do not have WiFi we wait 60 seconds in the configuration portal.
const int WIFI_MANAGER_TIMEOUT=60;

// Web interface
GyverPortal ui;

// RTC chip.
DS3231 rtc;
// RTC year is one byte. We need to adjust it.
const int RTC_BASE_YEAR = 2000;
// Temperature read from RTC
float clock_temp;

// Our fake TZ
struct timezone tz = {0, 0};

// IV-18 display size
const int display_size = 8;
// What we show on display
char display_string[display_size+1];
// Display digital dots.
bool dots[display_size];

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
  clock_tz = (signed char)EEPROM.read(eeprom_addr);
  clock_12 = EEPROM.read(eeprom_addr+2);
  clock_leading_0 = EEPROM.read(eeprom_addr+3);
  clock_bar_mode = EEPROM.read(eeprom_addr+4);
  clock_use_ntp = EEPROM.read(eeprom_addr+5);
  clock_use_rtc = EEPROM.read(eeprom_addr+6);
  clock_use_gps = EEPROM.read(eeprom_addr+7);
  clock_show_sec = EEPROM.read(eeprom_addr+8);
  EEPROM.readString(eeprom_addr+9, ntpServerName, sizeof(ntpServerName)-1);
  EEPROM.commit();
}

// Write the config data to EEPROM.
void write_eeprom_data()
{
  EEPROM.write(eeprom_addr, clock_tz);
  EEPROM.write(eeprom_addr+2, clock_12);
  EEPROM.write(eeprom_addr+3, clock_leading_0);
  EEPROM.write(eeprom_addr+4, clock_bar_mode);
  EEPROM.write(eeprom_addr+5, clock_use_ntp);
  EEPROM.write(eeprom_addr+6, clock_use_rtc);
  EEPROM.write(eeprom_addr+7, clock_use_gps);
  EEPROM.write(eeprom_addr+8,clock_show_sec);
  EEPROM.writeString(eeprom_addr+9, ntpServerName);
  EEPROM.commit();
}

// Display string as running display string across the digits
void run_string_on_display(const char *str)
{
    int len = strlen(str);

    log_printf("run_string_on_display: str=%s len=%d\n", str, len);

    // Scroll string over display
    for (int s=-display_size; s<=len; s++) {
      for (int i=0; i<display_size; i++) {
        if (i+s>=0 && i+s<len && str[i+s]>='0' && str[i+s]<='9') {
          // character fits to the display and is between 0 and 9.
          display_string[i] = str[i+s];
        } else {
          display_string[i] = ' ';
        }
        dots[i] = str[i+s] == '.';
      }

      // Scroll the strings on display with 5 char/sec speed.
      delay(200);
    }
}

// Function to show display string in a FreeRTOS task.
// Show the contents of display_string on IV-18 display.
// Please refer to MAX6921 documentation about how we send the data in 19-bit encoded strings.
void show_display_string_task(void *parameter)
{
  // In this order digits are sent to MAX6921.
  // The order is really all about hardware wiring.
  static const int display_order[] = { 6, 4, 2, 1, 0, 3, 5, 7 };

  // Turn on the display
  digitalWrite(BLANKPin, LOW);

  // Loop through the display digits
  for (int i=0; ; i++) {
    // In this infinite loop we get back to the first digit.
    if (i == display_size) {
      i = 0;
    }

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
    
    // Last 9 bits: digit number (decoded as 8-bit bit mask)
    data |= (1 << (8 - i));

    // Send 20 bits via SPI
    SPI.transfer((data >> 16) & 0xFF);
    SPI.transfer((data >> 8) & 0xFF);
    SPI.transfer(data & 0xFF);

    // Shift the digit to display.
    digitalWrite(LOADPin, HIGH);
    digitalWrite(LOADPin, LOW);

    // 2ms is sort of magic value: less - and the digits are dimmed,
    // more - and it starts to flicker.
    vTaskDelay(pdMS_TO_TICKS(2)); // Adjust the delay as necessary
  }
}

void setup()
{
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
  SPI.setClockDivider(SPI_CLOCK_DIV2);
  SPI.setDataMode(SPI_MODE0);
  SPI.setBitOrder(MSBFIRST);

  // Serial Monitor
  Serial.begin(115200);

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
      getNtpTime();
    }
  }

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
    struct tm tm = { .tm_sec=s, .tm_min=m, .tm_hour=h, .tm_mday=rtc.getDate(), .tm_mon=rtc.getMonth(century), .tm_year = rtc.getYear()+1900 };
    log_printf("set date from RTC: %02u-%02u-%04u\n", tm.tm_mday, tm.tm_mon, tm.tm_year);

    struct timeval tv = { .tv_sec = mktime(&tm), .tv_usec = 0 };
    settimeofday(&tv, &tz);

    clock_temp = rtc.getTemperature();
  }
}

// Set display_string to show the time from RTC
void display_time()
{
  time_t t = time(NULL);
  tm *ttm = localtime(&t);

  snprintf(display_string, sizeof(display_string), "%2d %02d %02d", ttm->tm_hour, ttm->tm_min, ttm->tm_sec);
  if (clock_bar_mode == 0) {
  } else if (clock_bar_mode == 1) {
    if (millis()%1000<500) {
      display_string[2] = '-';
    } else {
      display_string[5] = '-';
    }
  } else if ((clock_bar_mode == 2 && millis()%1000<500) || clock_bar_mode == 3) {
    display_string[2] = display_string[5] = '-';
  }
}

// Set display_string to show the date from RTC
void display_date()
{
  time_t t = time(NULL);
  tm *ttm = localtime(&t);

  snprintf(display_string, sizeof(display_string), "%2d%02d%4d", ttm->tm_mday, ttm->tm_mon+1, ttm->tm_year);
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
  snprintf(display_string, sizeof(display_string), "%4d oC", (int)clock_temp);
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
  // Get current time for the form
  time_t t = time(NULL);
  tm *ttm = localtime(&t);
  rtc.setClockMode(false);  // set to 24h
  rtc.setSecond(ttm->tm_sec);
  rtc.setMinute(ttm->tm_min);
  rtc.setHour(ttm->tm_hour);
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
  int mode = (time(NULL)/2)%10;
  
  // In a loop show what we know: date, time, gps location, altitude, temperature.
  if (mode==0) {
    display_temp();
  } else if (mode==1 || mode == 2) {
    display_date();
  } else if (!gps_info_set) {
    display_time();
  } else if (mode==3) {
    display_location();
  } else if (gps.altitude.isValid()) {
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

  while (gpsSerial.available() > 0) {
    char c = gpsSerial.read();
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
  if (gps_reader()) {
    set_rtc_time();
    print_rtc_time();
    gps_info_set = true;
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

void loop()
{
  if (clock_use_gps) {
    update_gps_info();
  }

  if (millis()%100<10) {
    // Update the display string.
    update_display();
  }

  static time_t last_update_from_trc;
  if (time(NULL) > last_update_from_trc+1000) {
    // Every ~20 minutes sync time from RTC.
    last_update_from_trc = time(NULL);
    set_time_from_rtc();
    print_rtc_time();
  }

  ui.tick();
}

// Print RTC time for debugging.
void print_rtc_time()
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

// NTP support code (adapted from TimeNTP sample)
WiFiUDP Udp;
unsigned int localPort = 8888;  // local port to listen for UDP packets
const int NTP_PACKET_SIZE = 48; // NTP time is in the first 48 bytes of message
byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming & outgoing packets

// Get current time from NTP server
time_t getNtpTime()
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
      // Convert NTP time to UNIX time and apply timezone offset
      secsSince1900 = secsSince1900 - 2208988800UL + clock_tz * SECS_PER_HOUR;

      log_printf("Receive NTP Response %lu\n", (unsigned long)secsSince1900);

      // Update RTC and system time with received NTP time
      tm *ttm = localtime(&secsSince1900);
      rtc.setSecond(ttm->tm_sec);
      rtc.setMinute(ttm->tm_min);
      rtc.setHour(ttm->tm_hour);

      log_printf("Receive NTP Response %lu\n", (unsigned long)secsSince1900);

      rtc.setDate (ttm->tm_mday);
      rtc.setMonth(ttm->tm_mon);
      rtc.setYear(ttm->tm_year);

      log_printf("Receive NTP Response %lu\n", (unsigned long)secsSince1900);

      struct timeval tv = { .tv_sec = mktime(ttm), .tv_usec = 0 };
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
    GP_MAKE_BOX(GP.LABEL("Show seconds"); GP.SWITCH("clock_show_sec", clock_show_sec ? true: false, 0););

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
    if (n>=-12 && n<=12) {
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

    n = ui.getBool("clock_show_sec");
    if (n>=0 && n<=1) {
      clock_show_sec = n;
    }

    String s = ui.getString("clock_ntp_server");
    if (s && strcmp(s.c_str(), ntpServerName) != 0) {
      strncpy(ntpServerName, s.c_str(), sizeof(ntpServerName)-1);
    }

    // Save new settings to EEPROM
    write_eeprom_data();

    // If timezone changed, update system time
    if (update_time) {
      if (clock_use_ntp) {
        getNtpTime();
      } else if (clock_use_rtc) {
        set_time_from_rtc();
      }
    }
  }

  // Handle time setting form
  if (p.form("/settime")) {
    GPtime gptime = ui.getTime("time");
    log_printf("Action Settime: %d:%02d:%02d\n", gptime.hour, gptime.minute, gptime.second);

    // Set time to RTC
    rtc.setSecond(gptime.second);
    rtc.setMinute(gptime.minute);
    rtc.setHour(gptime.hour);

    set_clock_time(gptime.hour, gptime.minute, gptime.second);
  }
}
