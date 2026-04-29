#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

// Other includes and declarations...

char timezonedb_api_key[80] = "";  // TimezoneDB API key

// Existing code...

// Get timezone from TimezoneDB API based on GPS coordinates
// Returns the timezone offset in hours (can be negative)
// Returns INT_MIN if the request fails
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
    int tzHours = gmtOffset / 3600;   // Convert to hours
    
    log_printf("Timezone offset: %d hours\n", tzHours);
    return tzHours;
  }
  
  log_printf("gmtOffset not found in response\n");
  return INT_MIN;
}

// Periodically update timezone from GPS
void update_timezone_from_gps()
{
  static unsigned long last_tz_update = 0;
  unsigned long now = millis();
  
  // Update timezone every 60 minutes (3600000 ms)
  if (now - last_tz_update > 3600000) {
    last_tz_update = now;
    
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
}