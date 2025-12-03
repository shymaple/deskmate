/*
 * SECURITY WARNING: This is a TEMPLATE file.
 * 
 * DO NOT commit this file or any secrets.h file to version control!
 * 
 * Instructions:
 * 1. Copy this file to secrets.h (it will be gitignored)
 * 2. Fill in your actual credentials below
 * 3. In deskmate.ino, add #define USE_SECRETS before compiling
 * 4. Never share or commit secrets.h
 * 
 * For production use, configure via web interface instead of using this file.
 */

#ifndef SECRETS_H
#define SECRETS_H

// WiFi Credentials (for local development only)
#define WIFI_SSID "YourWiFiSSID"
#define WIFI_PASSWORD "YourWiFiPassword"

// Weather API Configuration
#define WEATHER_API_KEY "YourOpenWeatherMapAPIKey"
#define WEATHER_LAT "YourLatitude"
#define WEATHER_LON "YourLongitude"
#define WEATHER_CITY "YourCityName"

// Timezone Configuration (optional - can be set via web interface)
#define GMT_OFFSET_SEC 19800  // Example: IST = UTC+5:30 = 19800 seconds
#define DAYLIGHT_OFFSET_SEC 0

#endif // SECRETS_H

