/*
 * Deskmate Clock Firmware
 * 
 * SECURITY NOTICE:
 * ================
 * This firmware does NOT contain hardcoded credentials by default.
 * All sensitive information (WiFi passwords, API keys, location data) must be
 * configured via the web interface at http://192.168.4.1 after first boot.
 * 
 * For local development/testing, you can create a secrets.h file (see secrets_template.h)
 * and define USE_SECRETS before compiling. This file should NEVER be committed to version control.
 * 
 * Sensitive data that should NOT be in source code:
 * - WiFi SSID and password
 * - Weather API keys
 * - Personal location coordinates
 * 
 * The device starts in Access Point mode by default, allowing secure configuration
 * via the web interface. Credentials are stored in EEPROM after configuration.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <Wire.h>
#include <math.h>
#include <DNSServer.h>    // Captive portal DNS redirect
#include <ESPmDNS.h>      // deskmate.local hostname

// SECURITY: Optional secrets file for local development only
// Create secrets.h from secrets_template.h and define USE_SECRETS to enable
#ifdef USE_SECRETS
  #include "secrets.h"
#endif

// Math constant for angle calculations
#ifndef PI
  #define PI 3.14159265359
#endif

// --- HARDWARE CONFIG ---
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 4 // 32x8 is 4 modules


// For ESP32 C3 Super Mini
#define CLK_PIN 8
#define DATA_PIN 10
#define CS_PIN 9
#define MODE_BUTTON_PIN 6


// MPU6050 I2C pins
#define SDA_PIN 4
#define SCL_PIN 5


// MPU6050 I2C address and registers
#define MPU6050_ADDR 0x68
#define MPU6050_PWR_MGMT_1 0x6B
#define MPU6050_ACCEL_XOUT_H 0x3B
#define MPU6050_ACCEL_YOUT_H 0x3D
#define MPU6050_ACCEL_ZOUT_H 0x3F
#define MPU6050_WHO_AM_I 0x75


// Initialize Parola
MD_Parola P = MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);


// Hardware object for rotation control (shared with Parola)
MD_MAX72XX* mxHardware = NULL;


// --- BREAK TYPES (Water/Posture) ---
enum BreakType : uint8_t {
  WATER_BREAK = 0,
  POSTURE_BREAK = 1
};

// Break intervals in minutes (stored in config)
// Water: 15, 20, 30 minutes
// Posture: 30, 45, 60 minutes


// --- POMODORO PHASE STRUCTURE ---
struct PomodoroPhase {
 int durationMinutes; // Duration in minutes
 char name[16]; // Phase name (e.g., "FOCUS", "BREAK")
};


// --- CONFIGURATION STRUCTURE ---
// CONFIG_VERSION: Increment when config structure changes in incompatible way
#define CONFIG_VERSION 3  // Bumped for auto-weather and auto-brightness settings

struct Config {
  uint8_t version;
  char ssid[32];
  char password[64];
  char ntpServer[64];
  long gmtOffset_sec;
  int daylightOffset_sec;
  char weatherApiKey[64];
  char lat[16];
  char lon[16];
  char cityName[32];
  uint8_t brightness;          // 0-15, use uint8_t to save space
  bool clockFormat24h;
  // Break settings (replaces reminders - saves ~150 bytes)
  bool waterBreakEnabled;      // Water break on/off
  uint8_t waterIntervalMins;   // 15, 20, or 30 minutes
  bool postureBreakEnabled;    // Posture break on/off
  uint8_t postureIntervalMins; // 30, 45, or 60 minutes
  // Auto-weather settings
  bool autoWeatherEnabled;     // Enable 10-min auto weather display
  // Auto-brightness settings
  bool autoBrightnessEnabled;  // Enable day/night auto brightness
  uint8_t dayBrightness;       // Brightness during day (0-15, default 5)
  uint8_t nightBrightness;     // Brightness during night (0-15, default 2)
  uint8_t nightStartHour;      // Hour when night starts (0-23, default 20 = 8 PM)
  uint8_t nightEndHour;        // Hour when night ends (0-23, default 6 = 6 AM)
  PomodoroPhase pomodoroPhases[4];
  bool valid;
};

Config config;


// --- EEPROM SETTINGS ---
#define EEPROM_SIZE 512  // Reduced - no longer need space for reminders
#define EEPROM_ADDR 0


// --- WEB SERVER ---
WebServer server(80);


// --- ACCESS POINT SETTINGS ---
const char* ap_ssid = "ESP32-Clock";
const char* ap_password = "clock123";


// --- CAPTIVE PORTAL / DNS SERVER ---
DNSServer dnsServer;
const byte DNS_PORT = 53;
const IPAddress apIP(192, 168, 4, 1);
bool captivePortalActive = false;
unsigned long lastDNSProcess = 0;
const unsigned long DNS_PROCESS_INTERVAL = 10; // Process DNS every 10ms
bool mdnsStarted = false;


// --- GLOBAL STATE ---
struct tm timeinfo;
char timeYesterday[4], timeLastHour[3];
// RTC time management
unsigned long lastNTPSync = 0;
const unsigned long NTP_SYNC_INTERVAL = 86400000; // 24 hours in milliseconds
bool ntpSyncScheduled = false; // Flag for scheduled sync at 1:11:11 AM
int lastSyncedDay = -1; // Track last sync day


// Display Mode State
enum DisplayMode : uint8_t {
  MODE_CLOCK = 0,
  MODE_WEATHER = 1,
  MODE_POMODORO = 2,
  MODE_BREAK = 3      // Shared for water/posture breaks
};
DisplayMode currentMode = MODE_CLOCK;
DisplayMode previousMode = MODE_CLOCK;  // For returning from weather/break


// Blinking Colon for Clock
unsigned long previousMillisColon = 0;
const uint16_t colonBlinkInterval = 1000;
bool colonVisible = true;


// Weather data buffer
char weatherData[100] = "Loading...";
bool weatherDataReady = false;


// --- AUTO-WEATHER TIMER (every 10 min, show 10 sec) ---
unsigned long lastAutoWeather = 0;
const uint32_t AUTO_WEATHER_INTERVAL = 600000;  // 10 minutes in ms
const uint32_t AUTO_WEATHER_DISPLAY_MS = 10000; // 10 seconds display
const uint32_t FLIP_WEATHER_DISPLAY_MS = 15000; // 15 seconds for flip-triggered weather
unsigned long weatherDisplayStart = 0;          // When weather display started
bool autoWeatherActive = false;                 // Currently showing auto-weather
bool flipWeatherActive = false;                 // Currently showing flip-triggered weather


// --- BREAK SYSTEM (shared for water/posture) ---
char breakDisplayText[40] = "";  // Shared buffer for break messages
BreakType activeBreakType = WATER_BREAK;  // Which break is currently active
unsigned long lastWaterBreak = 0;   // Last water break trigger time
unsigned long lastPostureBreak = 0; // Last posture break trigger time
unsigned long breakDisplayStart = 0; // When break display started
const uint32_t BREAK_DISPLAY_MAX_MS = 30000; // Max 30 sec display
bool breakActive = false;           // Currently in break mode


// Pomodoro timer state
int currentPomodoroPhase = 0; // Current phase index (0-3)
unsigned long pomodoroStartTime = 0;
unsigned long pomodoroRemainingSeconds = 0;
bool pomodoroPaused = false;
bool pomodoroTimerExpired = false;
unsigned long pomodoroExpiredTime = 0;
const unsigned long POMODORO_AUTO_RETURN_MS = 300000; // 5 min auto-return
unsigned long lastPomodoroUpdate = 0;

// Pomodoro gesture detection (shared accelerometer data)
int16_t g_accelX = 0, g_accelY = 0, g_accelZ = 0;
float g_magnitude = 0;
bool g_sensorRead = false;
bool pomodoroBlinkState = false; // For blinking display when expired
unsigned long lastPomodoroBlink = 0; // Last blink toggle time
const unsigned long POMODORO_BLINK_INTERVAL = 500; // Blink every 500ms
// Lift detection for pause/resume
int16_t lastAccelY = 0; // For lift detection
bool deviceLifted = false; // Current lift state
unsigned long liftDetectedTime = 0; // When lift was detected
const unsigned long LIFT_DEBOUNCE_MS = 1000; // Require 1s stable lift
const int16_t LIFT_THRESHOLD = 5000; // Acceleration threshold for lift detection
// Flip detection for phase advance
bool flipDetected = false; // Flip detected flag
unsigned long flipDetectedTime = 0; // When flip was detected
const unsigned long FLIP_DEBOUNCE_MS = 500; // Require 500ms stable flip
// Phase start animation (3 blinks when new phase starts)
bool phaseStartBlinking = false;       // Currently in phase start animation
uint8_t phaseStartBlinkCount = 0;      // Number of blinks completed
unsigned long phaseStartBlinkTime = 0; // Last blink toggle time
const uint8_t PHASE_START_BLINK_TOTAL = 6; // 6 toggles = 3 blinks (on-off-on-off-on-off)


// MPU6050 orientation tracking
bool displayFlipped = false;
unsigned long lastOrientationCheck = 0;
const unsigned long orientationCheckInterval = 2000; // Check every 2 seconds (reduced for battery)
int16_t lastAccelZ = 0;
bool orientationCheckEnabled = true; // Can be disabled when not needed


// Button handling
unsigned long lastButtonPress = 0;
const long debounceDelay = 250;
bool buttonPressed = false;


// WiFi connection state
bool wifiConnected = false;
unsigned long lastWiFiReconnectAttempt = 0;
const unsigned long wifiReconnectInterval = 30000; // 30 seconds
unsigned long lastAPCheck = 0;
const unsigned long apCheckInterval = 10000; // Check AP every 10 seconds

// Async configuration update flags (for immediate response to web requests)
bool pendingWiFiReconnect = false;
bool pendingNTPSync = false;
bool pendingWeatherUpdate = false;
unsigned long pendingWiFiReconnectTime = 0;
const unsigned long WIFI_RECONNECT_DELAY = 500; // Small delay before reconnect


// --- CUSTOM FONT DEFINITION ---
MD_MAX72XX::fontType_t customFontData[] PROGMEM = {
 // ASCII 0-31 unused - width 0 for each
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 1, 0x00,             // 32 - Space
 1, 0x5F,             // 33 - !
 3, 0x03, 0x00, 0x03, // 34 - "
 5, 0x14, 0x7F, 0x14, 0x7F, 0x14, // 35 - #
 5, 0x24, 0x2A, 0x7F, 0x2A, 0x12, // 36 - $
 5, 0x23, 0x13, 0x08, 0x64, 0x62, // 37 - %
 5, 0x36, 0x49, 0x55, 0x22, 0x50, // 38 - &
 1, 0x03,             // 39 - '
 3, 0x1C, 0x22, 0x41, // 40 - (
 3, 0x41, 0x22, 0x1C, // 41 - )
 5, 0x14, 0x08, 0x3E, 0x08, 0x14, // 42 - *
 5, 0x08, 0x08, 0x3E, 0x08, 0x08, // 43 - +
 2, 0x50, 0x30,       // 44 - ,
 5, 0x08, 0x08, 0x08, 0x08, 0x08, // 45 - -
 1, 0x60,             // 46 - .
 5, 0x20, 0x10, 0x08, 0x04, 0x02, // 47 - /
 // Numbers 0-9 (ASCII 48-57)
 3, 0x3E, 0x41, 0x3E, // 48 - 0
 3, 0x00, 0x7F, 0x00, // 49 - 1
 3, 0x79, 0x49, 0x4F, // 50 - 2
 3, 0x49, 0x49, 0x7F, // 51 - 3
 3, 0x0F, 0x08, 0x7F, // 52 - 4
 3, 0x4F, 0x49, 0x79, // 53 - 5
 3, 0x7F, 0x49, 0x79, // 54 - 6
 3, 0x01, 0x01, 0x7F, // 55 - 7
 3, 0x7F, 0x49, 0x7F, // 56 - 8
 3, 0x4F, 0x49, 0x7F, // 57 - 9
 1, 0x22,             // 58 - :
 2, 0x56, 0x36,       // 59 - ;
 3, 0x08, 0x14, 0x22, // 60 - <
 3, 0x14, 0x14, 0x14, // 61 - =
 3, 0x22, 0x14, 0x08, // 62 - >
 5, 0x02, 0x01, 0x51, 0x09, 0x06, // 63 - ?
 5, 0x3E, 0x41, 0x5D, 0x55, 0x5E, // 64 - @
 // Uppercase Letters A-Z (ASCII 65-90)
 3, 0x7E, 0x09, 0x7E, // 65 - A
 3, 0x7F, 0x49, 0x36, // 66 - B
 3, 0x3E, 0x41, 0x41, // 67 - C
 3, 0x7F, 0x41, 0x3E, // 68 - D
 3, 0x7F, 0x49, 0x41, // 69 - E
 3, 0x7F, 0x09, 0x01, // 70 - F
 3, 0x3E, 0x41, 0x79, // 71 - G
 3, 0x7F, 0x08, 0x7F, // 72 - H
 3, 0x41, 0x7F, 0x41, // 73 - I
 3, 0x20, 0x40, 0x3F, // 74 - J
 3, 0x7F, 0x08, 0x77, // 75 - K
 3, 0x7F, 0x40, 0x40, // 76 - L
 5, 0x7F, 0x02, 0x04, 0x02, 0x7F, // 77 - M
 4, 0x7F, 0x04, 0x08, 0x7F, // 78 - N
 3, 0x3E, 0x41, 0x3E, // 79 - O
 3, 0x7F, 0x09, 0x06, // 80 - P
 3, 0x3E, 0x41, 0x7E, // 81 - Q
 3, 0x7F, 0x09, 0x76, // 82 - R
 3, 0x46, 0x49, 0x31, // 83 - S
 3, 0x01, 0x7F, 0x01, // 84 - T
 3, 0x3F, 0x40, 0x3F, // 85 - U
 3, 0x1F, 0x60, 0x1F, // 86 - V
 5, 0x3F, 0x40, 0x30, 0x40, 0x3F, // 87 - W
 3, 0x77, 0x08, 0x77, // 88 - X
 3, 0x07, 0x78, 0x07, // 89 - Y
 3, 0x71, 0x49, 0x47, // 90 - Z
};


// Custom characters
uint8_t colonOn[] = {1, 0x22};
uint8_t colonOff[] = {1, 0x00};
uint8_t degreeSymbol[] = {1, 0x06};
uint8_t pmDot[] = {1, 0x40}; // Single dot at top for PM indicator


// --- WEB UI HTML ---
const char* htmlPage = R"HTML(
<!DOCTYPE html>
<html>
<head>
 <meta name="viewport" content="width=device-width, initial-scale=1">
 <meta charset="UTF-8">
 <title>Clock Config</title>
 <style>
   * { box-sizing: border-box; margin: 0; padding: 0; }
   body { font-family: Arial, sans-serif; background: #f0f0f0; padding: 10px; }
   .container { max-width: 500px; margin: 0 auto; background: white; padding: 20px; }
   h1 { font-size: 20px; margin-bottom: 10px; }
   .subtitle { color: #666; font-size: 12px; margin-bottom: 15px; }
   .section { margin-bottom: 15px; }
   .section-title { font-size: 11px; font-weight: bold; color: #0066cc; margin-bottom: 8px; }
   .form-group { margin-bottom: 12px; }
   label { display: block; font-size: 12px; margin-bottom: 4px; }
   input, select { width: 100%; padding: 8px; border: 1px solid #ccc; font-size: 13px; }
   .help { font-size: 10px; color: #999; margin-top: 2px; }
   .btn { width: 100%; padding: 10px; background: #0066cc; color: white; border: none; font-size: 14px; margin-top: 10px; cursor: pointer; }
   .status { padding: 10px; margin-bottom: 15px; font-size: 12px; display: none; }
   .status.success { background: #d4edda; color: #155724; display: block; }
   .status.error { background: #f8d7da; color: #721c24; display: block; }
   .status.saving { background: #cce5ff; color: #004085; display: block; }
   .info { background: #e7f3ff; padding: 10px; font-size: 11px; margin-bottom: 15px; }
 </style>
</head>
<body>
 <div class="container">
   <h1>⚙️ Clock Settings</h1>
   <p class="subtitle">Configure WiFi, Time & Weather</p>
  
   <div id="status" class="status"></div>
  
   <div class="info">
     <strong>IP:</strong> <span id="ipAddr">-</span> |
     <strong>WiFi:</strong> <span id="wifiStatus">-</span>
   </div>
  
   <form id="configForm">
     <div class="section">
       <div class="section-title">WiFi</div>
       <div class="form-group">
         <label>SSID</label>
         <input type="text" id="ssid" name="ssid" required>
       </div>
       <div class="form-group">
         <label>Password</label>
         <div style="position:relative;">
           <input type="password" id="password" name="password" style="padding-right:60px;">
           <button type="button" id="togglePwd" onclick="togglePassword()" style="position:absolute;right:8px;top:50%;transform:translateY(-50%);padding:4px 8px;font-size:11px;background:#f0f0f0;border:1px solid #ccc;border-radius:4px;cursor:pointer;">Show</button>
         </div>
       </div>
     </div>
    
     <div class="section">
       <div class="section-title">Time</div>
       <div class="form-group">
         <label>Clock Format</label>
         <select id="clockFormat" name="clockFormat" style="width: 100%; padding: 10px 12px; border: 1.5px solid #e0e0e0; border-radius: 6px; font-size: 14px;">
           <option value="24">24-Hour</option>
           <option value="12">12-Hour (AM/PM)</option>
         </select>
         <div class="help">12h format shows dot (*) for PM</div>
       </div>
       <div class="form-group">
         <label>NTP Server</label>
         <input type="text" id="ntpServer" name="ntpServer" value="pool.ntp.org">
         <div class="help">Default: pool.ntp.org</div>
       </div>
       <div class="form-group">
         <label>GMT Offset (seconds)</label>
         <input type="number" id="gmtOffset" name="gmtOffset" value="19800">
         <div class="help">IST = 19800 (UTC+5:30)</div>
       </div>
       <div class="form-group">
         <label>Daylight Saving (seconds)</label>
         <input type="number" id="daylightOffset" name="daylightOffset" value="0">
       </div>
     </div>
    
     <div class="section">
       <div class="section-title">Weather</div>
       <div class="form-group">
         <label>API Key</label>
         <input type="text" id="apiKey" name="apiKey">
       </div>
       <div class="form-group">
         <label>Latitude</label>
         <input type="text" id="lat" name="lat">
       </div>
       <div class="form-group">
         <label>Longitude</label>
         <input type="text" id="lon" name="lon">
       </div>
       <div class="form-group">
         <label>City Name</label>
         <input type="text" id="cityName" name="cityName">
       </div>
     </div>
    
     <div class="section">
       <div class="section-title">Display</div>
       <div class="form-group">
         <label>Brightness (0-15)</label>
         <input type="number" id="brightness" name="brightness" min="0" max="15" value="5">
       </div>
     </div>
    
     <div class="section">
       <div class="section-title">Break Reminders</div>
       <div class="form-group" style="display:flex;align-items:center;gap:10px;">
         <input type="checkbox" id="waterBreakEnabled" name="waterBreakEnabled" style="width:auto;">
         <label for="waterBreakEnabled" style="margin:0;">Water Break</label>
         <select id="waterInterval" name="waterInterval" style="margin-left:auto;">
           <option value="15">15 min</option>
           <option value="20">20 min</option>
           <option value="30">30 min</option>
         </select>
       </div>
       <div class="form-group" style="display:flex;align-items:center;gap:10px;">
         <input type="checkbox" id="postureBreakEnabled" name="postureBreakEnabled" style="width:auto;">
         <label for="postureBreakEnabled" style="margin:0;">Posture Break</label>
         <select id="postureInterval" name="postureInterval" style="margin-left:auto;">
           <option value="30">30 min</option>
           <option value="45">45 min</option>
           <option value="60">60 min</option>
         </select>
       </div>
       <div class="help">Breaks only trigger in Clock mode. Flip device to dismiss.</div>
     </div>

     <div class="section">
       <div class="section-title">Weather Display</div>
       <div class="form-group" style="display:flex;align-items:center;gap:10px;">
         <input type="checkbox" id="autoWeatherEnabled" name="autoWeatherEnabled" style="width:auto;">
         <label for="autoWeatherEnabled" style="margin:0;">Auto-weather every 10 min</label>
       </div>
       <div class="help">Flip device in Clock mode to show weather for 15 sec anytime.</div>
     </div>

     <div class="section">
       <div class="section-title">Auto Brightness</div>
       <div class="form-group" style="display:flex;align-items:center;gap:10px;">
         <input type="checkbox" id="autoBrightnessEnabled" name="autoBrightnessEnabled" style="width:auto;">
         <label for="autoBrightnessEnabled" style="margin:0;">Enable day/night auto brightness</label>
       </div>
       <div class="form-group" style="display:flex;align-items:center;gap:10px;">
         <label style="width:100px;">Day (0-15)</label>
         <input type="number" id="dayBrightness" name="dayBrightness" min="0" max="15" value="5" style="width:60px;">
         <label style="width:100px;margin-left:15px;">Night (0-15)</label>
         <input type="number" id="nightBrightness" name="nightBrightness" min="0" max="15" value="2" style="width:60px;">
       </div>
       <div class="form-group" style="display:flex;align-items:center;gap:10px;">
         <label style="width:100px;">Night starts</label>
         <select id="nightStartHour" name="nightStartHour" style="width:80px;">
           <option value="18">6 PM</option>
           <option value="19">7 PM</option>
           <option value="20">8 PM</option>
           <option value="21">9 PM</option>
           <option value="22">10 PM</option>
           <option value="23">11 PM</option>
         </select>
         <label style="width:100px;margin-left:15px;">Night ends</label>
         <select id="nightEndHour" name="nightEndHour" style="width:80px;">
           <option value="5">5 AM</option>
           <option value="6">6 AM</option>
           <option value="7">7 AM</option>
           <option value="8">8 AM</option>
         </select>
       </div>
       <div class="help">Brightness adjusts automatically based on your timezone.</div>
     </div>

     <div class="section">
       <div class="section-title">Pomodoro Timer</div>
       <div class="form-group">
         <label>Phase 1: Focus (minutes)</label>
         <input type="number" id="pomodoroPhase0Duration" name="pomodoroPhase0Duration" min="1" max="120" value="60">
       </div>
       <div class="form-group">
         <label>Phase 2: Short Break (minutes)</label>
         <input type="number" id="pomodoroPhase1Duration" name="pomodoroPhase1Duration" min="1" max="120" value="5">
       </div>
       <div class="form-group">
         <label>Phase 3: Focus (minutes)</label>
         <input type="number" id="pomodoroPhase2Duration" name="pomodoroPhase2Duration" min="1" max="120" value="30">
       </div>
       <div class="form-group">
         <label>Phase 4: Long Break (minutes)</label>
         <input type="number" id="pomodoroPhase3Duration" name="pomodoroPhase3Duration" min="1" max="120" value="10">
       </div>
       <div class="help" style="margin-top: 10px;">
         <strong>Usage:</strong> Press button to toggle: Clock &harr; Pomodoro<br>
         <strong>Flip device 180&deg;</strong> to advance phase<br>
         <strong>Lift and put back</strong> to pause/resume
       </div>
     </div>
    
     <button type="submit" class="btn" id="saveBtn">Save & Apply</button>
   </form>
 </div>
  <script>
   function showStatus(msg, type) {
     const status = document.getElementById('status');
     status.className = 'status ' + type;
     status.textContent = msg;
     if (type !== 'saving') {
       setTimeout(() => status.className = 'status', 5000);
     }
   }

   function togglePassword() {
     const pwd = document.getElementById('password');
     const btn = document.getElementById('togglePwd');
     if (pwd.type === 'password') {
       pwd.type = 'text';
       btn.textContent = 'Hide';
     } else {
       pwd.type = 'password';
       btn.textContent = 'Show';
     }
   }
  
   function updateStatus() {
     fetch('/status')
       .then(r => r.json())
       .then(data => {
         document.getElementById('ipAddr').textContent = data.ip || '-';
         document.getElementById('wifiStatus').textContent = data.wifi || '-';
       });
   }
  
   window.onload = function() {
     updateStatus();
     setInterval(updateStatus, 5000);
    
     fetch('/config')
       .then(r => r.json())
       .then(data => {
         // Always populate fields, even if not saved yet
         document.getElementById('ssid').value = data.ssid || "";
         document.getElementById('password').value = data.password || "";
         document.getElementById('ntpServer').value = data.ntpServer || 'pool.ntp.org';
         document.getElementById('gmtOffset').value = data.gmtOffset_sec || 19800;
         document.getElementById('daylightOffset').value = data.daylightOffset_sec || 0;
         document.getElementById('apiKey').value = data.weatherApiKey || "";
         document.getElementById('lat').value = data.lat || "";
         document.getElementById('lon').value = data.lon || "";
         document.getElementById('cityName').value = data.cityName || "";
         document.getElementById('brightness').value = data.brightness || 5;
         document.getElementById('clockFormat').value = data.clockFormat24h ? '24' : '12';
        
         // Load break settings
         document.getElementById('waterBreakEnabled').checked = data.waterBreakEnabled !== false;
         document.getElementById('waterInterval').value = data.waterIntervalMins || 15;
         document.getElementById('postureBreakEnabled').checked = data.postureBreakEnabled !== false;
         document.getElementById('postureInterval').value = data.postureIntervalMins || 30;

         // Load auto-weather setting
         document.getElementById('autoWeatherEnabled').checked = data.autoWeatherEnabled !== false;

         // Load auto-brightness settings
         document.getElementById('autoBrightnessEnabled').checked = data.autoBrightnessEnabled !== false;
         document.getElementById('dayBrightness').value = data.dayBrightness || 5;
         document.getElementById('nightBrightness').value = data.nightBrightness || 2;
         document.getElementById('nightStartHour').value = data.nightStartHour || 20;
         document.getElementById('nightEndHour').value = data.nightEndHour || 6;

         // Load Pomodoro phases
         if (data.pomodoroPhases && Array.isArray(data.pomodoroPhases)) {
           for (let i = 0; i < 4; i++) {
             if (data.pomodoroPhases[i]) {
               document.getElementById('pomodoroPhase' + i + 'Duration').value = data.pomodoroPhases[i].durationMinutes || (i === 0 ? 60 : i === 1 ? 5 : i === 2 ? 30 : 10);
             }
           }
         }
       })
       .catch(err => console.log('Error loading config:', err));
   };

   document.getElementById('configForm').onsubmit = function(e) {
     e.preventDefault();
     const btn = document.getElementById('saveBtn');
     const origText = btn.textContent;

     // Show saving state
     btn.disabled = true;
     btn.textContent = 'Saving...';
     btn.style.opacity = '0.7';
     showStatus('Saving settings to device...', 'saving');

     const formData = new FormData(this);

     fetch('/save', {
       method: 'POST',
       body: formData
     })
     .then(r => r.text())
     .then(data => {
       btn.disabled = false;
       btn.textContent = origText;
       btn.style.opacity = '1';

       if (data.includes('success')) {
         showStatus('Settings saved! Applying changes...', 'success');
         setTimeout(() => {
           showStatus('Done! Settings applied.', 'success');
           updateStatus();
         }, 2000);
       } else {
         showStatus('Error: Could not save settings', 'error');
       }
     })
     .catch(err => {
       btn.disabled = false;
       btn.textContent = origText;
       btn.style.opacity = '1';
       showStatus('Network error. Please try again.', 'error');
     });
   };
 </script>
</body>
</html>
)HTML";


// Function prototypes
void loadConfig();
void saveConfig();
void setupAP();
void connectWiFi();
void syncNTP();
void getWeather();
void showWiFiConnecting();
void handleButton();
void updateClockDisplay();
void startWeatherScroll();
void startAutoWeather();
void startFlipWeather();
void checkAutoBrightness();
void handleBreak(BreakType type);
void checkBreaks();
void dismissBreak();
void initMPU6050();
bool checkMPU6050();
void checkOrientation();
int16_t readMPU6050Accel(int reg);
void flipDisplayHardware(bool flipped);
textEffect_t getScrollEffect();  // Returns correct scroll direction based on flip state
void handleRoot();
void handleConfig();
void handleSave();
void handleStatus();
// Captive portal functions
void handleCaptivePortalRedirect();
void handleAndroidCaptive();
void handleAppleCaptive();
void handleWindowsCaptive();
void handleFirefoxCaptive();
void handleNotFound();
void startCaptivePortal();
void stopCaptivePortal();
// Pomodoro functions
void startPomodoroMode();
void updatePomodoroDisplay();
void advancePomodoroPhase();
void pausePomodoro();
void resumePomodoro();
void readPomodoroAccel();
void checkPomodoroLift();
void checkPomodoroFlip();


void setup() {
 Serial.begin(115200);
 delay(1000);
  Serial.println("\n\nESP32 Clock with Web Config");
 Serial.println("==========================\n");
  // Button setup
 pinMode(MODE_BUTTON_PIN, INPUT_PULLUP);
  // Initialize I2C for MPU6050
 Wire.begin(SDA_PIN, SCL_PIN);
 delay(100);
  // Initialize MPU6050
 initMPU6050();
  // Display setup
 P.begin(1);
 P.setZone(0, 0, MAX_DEVICES - 1);
 P.setFont(0, customFontData);
 P.setCharSpacing(0, 1); // Normal spacing
 P.addChar(':', colonOn);
 P.addChar('$', degreeSymbol);
 P.addChar('*', pmDot); // Use * for PM dot indicator
 P.setIntensity(5);
 P.displayClear();
  // Get hardware reference for rotation control
 // Note: This is a workaround since MD_Parola doesn't expose rotation directly
 // We'll use a separate MAX72xx object for rotation control
 mxHardware = new MD_MAX72XX(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);
 mxHardware->begin();
  // Initialize EEPROM (preserves existing data across firmware updates)
  // EEPROM data persists even when flashing new firmware
  EEPROM.begin(EEPROM_SIZE);
  Serial.println("EEPROM initialized - existing config will be preserved across updates");
  
  // Load configuration (will preserve existing valid configs)
  loadConfig();
  // ALWAYS start AP mode first so web interface is always accessible
 Serial.println("Starting Access Point...");
 setupAP();
 delay(500); // Give AP time to initialize
  // Setup web server (always running)
 server.on("/", handleRoot);
 server.on("/config", handleConfig);
 server.on("/save", handleSave);
 server.on("/status", handleStatus);

 // Captive portal detection routes
 server.on("/generate_204", handleAndroidCaptive);      // Android
 server.on("/gen_204", handleAndroidCaptive);           // Android alt
 server.on("/hotspot-detect.html", handleAppleCaptive); // Apple iOS/macOS
 server.on("/library/test/success.html", handleAppleCaptive); // Apple alt
 server.on("/connecttest.txt", handleWindowsCaptive);   // Windows
 server.on("/ncsi.txt", handleWindowsCaptive);          // Windows alt
 server.on("/success.txt", handleFirefoxCaptive);       // Firefox
 server.on("/canonical.html", handleFirefoxCaptive);    // Firefox alt
 server.onNotFound(handleNotFound);                     // Catch-all

 server.begin();
 Serial.println("[WEB] Server started with captive portal routes");
  // Show AP info
 Serial.println("\n=== Access Point Started ===");
 Serial.print("AP SSID: ");
 Serial.println(ap_ssid);
 Serial.print("AP Password: ");
 Serial.println(ap_password);
 Serial.print("AP IP: ");
 Serial.println(WiFi.softAPIP());
 Serial.println("Open: http://192.168.4.1 or http://deskmate.local");
 Serial.println("Captive portal active - popup will appear on connect");
 Serial.println("===========================\n");

 // Show IP address briefly on display
 P.displayZoneText(0, "192.168", PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
 P.displayAnimate();
 delay(1000);
 P.displayZoneText(0, "4.1", PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
 P.displayAnimate();
 delay(1000);

  // Try to connect to WiFi if SSID is configured (in background, AP still works)
 if (strlen(config.ssid) > 0) {
   Serial.println("Attempting WiFi connection in background...");
   P.displayZoneText(0, "WIFI", PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
   P.displayAnimate();
   connectWiFi();
 }

  // Wait a bit for WiFi connection attempt
 delay(2000);

  // If WiFi connected, sync time and get weather
 if (WiFi.status() == WL_CONNECTED) {
   wifiConnected = true;
   Serial.println("\nWiFi connected!");
   Serial.print("Station IP: ");
   Serial.println(WiFi.localIP());
   Serial.print("AP IP (still available): ");
   Serial.println(WiFi.softAPIP());

   P.displayZoneText(0, "SYNC", PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
   P.displayAnimate();
   syncNTP();

   P.displayZoneText(0, "READY", PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
   P.displayAnimate();
   delay(1000);

   getLocalTime(&timeinfo);
   strftime(timeYesterday, 4, "%a", &timeinfo);
   strftime(timeLastHour, 3, "%H", &timeinfo);

   getWeather();

   P.displayClear();
   currentMode = MODE_CLOCK;
   colonVisible = true;

   // Initialize break and auto-weather timers (start counting from now)
   unsigned long now = millis();
   lastWaterBreak = now;
   lastPostureBreak = now;
   lastAutoWeather = now;

   // Apply auto-brightness based on time of day
   checkAutoBrightness();

   updateClockDisplay();
 } else {
   Serial.println("\nWiFi not configured or connection failed - AP mode active");
   Serial.println("Connect to WiFi network: ESP32-Clock");
   Serial.println("Password: clock123");
   Serial.println("A captive portal popup should appear automatically!");
   Serial.println("Or open: http://192.168.4.1 or http://deskmate.local");

   // Show scrolling instruction
   P.displayClear();
   textEffect_t scrollDir = getScrollEffect();
   P.displayZoneText(0, "CONNECT TO ESP32-CLOCK WIFI    ", PA_LEFT, 80, 0, scrollDir, scrollDir);

   // Wait for scroll to complete (non-blocking in loop will handle it)
   unsigned long scrollStart = millis();
   while (millis() - scrollStart < 5000) {
     if (P.displayAnimate()) {
       // Scroll complete
       break;
     }
     server.handleClient();
     if (captivePortalActive) {
       dnsServer.processNextRequest();
     }
     delay(10);
   }

   // Show CONFIG after scroll
   P.displayClear();
   P.displayZoneText(0, "CONFIG", PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
   P.displayAnimate();
 }
}


void loop() {
 // Handle web server
 server.handleClient();
  
  // Handle async configuration updates (non-blocking)
  unsigned long currentMillis = millis();
  
  // Handle pending WiFi reconnect
  if (pendingWiFiReconnect && currentMillis >= pendingWiFiReconnectTime) {
    pendingWiFiReconnect = false;
    Serial.println("Executing async WiFi reconnect...");
    WiFi.disconnect();
    delay(100); // Minimal delay
    connectWiFi();
    
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      Serial.println("WiFi reconnected successfully");
      // NTP and weather will be handled by their pending flags
    } else {
      wifiConnected = false;
      Serial.println("WiFi reconnect failed - AP mode remains active");
      // Ensure AP is still running
      if (WiFi.getMode() != WIFI_AP_STA && WiFi.getMode() != WIFI_AP) {
        setupAP();
      }
      // Clear pending operations if WiFi failed
      pendingNTPSync = false;
      pendingWeatherUpdate = false;
    }
  }
  
  // Handle pending NTP sync (only if WiFi is connected and no WiFi reconnect pending)
  if (pendingNTPSync && !pendingWiFiReconnect && WiFi.status() == WL_CONNECTED) {
    pendingNTPSync = false;
    Serial.println("Executing async NTP sync...");
    syncNTP();
    // Update clock display after sync
    if (getLocalTime(&timeinfo)) {
      updateClockDisplay();
    }
  }
  
  // Handle pending weather update (only if WiFi is connected and no WiFi reconnect pending)
  if (pendingWeatherUpdate && !pendingWiFiReconnect && WiFi.status() == WL_CONNECTED) {
    pendingWeatherUpdate = false;
    Serial.println("Executing async weather update...");
    getWeather();
  }
  
  // Handle button input
 handleButton();
  // Keep animations running
 P.displayAnimate();
  // Check orientation and flip display if needed (only in non-Pomodoro modes)
 // In Pomodoro mode, orientation is handled by checkPomodoroFlip()
 if (orientationCheckEnabled && currentMode != MODE_POMODORO && 
     currentMillis - lastOrientationCheck >= orientationCheckInterval) {
   lastOrientationCheck = currentMillis;
   checkOrientation();
 }
  // Ensure AP is always running (check every 10 seconds)
 if (currentMillis - lastAPCheck >= apCheckInterval) {
   lastAPCheck = currentMillis;
   if (WiFi.getMode() == WIFI_STA || WiFi.softAPIP().toString() == "0.0.0.0") {
     Serial.println("AP not active, restarting...");
     setupAP();
   }
 }
  // WiFi reconnection logic
 if (strlen(config.ssid) > 0 && WiFi.status() != WL_CONNECTED && wifiConnected) {
   if (currentMillis - lastWiFiReconnectAttempt >= wifiReconnectInterval) {
     lastWiFiReconnectAttempt = currentMillis;
     Serial.println("WiFi disconnected, attempting reconnect...");
     connectWiFi();
   }
 }
  // --- CLOCK MODE ---
 if (currentMode == MODE_CLOCK) {
   // Always try to update clock display if time is available
   if (getLocalTime(&timeinfo)) {
     // Update hourly weather (only if WiFi connected)
     if (wifiConnected) {
       char timeHour[3];
       strftime(timeHour, 3, "%H", &timeinfo);
       if (strcmp(timeLastHour, timeHour) != 0) {
         getWeather();
         strftime(timeLastHour, 3, "%H", &timeinfo);
       }
       
       // Daily NTP sync at 1:11:11 AM (background, no display)
       int currentHour = timeinfo.tm_hour;
       int currentMinute = timeinfo.tm_min;
       int currentSecond = timeinfo.tm_sec;
       int currentDay = timeinfo.tm_mday;
       
       // Check if it's 1:11:11 AM and we haven't synced today
       if (currentHour == 1 && currentMinute == 11 && currentSecond == 11 && currentDay != lastSyncedDay) {
         Serial.println("Scheduled NTP sync at 1:11:11 AM (background)");
         syncNTP();
         lastSyncedDay = currentDay;
       }
       
       // Fallback: If more than 24 hours since last sync, sync once
       if (lastNTPSync > 0 && (currentMillis - lastNTPSync > NTP_SYNC_INTERVAL)) {
         Serial.println("24-hour NTP sync interval reached (background)");
         syncNTP();
       }
     }
    
     // Blink colon every second
     if (currentMillis - previousMillisColon >= colonBlinkInterval) {
       previousMillisColon = currentMillis;
       colonVisible = !colonVisible;
       updateClockDisplay();
     }
     
     // Daily NTP sync at 1:11:11 AM (background, no display)
     if (wifiConnected) {
       int currentHour = timeinfo.tm_hour;
       int currentMinute = timeinfo.tm_min;
       int currentSecond = timeinfo.tm_sec;
       int currentDay = timeinfo.tm_mday;
       
       // Check if it's 1:11:11 AM and we haven't synced today
       if (currentHour == 1 && currentMinute == 11 && currentSecond == 11 && currentDay != lastSyncedDay) {
         Serial.println("Scheduled NTP sync at 1:11:11 AM");
         syncNTP();
         lastSyncedDay = currentDay;
       }
       
       // Fallback: If more than 24 hours since last sync, sync once
       if (lastNTPSync > 0 && (currentMillis - lastNTPSync > NTP_SYNC_INTERVAL)) {
         Serial.println("24-hour NTP sync interval reached");
         syncNTP();
       }
     }
   } else if (wifiConnected) {
     // Time not available but WiFi connected - try to sync once
     static unsigned long lastSyncAttempt = 0;
     if (lastSyncAttempt == 0 || (currentMillis - lastSyncAttempt > 60000)) { // Try once per minute max
       lastSyncAttempt = currentMillis;
       syncNTP();
     }
   }
 }
   // --- WEATHER MODE (auto-weather 10-sec, flip-weather 15-sec) ---
  else if (currentMode == MODE_WEATHER) {
    // Check if flip-triggered weather 15-second display time is up
    if (flipWeatherActive && (currentMillis - weatherDisplayStart >= FLIP_WEATHER_DISPLAY_MS)) {
      Serial.println("[FLIP-WEATHER] 15 seconds elapsed, returning to clock");
      flipWeatherActive = false;
      currentMode = MODE_CLOCK;
      P.displayClear();
      colonVisible = true;
      updateClockDisplay();
    }
    // Check if auto-weather 10-second display time is up
    else if (autoWeatherActive && (currentMillis - weatherDisplayStart >= AUTO_WEATHER_DISPLAY_MS)) {
      Serial.println("[AUTO-WEATHER] 10 seconds elapsed, returning to clock");
      autoWeatherActive = false;
      currentMode = MODE_CLOCK;
      P.displayClear();
      colonVisible = true;
      updateClockDisplay();
    }
    // Also return when scroll completes (for short weather messages)
    else if (P.getZoneStatus(0)) {
      if (autoWeatherActive || flipWeatherActive) {
        // Restart scroll to fill display time
        startWeatherScroll();
      } else {
        // Manual weather (shouldn't happen with new button logic, but safe)
        currentMode = MODE_CLOCK;
        P.displayClear();
        colonVisible = true;
        updateClockDisplay();
      }
    }
  }
  // --- BREAK MODE (water/posture breaks) ---
  else if (currentMode == MODE_BREAK) {
    // Check for flip to dismiss break
    if (currentMillis - lastOrientationCheck >= orientationCheckInterval) {
      lastOrientationCheck = currentMillis;
      // Check if device was flipped to dismiss
      int16_t accelZ = readMPU6050Accel(MPU6050_ACCEL_ZOUT_H);
      bool currentlyFlipped = (accelZ < -8000);
      if (currentlyFlipped != displayFlipped) {
        Serial.println("[BREAK] Flip detected - dismissing break");
        dismissBreak();
      }
    }

    // Check 30-second max timeout
    if (currentMillis - breakDisplayStart >= BREAK_DISPLAY_MAX_MS) {
      Serial.println("[BREAK] 30-second timeout reached");
      dismissBreak();
    }

    // Restart scroll if completed (keep showing until timeout or flip)
    if (P.getZoneStatus(0) && breakActive) {
      textEffect_t scrollDir = getScrollEffect();
      P.displayZoneText(0, breakDisplayText, PA_LEFT, 80, 0, scrollDir, scrollDir);
      P.displayAnimate();
    }
  }
   // --- POMODORO MODE ---
  else if (currentMode == MODE_POMODORO) {
    // Check for gestures every 200ms (faster than clock mode for responsiveness)
    if (currentMillis - lastOrientationCheck >= 200) {
      lastOrientationCheck = currentMillis;
      g_sensorRead = false;  // Reset so accelerometer is read fresh
      readPomodoroAccel();   // Read once, shared by both functions
      checkPomodoroLift();   // Check lift first (to lock flip during lift)
      checkPomodoroFlip();   // Then check flip
    }
   
   // Update timer display every second
   if (currentMillis - lastPomodoroUpdate >= 1000) {
     lastPomodoroUpdate = currentMillis;
     
     if (!pomodoroPaused && !pomodoroTimerExpired) {
       // Decrement timer
       if (pomodoroRemainingSeconds > 0) {
         pomodoroRemainingSeconds--;
       } else {
         // Timer expired
         pomodoroTimerExpired = true;
         pomodoroExpiredTime = currentMillis;
         Serial.println("Pomodoro timer expired!");
       }
     }
     
     // Update display
     updatePomodoroDisplay();
   }
   
   // Auto-return to clock mode after 5 minutes of inactivity when expired
   if (pomodoroTimerExpired && (currentMillis - pomodoroExpiredTime >= POMODORO_AUTO_RETURN_MS)) {
     Serial.println("Pomodoro auto-return: 5 minutes expired, returning to clock");
     currentMode = MODE_CLOCK;
     pomodoroTimerExpired = false;
     P.displayClear();
     colonVisible = true;
     updateClockDisplay();
   }
 }
   // --- CLOCK MODE: Check breaks and auto-weather ---
  if (currentMode == MODE_CLOCK) {
    // Check breaks first (highest priority)
    checkBreaks();

    // Check auto-weather (every 10 minutes, only if enabled and not in break)
    if (currentMode == MODE_CLOCK && wifiConnected && config.autoWeatherEnabled) {
      if (currentMillis - lastAutoWeather >= AUTO_WEATHER_INTERVAL) {
        lastAutoWeather = currentMillis;
        Serial.println("[AUTO-WEATHER] 10-minute interval reached");
        startAutoWeather();
      }
    }

    // Check auto-brightness (based on time of day)
    static unsigned long lastBrightnessCheck = 0;
    if (currentMillis - lastBrightnessCheck >= 60000) {  // Check every minute
      lastBrightnessCheck = currentMillis;
      checkAutoBrightness();
    }
  }
}


// --- CONFIGURATION FUNCTIONS ---


void loadConfig() {
 EEPROM.get(EEPROM_ADDR, config);
  
 // Check if config is valid and version matches
 bool configNeedsReset = false;
 
 if (!config.valid) {
   Serial.println("No saved config found, using safe defaults");
   configNeedsReset = true;
 } else if (config.version != CONFIG_VERSION) {
   Serial.print("Config version mismatch: EEPROM=");
   Serial.print(config.version);
   Serial.print(", Current=");
   Serial.print(CONFIG_VERSION);
   Serial.println(" - Preserving existing config data");
   // For now, preserve config even if version differs (backward compatibility)
   // Only reset if version is 0 (uninitialized)
   if (config.version == 0) {
     configNeedsReset = true;
   } else {
     // Update version to current but keep all other data
     config.version = CONFIG_VERSION;
     saveConfig();
     Serial.println("Config version updated, data preserved");
   }
 }
 
 if (configNeedsReset) {
   Serial.println("SECURITY: Please configure via web interface at http://192.168.4.1");
   memset(&config, 0, sizeof(config));
   config.version = CONFIG_VERSION; // Set current version
   
   // SECURITY: No hardcoded credentials - user must configure via web interface
   // For local development, create secrets.h from secrets_template.h and define USE_SECRETS
   #ifdef USE_SECRETS
     // Use secrets from secrets.h (local development only)
     #ifdef WIFI_SSID
       strcpy(config.ssid, WIFI_SSID);
       strcpy(config.password, WIFI_PASSWORD);
     #else
       strcpy(config.ssid, "");
       strcpy(config.password, "");
     #endif
     #ifdef WEATHER_API_KEY
       strcpy(config.weatherApiKey, WEATHER_API_KEY);
       strcpy(config.lat, WEATHER_LAT);
       strcpy(config.lon, WEATHER_LON);
       strcpy(config.cityName, WEATHER_CITY);
     #else
       strcpy(config.weatherApiKey, "");
       strcpy(config.lat, "");
       strcpy(config.lon, "");
       strcpy(config.cityName, "");
     #endif
     #ifdef GMT_OFFSET_SEC
       config.gmtOffset_sec = GMT_OFFSET_SEC;
       config.daylightOffset_sec = DAYLIGHT_OFFSET_SEC;
     #endif
   #else
     // Empty defaults - user must configure via web interface
     strcpy(config.ssid, "");
     strcpy(config.password, "");
     strcpy(config.weatherApiKey, "");
     strcpy(config.lat, "");
     strcpy(config.lon, "");
     strcpy(config.cityName, "");
   #endif
   
   // Safe defaults (not sensitive)
    strcpy(config.ntpServer, "pool.ntp.org");
    config.gmtOffset_sec = 0; // UTC - user should configure their timezone
    config.daylightOffset_sec = 0;
    config.brightness = 5;
    config.clockFormat24h = true; // Default to 24h format

    // Initialize break settings with defaults
    config.waterBreakEnabled = true;    // Water break ON by default
    config.waterIntervalMins = 15;      // 15 minutes default
    config.postureBreakEnabled = true;  // Posture break ON by default
    config.postureIntervalMins = 30;    // 30 minutes default

    // Initialize auto-weather settings
    config.autoWeatherEnabled = true;   // Auto-weather ON by default

    // Initialize auto-brightness settings
    config.autoBrightnessEnabled = true;  // Auto-brightness ON by default
    config.dayBrightness = 5;             // Day brightness (default 5)
    config.nightBrightness = 2;           // Night brightness (default 2)
    config.nightStartHour = 20;           // Night starts at 8 PM
    config.nightEndHour = 6;              // Night ends at 6 AM

    // Initialize Pomodoro phases with defaults
   config.pomodoroPhases[0].durationMinutes = 60;
   strcpy(config.pomodoroPhases[0].name, "FOCUS");
   config.pomodoroPhases[1].durationMinutes = 5;
   strcpy(config.pomodoroPhases[1].name, "BREAK");
   config.pomodoroPhases[2].durationMinutes = 30;
   strcpy(config.pomodoroPhases[2].name, "FOCUS");
   config.pomodoroPhases[3].durationMinutes = 10;
   strcpy(config.pomodoroPhases[3].name, "BREAK");
   // Try to connect with defaults, but don't mark as valid yet
   // User can save via web interface to make it permanent
 } else {
   Serial.println("Loaded saved configuration from EEPROM");
   Serial.print("SSID: ");
   Serial.println(config.ssid);
   Serial.print("Valid flag: ");
   Serial.println(config.valid ? "true" : "false");
   
   // Ensure Pomodoro phases are initialized even if loaded from EEPROM
   if (config.pomodoroPhases[0].durationMinutes == 0) {
     config.pomodoroPhases[0].durationMinutes = 60;
     strcpy(config.pomodoroPhases[0].name, "FOCUS");
     config.pomodoroPhases[1].durationMinutes = 5;
     strcpy(config.pomodoroPhases[1].name, "BREAK");
     config.pomodoroPhases[2].durationMinutes = 30;
     strcpy(config.pomodoroPhases[2].name, "FOCUS");
     config.pomodoroPhases[3].durationMinutes = 10;
     strcpy(config.pomodoroPhases[3].name, "BREAK");
   }
 }
 
 // Always ensure valid flag and version are set if we have any saved data
 // This prevents accidental reset on next boot and ensures version is current
 if (strlen(config.ssid) > 0 || strlen(config.weatherApiKey) > 0) {
   if (!config.valid || config.version != CONFIG_VERSION) {
     Serial.println("Warning: Config has data but flags incorrect. Correcting...");
     config.valid = true;
     config.version = CONFIG_VERSION;
     saveConfig(); // Save the corrected flags
   }
 }
 
 Serial.print("Loaded config version: ");
 Serial.print(config.version);
 Serial.print(" (Current: ");
 Serial.print(CONFIG_VERSION);
 Serial.println(")");
 
 if (config.valid && strlen(config.ssid) > 0) {
   Serial.println("✓ Valid configuration found - will persist across firmware updates");
 }
}


void saveConfig() {
 // Always set version and valid flag before saving
 config.version = CONFIG_VERSION;
 config.valid = true;
 
 EEPROM.put(EEPROM_ADDR, config);
 bool commitSuccess = EEPROM.commit();
 
 if (commitSuccess) {
   Serial.println("✓ Configuration saved to EEPROM (persists across firmware updates)");
   Serial.print("  Config version: ");
   Serial.println(config.version);
 } else {
   Serial.println("⚠ WARNING: EEPROM commit failed - config may not be saved!");
 }
}


void setupAP() {
  Serial.println("[AP] Starting Access Point setup...");

  // Show progress on display
  P.displayZoneText(0, "AP...", PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  P.displayAnimate();

  // Disconnect any existing connections first
  WiFi.disconnect(true);
  WiFi.softAPdisconnect(true);
  delay(300);

  // Set to AP_STA mode to allow both AP and Station simultaneously
  WiFi.mode(WIFI_AP_STA);
  delay(200);

  // Configure AP with channel 6 (less congested than 1)
  // Parameters: ssid, password, channel, hidden, max_connections
  bool apStarted = WiFi.softAP(ap_ssid, ap_password, 6, 0, 4);

  if (!apStarted) {
    Serial.println("[AP] ERROR: First attempt failed! Retrying...");
    P.displayZoneText(0, "RETRY", PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
    P.displayAnimate();
    delay(500);

    // Try with AP-only mode
    WiFi.mode(WIFI_AP);
    delay(300);
    apStarted = WiFi.softAP(ap_ssid, ap_password, 6, 0, 4);
  }

  if (!apStarted) {
    Serial.println("[AP] ERROR: Second attempt failed! Trying channel 1...");
    delay(500);
    apStarted = WiFi.softAP(ap_ssid, ap_password, 1, 0, 4);
  }

  if (apStarted) {
    // Configure AP IP address explicitly for reliability
    delay(100);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

    // Wait for AP to be fully ready (up to 3 seconds)
    int retries = 0;
    while (WiFi.softAPIP().toString() == "0.0.0.0" && retries < 30) {
      delay(100);
      retries++;
    }

    IPAddress IP = WiFi.softAPIP();
    if (IP.toString() != "0.0.0.0") {
      Serial.println("[AP] SUCCESS!");
      Serial.print("[AP] SSID: ");
      Serial.println(ap_ssid);
      Serial.print("[AP] IP: ");
      Serial.println(IP);
      Serial.print("[AP] MAC: ");
      Serial.println(WiFi.softAPmacAddress());
      Serial.print("[AP] Channel: ");
      Serial.println(WiFi.channel());

      // Start captive portal DNS server
      startCaptivePortal();

      P.displayZoneText(0, "AP OK", PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
      P.displayAnimate();
    } else {
      Serial.println("[AP] ERROR: AP started but IP not assigned!");
      P.displayZoneText(0, "AP IP?", PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
      P.displayAnimate();
    }
  } else {
    Serial.println("[AP] ERROR: AP failed to start after all retries!");
    P.displayZoneText(0, "AP ERR", PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
    P.displayAnimate();
  }
}


void connectWiFi() {
  Serial.println("\n=== WiFi Connection Attempt ===");
  Serial.print("SSID: ");
  Serial.println(config.ssid);
  Serial.print("SSID length: ");
  Serial.println(strlen(config.ssid));
  Serial.print("Password length: ");
  Serial.println(strlen(config.password));

  // Check if credentials are valid
  if (strlen(config.ssid) == 0) {
    Serial.println("✗ No SSID configured");
    wifiConnected = false;
    return;
  }

  // Disconnect any existing connection first
  WiFi.disconnect(true);
  delay(100);

  // Ensure we're in AP_STA mode so AP stays active
  WiFi.mode(WIFI_AP_STA);
  delay(200);

  // Ensure AP is running
  if (WiFi.softAPgetStationNum() >= 0) {
    Serial.println("[AP] AP mode active");
  } else {
    Serial.println("[AP] Restarting AP...");
    WiFi.softAP(ap_ssid, ap_password, 6); // Channel 6
  }
  delay(100);

  // Begin WiFi connection
  Serial.println("Calling WiFi.begin()...");
  WiFi.begin(config.ssid, config.password);

  unsigned long startAttempt = millis();
  int attempts = 0;
  wl_status_t lastStatus = WL_IDLE_STATUS;

  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 20000) {
    delay(500);
    showWiFiConnecting();

    wl_status_t currentStatus = WiFi.status();
    if (currentStatus != lastStatus) {
      Serial.print("\nWiFi status: ");
      switch (currentStatus) {
        case WL_IDLE_STATUS: Serial.println("IDLE"); break;
        case WL_NO_SSID_AVAIL: Serial.println("NO_SSID_AVAIL - Network not found!"); break;
        case WL_SCAN_COMPLETED: Serial.println("SCAN_COMPLETED"); break;
        case WL_CONNECTED: Serial.println("CONNECTED"); break;
        case WL_CONNECT_FAILED: Serial.println("CONNECT_FAILED - Wrong password?"); break;
        case WL_CONNECTION_LOST: Serial.println("CONNECTION_LOST"); break;
        case WL_DISCONNECTED: Serial.println("DISCONNECTED"); break;
        default: Serial.println(currentStatus); break;
      }
      lastStatus = currentStatus;
    } else {
      Serial.print(".");
    }
    attempts++;

    // Keep web server responsive
    server.handleClient();

    // Process DNS for captive portal
    if (captivePortalActive) {
      dnsServer.processNextRequest();
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\n✓ WiFi connected!");
    Serial.print("Station IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("AP still active at: ");
    Serial.println(WiFi.softAPIP());
    Serial.print("Signal strength (RSSI): ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    wifiConnected = false;
    Serial.println("\n✗ WiFi connection failed");
    Serial.print("Final status: ");
    switch (WiFi.status()) {
      case WL_NO_SSID_AVAIL: Serial.println("Network not found - check SSID"); break;
      case WL_CONNECT_FAILED: Serial.println("Connection failed - check password"); break;
      default: Serial.println("Unknown error"); break;
    }
    Serial.println("AP mode remains active for configuration");
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  }
  Serial.println("===============================\n");
}


// --- WEB SERVER HANDLERS ---


void handleRoot() {
 server.send(200, "text/html", htmlPage);
}


void handleConfig() {
  DynamicJsonDocument doc(1024);  // Reduced - no reminders array needed
  doc["valid"] = config.valid;
  doc["ssid"] = config.ssid;
  doc["password"] = config.password;
  doc["ntpServer"] = config.ntpServer;
  doc["gmtOffset_sec"] = config.gmtOffset_sec;
  doc["daylightOffset_sec"] = config.daylightOffset_sec;
  doc["weatherApiKey"] = config.weatherApiKey;
  doc["lat"] = config.lat;
  doc["lon"] = config.lon;
  doc["cityName"] = config.cityName;
  doc["brightness"] = config.brightness;
  doc["clockFormat24h"] = config.clockFormat24h;

  // Add break settings
  doc["waterBreakEnabled"] = config.waterBreakEnabled;
  doc["waterIntervalMins"] = config.waterIntervalMins;
  doc["postureBreakEnabled"] = config.postureBreakEnabled;
  doc["postureIntervalMins"] = config.postureIntervalMins;

  // Add auto-weather setting
  doc["autoWeatherEnabled"] = config.autoWeatherEnabled;

  // Add auto-brightness settings
  doc["autoBrightnessEnabled"] = config.autoBrightnessEnabled;
  doc["dayBrightness"] = config.dayBrightness;
  doc["nightBrightness"] = config.nightBrightness;
  doc["nightStartHour"] = config.nightStartHour;
  doc["nightEndHour"] = config.nightEndHour;

  // Add Pomodoro phases array
 JsonArray pomodoroPhases = doc.createNestedArray("pomodoroPhases");
 for (int i = 0; i < 4; i++) {
   JsonObject phase = pomodoroPhases.createNestedObject();
   phase["durationMinutes"] = config.pomodoroPhases[i].durationMinutes;
   phase["name"] = config.pomodoroPhases[i].name;
 }
  String response;
 serializeJson(doc, response);
 server.send(200, "application/json", response);
}


void handleSave() {
  Serial.println("\n=== Save Request Received ===");

  // Store old WiFi credentials to check if they changed
  char oldSSID[32];
  char oldPassword[64];
  strncpy(oldSSID, config.ssid, sizeof(oldSSID));
  oldSSID[sizeof(oldSSID) - 1] = '\0';
  strncpy(oldPassword, config.password, sizeof(oldPassword));
  oldPassword[sizeof(oldPassword) - 1] = '\0';

  bool wifiChanged = false;

  if (server.hasArg("ssid")) {
    String newSSID = server.arg("ssid");
    Serial.print("Received SSID: '");
    Serial.print(newSSID);
    Serial.print("' (length: ");
    Serial.print(newSSID.length());
    Serial.println(")");
    if (strcmp(config.ssid, newSSID.c_str()) != 0) {
      wifiChanged = true;
      Serial.println("  -> SSID changed!");
    }
    strncpy(config.ssid, newSSID.c_str(), sizeof(config.ssid) - 1);
    config.ssid[sizeof(config.ssid) - 1] = '\0';
  } else {
    Serial.println("Warning: No SSID in request");
  }

  if (server.hasArg("password")) {
    String newPassword = server.arg("password");
    Serial.print("Received password length: ");
    Serial.println(newPassword.length());
    if (strcmp(config.password, newPassword.c_str()) != 0) {
      wifiChanged = true;
      Serial.println("  -> Password changed!");
    }
    strncpy(config.password, newPassword.c_str(), sizeof(config.password) - 1);
    config.password[sizeof(config.password) - 1] = '\0';
  } else {
    Serial.println("Warning: No password in request");
  }

  Serial.print("WiFi changed: ");
  Serial.println(wifiChanged ? "YES" : "NO");
  if (server.hasArg("ntpServer")) {
   strncpy(config.ntpServer, server.arg("ntpServer").c_str(), sizeof(config.ntpServer) - 1);
   config.ntpServer[sizeof(config.ntpServer) - 1] = '\0';
 }
  if (server.hasArg("gmtOffset")) {
   config.gmtOffset_sec = server.arg("gmtOffset").toInt();
 }
  if (server.hasArg("daylightOffset")) {
   config.daylightOffset_sec = server.arg("daylightOffset").toInt();
 }
  if (server.hasArg("apiKey")) {
   strncpy(config.weatherApiKey, server.arg("apiKey").c_str(), sizeof(config.weatherApiKey) - 1);
   config.weatherApiKey[sizeof(config.weatherApiKey) - 1] = '\0';
 }
  if (server.hasArg("lat")) {
   strncpy(config.lat, server.arg("lat").c_str(), sizeof(config.lat) - 1);
   config.lat[sizeof(config.lat) - 1] = '\0';
 }
  if (server.hasArg("lon")) {
   strncpy(config.lon, server.arg("lon").c_str(), sizeof(config.lon) - 1);
   config.lon[sizeof(config.lon) - 1] = '\0';
 }
  if (server.hasArg("cityName")) {
   strncpy(config.cityName, server.arg("cityName").c_str(), sizeof(config.cityName) - 1);
   config.cityName[sizeof(config.cityName) - 1] = '\0';
 }
  if (server.hasArg("brightness")) {
   config.brightness = server.arg("brightness").toInt();
   if (config.brightness < 0) config.brightness = 0;
   if (config.brightness > 15) config.brightness = 15;
   P.setIntensity(config.brightness);
 }
   if (server.hasArg("clockFormat")) {
    config.clockFormat24h = (server.arg("clockFormat") == "24");
  }

  // Handle break settings
  config.waterBreakEnabled = server.hasArg("waterBreakEnabled");
  if (server.hasArg("waterInterval")) {
    uint8_t val = server.arg("waterInterval").toInt();
    if (val == 15 || val == 20 || val == 30) {
      config.waterIntervalMins = val;
    }
  }

  config.postureBreakEnabled = server.hasArg("postureBreakEnabled");
  if (server.hasArg("postureInterval")) {
    uint8_t val = server.arg("postureInterval").toInt();
    if (val == 30 || val == 45 || val == 60) {
      config.postureIntervalMins = val;
    }
  }

  Serial.print("Break settings: Water=");
  Serial.print(config.waterBreakEnabled ? "ON" : "OFF");
  Serial.print(" (");
  Serial.print(config.waterIntervalMins);
  Serial.print("min), Posture=");
  Serial.print(config.postureBreakEnabled ? "ON" : "OFF");
  Serial.print(" (");
  Serial.print(config.postureIntervalMins);
  Serial.println("min)");

  // Handle auto-weather setting
  config.autoWeatherEnabled = server.hasArg("autoWeatherEnabled");
  Serial.print("Auto-weather: ");
  Serial.println(config.autoWeatherEnabled ? "ON" : "OFF");

  // Handle auto-brightness settings
  config.autoBrightnessEnabled = server.hasArg("autoBrightnessEnabled");
  if (server.hasArg("dayBrightness")) {
    uint8_t val = server.arg("dayBrightness").toInt();
    if (val <= 15) {
      config.dayBrightness = val;
    }
  }
  if (server.hasArg("nightBrightness")) {
    uint8_t val = server.arg("nightBrightness").toInt();
    if (val <= 15) {
      config.nightBrightness = val;
    }
  }
  if (server.hasArg("nightStartHour")) {
    uint8_t val = server.arg("nightStartHour").toInt();
    if (val <= 23) {
      config.nightStartHour = val;
    }
  }
  if (server.hasArg("nightEndHour")) {
    uint8_t val = server.arg("nightEndHour").toInt();
    if (val <= 23) {
      config.nightEndHour = val;
    }
  }

  Serial.print("Auto-brightness: ");
  Serial.print(config.autoBrightnessEnabled ? "ON" : "OFF");
  Serial.print(" (Day=");
  Serial.print(config.dayBrightness);
  Serial.print(", Night=");
  Serial.print(config.nightBrightness);
  Serial.print(", ");
  Serial.print(config.nightStartHour);
  Serial.print(":00-");
  Serial.print(config.nightEndHour);
  Serial.println(":00)");

  // Apply brightness change immediately if auto-brightness enabled
  if (config.autoBrightnessEnabled) {
    checkAutoBrightness();
  } else {
    // If auto-brightness disabled, apply manual brightness setting
    P.setIntensity(config.brightness);
  }

  // Handle Pomodoro phases (0-3)
 for (int i = 0; i < 4; i++) {
   String durationArg = "pomodoroPhase" + String(i) + "Duration";
   
   if (server.hasArg(durationArg)) {
     int duration = server.arg(durationArg).toInt();
     if (duration >= 1 && duration <= 120) {
       config.pomodoroPhases[i].durationMinutes = duration;
     }
   }
   
   // Keep default phase names (not user-configurable)
   if (i == 0 || i == 2) {
     strcpy(config.pomodoroPhases[i].name, "FOCUS");
   } else {
     strcpy(config.pomodoroPhases[i].name, "BREAK");
   }
   
   Serial.print("Saved Pomodoro phase ");
   Serial.print(i);
   Serial.print(": ");
   Serial.print(config.pomodoroPhases[i].durationMinutes);
   Serial.print(" min");
   Serial.println();
 }
  // Save config (saveConfig() will set version and valid flag automatically)
  saveConfig();
  
  // Verify save was successful
  Config testConfig;
  EEPROM.get(EEPROM_ADDR, testConfig);
  if (testConfig.valid && strcmp(testConfig.ssid, config.ssid) == 0) {
    Serial.println("✓ Configuration verified and saved successfully");
  } else {
    Serial.println("⚠ Warning: Configuration save verification failed - retrying");
    // Try saving again
    EEPROM.put(EEPROM_ADDR, config);
    EEPROM.commit();
  }
  
  Serial.println("\n=== Configuration Saved ===");
 Serial.print("SSID: ");
 Serial.println(config.ssid);
 Serial.print("NTP Server: ");
 Serial.println(config.ntpServer);
 Serial.print("City: ");
 Serial.println(config.cityName);
  // Apply immediate settings (brightness, clock format) - no delay needed
  if (server.hasArg("brightness")) {
    P.setIntensity(config.brightness);
    Serial.print("Brightness updated immediately: ");
    Serial.println(config.brightness);
  }
  
  // Update clock display immediately if format changed
  if (server.hasArg("clockFormat")) {
    if (getLocalTime(&timeinfo)) {
      updateClockDisplay();
    }
  }
  
  // Check if time settings changed
 bool timeSettingsChanged = false;
 if (server.hasArg("ntpServer") || server.hasArg("gmtOffset") || server.hasArg("daylightOffset")) {
   timeSettingsChanged = true;
 }
  // Check if weather settings changed
 bool weatherSettingsChanged = false;
 if (server.hasArg("apiKey") || server.hasArg("lat") || server.hasArg("lon") || server.hasArg("cityName")) {
   weatherSettingsChanged = true;
 }
  
  // Send HTTP response IMMEDIATELY - don't wait for async operations
  Serial.println("==========================\n");
  server.send(200, "text/plain", "success");
  
  // Now schedule async operations (these will happen in loop())
  if (wifiChanged && strlen(config.ssid) > 0) {
    Serial.println("WiFi credentials changed - scheduling async reconnect...");
    pendingWiFiReconnect = true;
    pendingWiFiReconnectTime = millis() + WIFI_RECONNECT_DELAY;
    // Also schedule NTP and weather after WiFi connects
    pendingNTPSync = true;
    pendingWeatherUpdate = true;
  } else {
    // WiFi didn't change, but check if we need to update time or weather
    if (WiFi.status() == WL_CONNECTED) {
      if (timeSettingsChanged) {
        Serial.println("Time settings changed - scheduling async NTP sync...");
        pendingNTPSync = true;
      }
    
      if (weatherSettingsChanged) {
        Serial.println("Weather settings changed - scheduling async weather update...");
        pendingWeatherUpdate = true;
      }
    }
  }
}


void handleStatus() {
 DynamicJsonDocument doc(256);
  if (WiFi.status() == WL_CONNECTED) {
   doc["ip"] = WiFi.localIP().toString();
   doc["wifi"] = config.ssid;
 } else if (WiFi.getMode() == WIFI_AP) {
   doc["ip"] = WiFi.softAPIP().toString();
   doc["wifi"] = "AP Mode";
 } else {
   doc["ip"] = "Not Connected";
   doc["wifi"] = "Disconnected";
 }
  String response;
 serializeJson(doc, response);
 server.send(200, "application/json", response);
}


// --- CAPTIVE PORTAL HANDLERS ---

void handleCaptivePortalRedirect() {
  String redirectUrl = "http://192.168.4.1/";

  Serial.print("[CAPTIVE] Redirecting ");
  Serial.print(server.uri());
  Serial.print(" to ");
  Serial.println(redirectUrl);

  server.sendHeader("Location", redirectUrl, true);
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.send(302, "text/plain", "");
}

void handleAndroidCaptive() {
  Serial.println("[CAPTIVE] Android detection: /generate_204");
  handleCaptivePortalRedirect();
}

void handleAppleCaptive() {
  Serial.println("[CAPTIVE] Apple detection: /hotspot-detect.html");

  // Apple expects HTML with redirect
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta http-equiv='refresh' content='0; url=http://192.168.4.1/'>";
  html += "</head><body>";
  html += "<a href='http://192.168.4.1/'>Configure Clock</a>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleWindowsCaptive() {
  Serial.print("[CAPTIVE] Windows detection: ");
  Serial.println(server.uri());
  handleCaptivePortalRedirect();
}

void handleFirefoxCaptive() {
  Serial.println("[CAPTIVE] Firefox detection: /success.txt");
  handleCaptivePortalRedirect();
}

void handleNotFound() {
  String uri = server.uri();
  String host = server.hostHeader();

  Serial.print("[CAPTIVE] NotFound: ");
  Serial.print(uri);
  Serial.print(" Host: ");
  Serial.println(host);

  // If request is not for our IP/hostname, redirect to config page
  if (host != "192.168.4.1" &&
      host != "deskmate.local" &&
      host != WiFi.softAPIP().toString()) {
    handleCaptivePortalRedirect();
    return;
  }

  // For unknown paths, also redirect
  handleCaptivePortalRedirect();
}

void startCaptivePortal() {
  if (captivePortalActive) {
    Serial.println("[DNS] Captive portal already active");
    return;
  }

  Serial.println("[DNS] Starting captive portal DNS server...");

  // Start DNS server - redirect all DNS queries to our AP IP
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  bool dnsStarted = dnsServer.start(DNS_PORT, "*", apIP);

  if (dnsStarted) {
    captivePortalActive = true;
    Serial.println("[DNS] DNS server started on port 53");
    Serial.println("[DNS] All DNS queries will resolve to 192.168.4.1");
  } else {
    Serial.println("[DNS] ERROR: DNS server failed to start!");
  }

  // Start mDNS for deskmate.local
  if (MDNS.begin("deskmate")) {
    mdnsStarted = true;
    MDNS.addService("http", "tcp", 80);
    Serial.println("[mDNS] Started: http://deskmate.local");
  } else {
    Serial.println("[mDNS] Failed to start mDNS");
  }
}

void stopCaptivePortal() {
  if (captivePortalActive) {
    dnsServer.stop();
    captivePortalActive = false;
    Serial.println("[DNS] Captive portal DNS server stopped");
  }

  if (mdnsStarted) {
    MDNS.end();
    mdnsStarted = false;
    Serial.println("[mDNS] Stopped");
  }
}


// --- CLOCK FUNCTIONS ---


void handleButton() {
  bool currentState = (digitalRead(MODE_BUTTON_PIN) == LOW);
  if (currentState && !buttonPressed) {
    if (millis() - lastButtonPress > debounceDelay) {
      lastButtonPress = millis();
      buttonPressed = true;

      // New cycling: Clock <-> Pomodoro only
      // Weather/Break modes -> return to Clock
      if (currentMode == MODE_CLOCK) {
        Serial.println("Button: Clock -> Pomodoro");
        startPomodoroMode();
      } else if (currentMode == MODE_POMODORO) {
        Serial.println("Button: Pomodoro -> Clock");
        currentMode = MODE_CLOCK;
        P.displayClear();
        colonVisible = true;
        updateClockDisplay();
      } else if (currentMode == MODE_WEATHER || currentMode == MODE_BREAK) {
        // Exit auto-weather or break, return to clock
        Serial.println("Button: Weather/Break -> Clock");
        autoWeatherActive = false;
        breakActive = false;
        currentMode = MODE_CLOCK;
        P.displayClear();
        colonVisible = true;
        updateClockDisplay();
      }
    }
  }
  if (!currentState) {
   buttonPressed = false;
  }
}


void updateClockDisplay() {
 static char timeText[25];
 static char dayText[4];
  if (getLocalTime(&timeinfo)) {
   // Get day name (3 chars)
   strftime(dayText, sizeof(dayText), "%a", &timeinfo);
  
   // Convert day to uppercase
   for (int i = 0; dayText[i]; i++) {
     if (dayText[i] >= 'a' && dayText[i] <= 'z') {
       dayText[i] = dayText[i] - 32;
     }
   }
  
   // Format time based on 12h/24h setting
   int hour = timeinfo.tm_hour;
   bool isPM = false;
  
   if (!config.clockFormat24h) {
     // 12-hour format
     isPM = (hour >= 12);
     if (hour == 0) hour = 12;
     else if (hour > 12) hour = hour - 12;
   }
  
   // Format: "MON14:30" or "MON2:30*" (with PM dot)
   // Minimal gap: Only character spacing (total 2 LEDs gap between DAY and TIME)
   if (config.clockFormat24h) {
     snprintf(timeText, sizeof(timeText), "%s%02d%c%02d",
              dayText, hour, colonVisible ? ':' : ' ', timeinfo.tm_min);
   } else {
     // 12h format: add PM dot if PM
     snprintf(timeText, sizeof(timeText), "%s%d%c%02d%s",
              dayText, hour, colonVisible ? ':' : ' ', timeinfo.tm_min, isPM ? "*" : "");
   }
  
   P.addChar(':', colonVisible ? colonOn : colonOff);
  
   // Render text normally - don't reverse the string!
   // Apply 180° flip using MD_Parola zone effects
   if (displayFlipped) {
     // For 180° flip: enable both horizontal and vertical flip
     // PA_FLIP_LR = horizontal flip, PA_FLIP_UD = vertical flip
     P.setZoneEffect(0, true, PA_FLIP_LR);  // Enable horizontal flip
     P.setZoneEffect(0, true, PA_FLIP_UD);   // Enable vertical flip
   } else {
     P.setZoneEffect(0, false, PA_FLIP_LR);  // Disable horizontal flip
     P.setZoneEffect(0, false, PA_FLIP_UD);   // Disable vertical flip
   }
  
   P.displayZoneText(0, timeText, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  
   Serial.print("Clock: ");
   Serial.print(timeText);
   Serial.print(" [Flipped: ");
   Serial.print(displayFlipped ? "YES" : "NO");
   Serial.println("]");
 } else {
   // Time not available - show status
   if (wifiConnected) {
     P.displayZoneText(0, "NO TIME", PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
   } else {
     P.displayZoneText(0, "NO WIFI", PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
   }
 }
}


void startWeatherScroll() {
 currentMode = MODE_WEATHER;
 P.displayClear();
  // Apply flip effect if needed
 if (displayFlipped) {
   P.setZoneEffect(0, true, PA_FLIP_LR);  // Enable horizontal flip
   P.setZoneEffect(0, true, PA_FLIP_UD);   // Enable vertical flip
 } else {
   P.setZoneEffect(0, false, PA_FLIP_LR);  // Disable horizontal flip
   P.setZoneEffect(0, false, PA_FLIP_UD);   // Disable vertical flip
 }
  // Show appropriate message based on data availability
 const char* displayText;
 if (!weatherDataReady || strlen(weatherData) == 0 || 
     strcmp(weatherData, "NO WIFI") == 0 || 
     strcmp(weatherData, "NO CONFIG") == 0 ||
     strcmp(weatherData, "API ERROR") == 0 ||
     strcmp(weatherData, "PARSE ERROR") == 0) {
   displayText = "PLEASE CONFIGURE WEATHER DATA    ";
   Serial.println("Weather data not available - showing configuration message");
 } else {
   displayText = weatherData;
   Serial.print("Weather scroll: ");
   Serial.println(weatherData);
 }
   // Slower, smoother scroll speed (100-120 is good for readability)
  // Use correct scroll direction based on flip state
  textEffect_t scrollDir = getScrollEffect();
  P.displayZoneText(0, displayText, PA_LEFT, 100, 0, scrollDir, scrollDir);
  P.displayAnimate();
}


// --- AUTO-WEATHER FUNCTION ---
void startAutoWeather() {
  if (currentMode != MODE_CLOCK) return;  // Only from clock mode

  previousMode = MODE_CLOCK;
  currentMode = MODE_WEATHER;
  autoWeatherActive = true;
  flipWeatherActive = false;
  weatherDisplayStart = millis();

  Serial.println("[AUTO-WEATHER] Starting 10-second weather display");
  startWeatherScroll();
}


// --- FLIP-TRIGGERED WEATHER (15 seconds) ---
void startFlipWeather() {
  if (currentMode != MODE_CLOCK) return;  // Only from clock mode

  previousMode = MODE_CLOCK;
  currentMode = MODE_WEATHER;
  flipWeatherActive = true;
  autoWeatherActive = false;
  weatherDisplayStart = millis();

  Serial.println("[FLIP-WEATHER] Starting 15-second weather display");
  startWeatherScroll();
}


// --- AUTO-BRIGHTNESS FUNCTION ---
void checkAutoBrightness() {
  if (!config.autoBrightnessEnabled) return;
  if (!getLocalTime(&timeinfo)) return;

  int currentHour = timeinfo.tm_hour;
  bool isNight = false;

  // Handle night time calculation (night can span midnight)
  if (config.nightStartHour > config.nightEndHour) {
    // Night spans midnight (e.g., 20:00 to 06:00)
    isNight = (currentHour >= config.nightStartHour || currentHour < config.nightEndHour);
  } else {
    // Night doesn't span midnight (e.g., 22:00 to 05:00 wouldn't work, but handle anyway)
    isNight = (currentHour >= config.nightStartHour && currentHour < config.nightEndHour);
  }

  // Apply appropriate brightness
  static bool wasNight = false;
  static bool firstCheck = true;

  if (firstCheck || isNight != wasNight) {
    uint8_t targetBrightness = isNight ? config.nightBrightness : config.dayBrightness;
    P.setIntensity(targetBrightness);

    Serial.print("[AUTO-BRIGHTNESS] ");
    Serial.print(isNight ? "Night" : "Day");
    Serial.print(" mode - brightness set to ");
    Serial.println(targetBrightness);

    wasNight = isNight;
    firstCheck = false;
  }
}


// --- BREAK HANDLING (shared for water/posture) ---
void handleBreak(BreakType type) {
  // Only trigger breaks in clock mode
  if (currentMode != MODE_CLOCK) return;

  activeBreakType = type;
  breakActive = true;
  breakDisplayStart = millis();
  previousMode = MODE_CLOCK;
  currentMode = MODE_BREAK;

  P.displayClear();

  // Set message based on break type
  if (type == WATER_BREAK) {
    strcpy(breakDisplayText, "TIME FOR WATER!    ");
    lastWaterBreak = millis();
    Serial.println("[BREAK] Water break triggered");
  } else {
    strcpy(breakDisplayText, "CHECK YOUR POSTURE!    ");
    lastPostureBreak = millis();
    Serial.println("[BREAK] Posture break triggered");
  }

  // Apply flip effect if needed
  if (displayFlipped) {
    P.setZoneEffect(0, true, PA_FLIP_LR);
    P.setZoneEffect(0, true, PA_FLIP_UD);
  } else {
    P.setZoneEffect(0, false, PA_FLIP_LR);
    P.setZoneEffect(0, false, PA_FLIP_UD);
  }

  // Start scrolling display with correct direction based on flip state
  textEffect_t scrollDir = getScrollEffect();
  P.displayZoneText(0, breakDisplayText, PA_LEFT, 80, 0, scrollDir, scrollDir);
  P.displayAnimate();
}


void checkBreaks() {
  // Only check breaks in clock mode
  if (currentMode != MODE_CLOCK) return;

  unsigned long now = millis();

  // Check water break (higher priority - check first)
  if (config.waterBreakEnabled && config.waterIntervalMins > 0) {
    uint32_t intervalMs = (uint32_t)config.waterIntervalMins * 60000UL;
    if (now - lastWaterBreak >= intervalMs) {
      handleBreak(WATER_BREAK);
      return;  // Only one break at a time
    }
  }

  // Check posture break
  if (config.postureBreakEnabled && config.postureIntervalMins > 0) {
    uint32_t intervalMs = (uint32_t)config.postureIntervalMins * 60000UL;
    if (now - lastPostureBreak >= intervalMs) {
      handleBreak(POSTURE_BREAK);
      return;
    }
  }
}


void dismissBreak() {
  if (!breakActive) return;

  Serial.println("[BREAK] Dismissed");
  breakActive = false;
  currentMode = MODE_CLOCK;
  P.displayClear();
  colonVisible = true;
  updateClockDisplay();
}


void showWiFiConnecting() {
 static int dotCount = 1;
 static unsigned long lastUpdate = 0;
 const long updateInterval = 500;
  if (millis() - lastUpdate >= updateInterval) {
   lastUpdate = millis();
  
   String statusText = "WIFI";
   for (int i = 0; i < dotCount; i++) {
     statusText += ".";
   }
  
   P.displayZoneText(0, statusText.c_str(), PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
   P.displayAnimate();
  
   dotCount = (dotCount % 3) + 1;
 }
}


void syncNTP() {
 // Only sync if WiFi is connected
 if (WiFi.status() != WL_CONNECTED) {
   Serial.println("NTP sync skipped - WiFi not connected");
   return;
 }
 
 Serial.println("Syncing NTP (background)...");
 configTime(config.gmtOffset_sec, config.daylightOffset_sec, config.ntpServer);
 delay(500); // Reduced delay
  
 int retry = 0;
 while (!getLocalTime(&timeinfo) && retry < 5) { // Reduced retries
   delay(300);
   retry++;
   // Keep web server responsive during sync
   server.handleClient();
 }
  
 if (retry < 5) {
   Serial.println("NTP sync successful");
   lastNTPSync = millis();
   // Update tracking variables
   strftime(timeYesterday, 4, "%a", &timeinfo);
   strftime(timeLastHour, 3, "%H", &timeinfo);
   lastSyncedDay = timeinfo.tm_mday;
 } else {
   Serial.println("NTP sync failed - continuing with current time");
 }
}


void getWeather() {
 Serial.println("Fetching weather...");
  if (WiFi.status() != WL_CONNECTED) {
   snprintf(weatherData, sizeof(weatherData), "NO WIFI");
   weatherDataReady = true;
   return;
 }
  if (strlen(config.weatherApiKey) == 0 || strlen(config.lat) == 0 || strlen(config.lon) == 0) {
   snprintf(weatherData, sizeof(weatherData), "NO CONFIG");
   weatherDataReady = true;
   return;
 }
  HTTPClient http;
 http.setTimeout(10000);
 String url = "http://api.openweathermap.org/data/2.5/weather?lat=" + String(config.lat) +
              "&lon=" + String(config.lon) + "&units=metric&appid=" + String(config.weatherApiKey);
 http.begin(url);
  int httpCode = http.GET();
  if (httpCode == 200) {
   String jsonData = http.getString();
   DynamicJsonDocument doc(2048);
  
   DeserializationError error = deserializeJson(doc, jsonData);
  
   if (!error) {
     const char* description = doc["weather"][0]["main"] | "Unknown";
     float temp = doc["main"]["temp"] | 0.0;
     int humidity = doc["main"]["humidity"] | 0;
    
     String desc = String(description);
     if (desc == "Thunderstorm") desc = "TSTORM";
     else if (desc == "Drizzle") desc = "DRIZZLE";
     else if (desc == "Clouds") desc = "CLOUDY";
     else if (desc == "Clear") desc = "CLEAR";
     else if (desc == "Rain") desc = "RAINY";
     else if (desc == "Mist") desc = "MIST";
     else if (desc == "Fog") desc = "FOG";
     else if (desc == "Snow") desc = "SNOW";
     else desc.toUpperCase();
    
     // Compact format: "PUNE 23$C CLOUDY HUM. 45%    "
     snprintf(weatherData, sizeof(weatherData),
              "%s %.0f$C %s HUM. %d%%    ",
              config.cityName, temp, desc.c_str(), humidity);
    
     // Convert to uppercase
     for (int i = 0; weatherData[i]; i++) {
       if (weatherData[i] >= 'a' && weatherData[i] <= 'z') {
         weatherData[i] = weatherData[i] - 32;
       }
     }
    
     weatherDataReady = true;
     Serial.print("Weather: ");
     Serial.println(weatherData);
   } else {
     snprintf(weatherData, sizeof(weatherData), "PARSE ERROR");
     weatherDataReady = false;
   }
 } else {
   snprintf(weatherData, sizeof(weatherData), "API ERROR");
   weatherDataReady = false;
 }
  http.end();
}


// --- POMODORO FUNCTIONS ---


void startPomodoroMode() {
  Serial.println("Starting Pomodoro mode");
  currentMode = MODE_POMODORO;
  currentPomodoroPhase = -1; // Will be set to 0 by advancePomodoroPhase
  pomodoroPaused = false;
  pomodoroTimerExpired = false;
  pomodoroBlinkState = false;
  deviceLifted = false;
  flipDetected = false;

  // Show "POMODORO" text first so user knows they're in Pomodoro mode
  P.displayClear();

  // Apply flip effect if needed
  if (displayFlipped) {
    P.setZoneEffect(0, true, PA_FLIP_LR);
    P.setZoneEffect(0, true, PA_FLIP_UD);
  } else {
    P.setZoneEffect(0, false, PA_FLIP_LR);
    P.setZoneEffect(0, false, PA_FLIP_UD);
  }

  P.displayZoneText(0, "POMODORO", PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  P.displayAnimate();
  Serial.println("Showing POMODORO text...");

  // Keep display for 1.5 seconds, but keep server responsive
  unsigned long showStart = millis();
  while (millis() - showStart < 1500) {
    server.handleClient();
    if (captivePortalActive) {
      dnsServer.processNextRequest();
    }
    delay(10);
  }

  // Start first phase (advancePomodoroPhase will increment to 0)
  advancePomodoroPhase();
}


void advancePomodoroPhase() {
  // Stop current phase and move to next
  Serial.print("Advancing Pomodoro phase from ");
  Serial.print(currentPomodoroPhase);

  // Move to next phase (cycle)
  currentPomodoroPhase = (currentPomodoroPhase + 1) % 4;

  Serial.print(" to ");
  Serial.println(currentPomodoroPhase);

  // Get phase duration
  int durationMinutes = config.pomodoroPhases[currentPomodoroPhase].durationMinutes;
  pomodoroRemainingSeconds = durationMinutes * 60;
  pomodoroStartTime = millis();
  pomodoroPaused = false;
  pomodoroTimerExpired = false;
  pomodoroBlinkState = true;  // Start with display ON

  // Start phase start blink animation (3 blinks)
  phaseStartBlinking = true;
  phaseStartBlinkCount = 0;
  phaseStartBlinkTime = millis();

  Serial.print("Phase: ");
  Serial.print(config.pomodoroPhases[currentPomodoroPhase].name);
  Serial.print(" (");
  Serial.print(durationMinutes);
  Serial.println(" minutes)");
  Serial.println("Starting 3-blink animation...");

  // Update display immediately (will show timer, then blink animation runs in loop)
  updatePomodoroDisplay();
}


void updatePomodoroDisplay() {
  static char timerText[10];
  unsigned long currentMillis = millis();

  // Phase start blink animation (3 blinks = 6 toggles)
  if (phaseStartBlinking) {
    if (currentMillis - phaseStartBlinkTime >= 300) {  // 300ms per toggle (faster blink)
      phaseStartBlinkTime = currentMillis;
      phaseStartBlinkCount++;
      pomodoroBlinkState = !pomodoroBlinkState;

      if (phaseStartBlinkCount >= PHASE_START_BLINK_TOTAL) {
        // Animation complete
        phaseStartBlinking = false;
        pomodoroBlinkState = true;  // End with display ON
        Serial.println("Phase start blink animation complete");
      }
    }

    if (!pomodoroBlinkState) {
      P.displayClear();
      return;
    }
  }

  // If timer expired, blink display
  if (pomodoroTimerExpired) {
    if (currentMillis - lastPomodoroBlink >= POMODORO_BLINK_INTERVAL) {
      lastPomodoroBlink = currentMillis;
      pomodoroBlinkState = !pomodoroBlinkState;
    }

    if (!pomodoroBlinkState) {
      // Blank during blink
      P.displayClear();
      return;
    }
  }

  // If paused, blink current time
  if (pomodoroPaused) {
    if (currentMillis - lastPomodoroBlink >= POMODORO_BLINK_INTERVAL) {
      lastPomodoroBlink = currentMillis;
      pomodoroBlinkState = !pomodoroBlinkState;
    }

    if (!pomodoroBlinkState) {
      P.displayClear();
      return;
    }
  }

  // Calculate minutes and seconds
  int minutes = pomodoroRemainingSeconds / 60;
  int seconds = pomodoroRemainingSeconds % 60;

  // Format as MM:SS (ensure colon is visible)
  snprintf(timerText, sizeof(timerText), "%02d:%02d", minutes, seconds);

  // Ensure colon character is properly defined for display
  P.addChar(':', colonOn);

  // Apply flip effect if needed
  if (displayFlipped) {
    P.setZoneEffect(0, true, PA_FLIP_LR);
    P.setZoneEffect(0, true, PA_FLIP_UD);
  } else {
    P.setZoneEffect(0, false, PA_FLIP_LR);
    P.setZoneEffect(0, false, PA_FLIP_UD);
  }

  // Display centered
  P.displayZoneText(0, timerText, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
}


void pausePomodoro() {
 if (!pomodoroPaused) {
   Serial.println("Pomodoro paused");
   pomodoroPaused = true;
   pomodoroBlinkState = true;
   lastPomodoroBlink = millis();
 }
}


void resumePomodoro() {
 if (pomodoroPaused) {
   Serial.println("Pomodoro resumed");
   pomodoroPaused = false;
   pomodoroBlinkState = false;
   // Timer continues from where it was
 }
}


// ============================================================================
// POMODORO GESTURE DETECTION (Flip & Lift)
// ============================================================================
// Key distinction:
// - FLIP: Device rotates 180° while staying on surface. Magnitude stays ~16384 (1g).
// - LIFT: Device is picked up. Magnitude changes during hand movement.
// ============================================================================

void readPomodoroAccel() {
  g_accelX = readMPU6050Accel(MPU6050_ACCEL_XOUT_H);
  g_accelY = readMPU6050Accel(MPU6050_ACCEL_YOUT_H);
  g_accelZ = readMPU6050Accel(MPU6050_ACCEL_ZOUT_H);
  g_magnitude = sqrt((float)g_accelX * g_accelX +
                     (float)g_accelY * g_accelY +
                     (float)g_accelZ * g_accelZ);
  g_sensorRead = true;
}


void checkPomodoroLift() {
  if (!checkMPU6050()) return;
  if (!g_sensorRead) readPomodoroAccel();

  // === LIFT DETECTION CONSTANTS ===
  const float MAGNITUDE_1G = 16384.0;       // Expected magnitude at rest (1g)
  const float LIFT_THRESHOLD = 0.70;         // Magnitude drops below 70% = lifted
  const float ON_DESK_THRESHOLD = 0.92;      // Magnitude above 92% = on desk
  const uint32_t MIN_LIFT_DURATION_MS = 400; // Must be lifted for 400ms+
  const uint32_t DEBOUNCE_MS = 800;          // Minimum time between pause/resume

  // State machine
  static enum { DESK, LIFTING, LIFTED, PUTTING_BACK } liftState = DESK;
  static unsigned long stateChangeTime = 0;
  static unsigned long lastActionTime = 0;
  static float smoothedMagnitude = MAGNITUDE_1G;

  // Smooth the magnitude to reduce noise (exponential moving average)
  smoothedMagnitude = smoothedMagnitude * 0.7 + g_magnitude * 0.3;
  float ratio = smoothedMagnitude / MAGNITUDE_1G;

  // Determine current physical state
  bool isLiftedNow = (ratio < LIFT_THRESHOLD);
  bool isOnDeskNow = (ratio > ON_DESK_THRESHOLD);

  unsigned long now = millis();

  switch (liftState) {
    case DESK:
      if (isLiftedNow) {
        liftState = LIFTING;
        stateChangeTime = now;
        Serial.println("[LIFT] Possible lift detected...");
      }
      break;

    case LIFTING:
      if (isOnDeskNow) {
        // False alarm - device wasn't actually lifted
        liftState = DESK;
        Serial.println("[LIFT] False alarm - back to desk");
      } else if (isLiftedNow && (now - stateChangeTime >= MIN_LIFT_DURATION_MS)) {
        // Confirmed lift!
        liftState = LIFTED;
        Serial.print("[LIFT] LIFTED confirmed (magnitude ratio: ");
        Serial.print(ratio, 2);
        Serial.println(")");
      }
      break;

    case LIFTED:
      if (isOnDeskNow) {
        liftState = PUTTING_BACK;
        stateChangeTime = now;
        Serial.println("[LIFT] Device being put back...");
      }
      break;

    case PUTTING_BACK:
      if (isLiftedNow) {
        // Picked up again before settling
        liftState = LIFTED;
        Serial.println("[LIFT] Picked up again");
      } else if (isOnDeskNow && (now - stateChangeTime >= 200)) {
        // Device is back on desk - trigger pause/resume
        liftState = DESK;

        if (now - lastActionTime >= DEBOUNCE_MS) {
          lastActionTime = now;
          if (pomodoroPaused) {
            resumePomodoro();
            Serial.println("[LIFT] >>> TIMER RESUMED <<<");
          } else {
            pausePomodoro();
            Serial.println("[LIFT] >>> TIMER PAUSED <<<");
          }
        } else {
          Serial.println("[LIFT] Debounce - action skipped");
        }
      }
      break;
  }
}


void checkPomodoroFlip() {
  if (!checkMPU6050()) return;
  if (!g_sensorRead) readPomodoroAccel();

  // === FLIP DETECTION CONSTANTS ===
  const float MAGNITUDE_1G = 16384.0;
  const float STABLE_MIN = 0.85;             // Magnitude must be >85% of 1g
  const float STABLE_MAX = 1.15;             // Magnitude must be <115% of 1g
  const int16_t VERTICAL_THRESHOLD = 10000;  // Strong vertical component required
  const int16_t TILT_MAX = 6000;             // Other axes must be small
  const uint32_t FLIP_DEBOUNCE_MS = 1000;    // 1 second between flips

  // State tracking
  static int8_t lastOrientation = 0;  // -1 = flipped, 0 = unknown, 1 = normal
  static unsigned long lastFlipTime = 0;
  static bool flipLocked = false;     // Lock during lift to prevent false flips

  // Check if magnitude is stable (device on surface, not being lifted)
  float ratio = g_magnitude / MAGNITUDE_1G;
  bool isStable = (ratio >= STABLE_MIN && ratio <= STABLE_MAX);

  // During a lift, don't detect flips
  if (!isStable) {
    flipLocked = true;
    return;
  }

  // Unlock after returning to stable (with small delay)
  if (flipLocked && isStable) {
    static unsigned long stableStartTime = 0;
    if (stableStartTime == 0) stableStartTime = millis();
    if (millis() - stableStartTime < 300) return;  // Wait 300ms stable
    flipLocked = false;
    stableStartTime = 0;
  }

  // Find the dominant (vertical) axis
  int16_t absX = abs(g_accelX);
  int16_t absY = abs(g_accelY);
  int16_t absZ = abs(g_accelZ);

  int16_t verticalValue = 0;
  int16_t otherMax = 0;

  if (absZ >= absX && absZ >= absY) {
    verticalValue = g_accelZ;
    otherMax = max(absX, absY);
  } else if (absY >= absX && absY >= absZ) {
    verticalValue = g_accelY;
    otherMax = max(absX, absZ);
  } else {
    verticalValue = g_accelX;
    otherMax = max(absY, absZ);
  }

  // Must have strong vertical component and minimal tilt
  if (abs(verticalValue) < VERTICAL_THRESHOLD || otherMax > TILT_MAX) {
    return;  // Not a clear orientation
  }

  // Determine current orientation
  int8_t currentOrientation = (verticalValue > 0) ? 1 : -1;

  // Detect flip transition
  if (lastOrientation != 0 && currentOrientation != lastOrientation) {
    unsigned long now = millis();
    if (now - lastFlipTime >= FLIP_DEBOUNCE_MS) {
      lastFlipTime = now;

      Serial.print("[FLIP] >>> 180-DEGREE FLIP DETECTED <<< (");
      Serial.print(lastOrientation == 1 ? "NORMAL" : "FLIPPED");
      Serial.print(" -> ");
      Serial.print(currentOrientation == 1 ? "NORMAL" : "FLIPPED");
      Serial.println(")");

      advancePomodoroPhase();
    }
  }

  // Update orientation and display flip state
  lastOrientation = currentOrientation;
  displayFlipped = (currentOrientation == -1);

  // Reset sensor read flag for next cycle
  g_sensorRead = false;
}


// --- MPU6050 FUNCTIONS ---


void initMPU6050() {
 Serial.print("Initializing MPU6050 on I2C (SDA=");
 Serial.print(SDA_PIN);
 Serial.print(", SCL=");
 Serial.print(SCL_PIN);
 Serial.print(")... ");
  Wire.beginTransmission(MPU6050_ADDR);
 Wire.write(MPU6050_PWR_MGMT_1);
 Wire.write(0); // Wake up MPU6050
 uint8_t error = Wire.endTransmission();
  if (error != 0) {
   Serial.print("ERROR! I2C error code: ");
   Serial.println(error);
   Serial.println("Check MPU6050 wiring!");
   return;
 }
  delay(100);
  // Verify MPU6050 is connected
 Wire.beginTransmission(MPU6050_ADDR);
 Wire.write(MPU6050_WHO_AM_I);
 error = Wire.endTransmission(false);
 Wire.requestFrom(MPU6050_ADDR, 1, true);
  if (Wire.available()) {
   uint8_t whoAmI = Wire.read();
   if (whoAmI == 0x68) {
     Serial.println("SUCCESS!");
     Serial.println("MPU6050 ready for orientation detection");
   } else {
     Serial.print("WARNING! WHO_AM_I = 0x");
     Serial.println(whoAmI, HEX);
     Serial.println("Expected 0x68 - sensor may not be MPU6050");
   }
 } else {
   Serial.println("FAILED!");
   Serial.println("MPU6050 not responding - check wiring:");
   Serial.println("  SDA -> Pin 4");
   Serial.println("  SCL -> Pin 5");
   Serial.println("  VCC -> 3.3V");
   Serial.println("  GND -> GND");
   Serial.println("Orientation flip will be disabled");
 }
}


int16_t readMPU6050Accel(int reg) {
 Wire.beginTransmission(MPU6050_ADDR);
 Wire.write(reg);
 Wire.endTransmission(false);
 Wire.requestFrom(MPU6050_ADDR, 2, true);
  if (Wire.available() >= 2) {
   int16_t value = (Wire.read() << 8) | Wire.read();
   return value;
 }
 return 0;
}


bool checkMPU6050() {
 Wire.beginTransmission(MPU6050_ADDR);
 Wire.write(MPU6050_WHO_AM_I);
 uint8_t error = Wire.endTransmission();
 return (error == 0);
}


void checkOrientation() {
 if (!checkMPU6050()) {
   // Serial.println("MPU6050 not available");
   return; // MPU6050 not available
 }
  // Read accelerometer values
 int16_t accelX = readMPU6050Accel(MPU6050_ACCEL_XOUT_H);
 int16_t accelY = readMPU6050Accel(MPU6050_ACCEL_YOUT_H);
 int16_t accelZ = readMPU6050Accel(MPU6050_ACCEL_ZOUT_H);
  // Debug output (every 2 seconds to see what's happening)
 static int debugCounter = 0;
 static unsigned long lastDebugTime = 0;
 if (millis() - lastDebugTime > 2000) {
   // Find which axis is vertical
   int16_t absX = abs(accelX);
   int16_t absY = abs(accelY);
   int16_t absZ = abs(accelZ);
   int16_t maxAxis = max(absX, max(absY, absZ));
   
   char verticalAxisName = '?';
   if (maxAxis == absX) verticalAxisName = 'X';
   else if (maxAxis == absY) verticalAxisName = 'Y';
   else verticalAxisName = 'Z';
   
   Serial.print("[MPU6050] X: ");
   Serial.print(accelX);
   Serial.print(" Y: ");
   Serial.print(accelY);
   Serial.print(" Z: ");
   Serial.print(accelZ);
   Serial.print(" | Vertical: ");
   Serial.print(verticalAxisName);
   Serial.print(" (");
   Serial.print(maxAxis);
   Serial.print(") | State: ");
   Serial.println(displayFlipped ? "FLIPPED" : "NORMAL");
   lastDebugTime = millis();
 }
  // Find which axis is vertical (has largest magnitude)
 // This works regardless of how MPU6050 is mounted
 int16_t absX = abs(accelX);
 int16_t absY = abs(accelY);
 int16_t absZ = abs(accelZ);
 
 int16_t maxAxis = max(absX, max(absY, absZ));
 
 // Determine which axis is vertical and get its value
 int16_t verticalAxis = 0;
 if (maxAxis == absX) {
   verticalAxis = accelX;
 } else if (maxAxis == absY) {
   verticalAxis = accelY;
 } else {
   verticalAxis = accelZ;
 }
 
 // For 180° flip: vertical axis sign changes
 const int16_t minVerticalMagnitude = 8000; // Minimum for stable reading
 
 if (maxAxis < minVerticalMagnitude) {
   return; // Not stable enough
 }
 
 // Check if other axes are small (device not tilted)
 int16_t otherAxesMax = 0;
 if (maxAxis == absX) {
   otherAxesMax = max(absY, absZ);
 } else if (maxAxis == absY) {
   otherAxesMax = max(absX, absZ);
 } else {
   otherAxesMax = max(absX, absY);
 }
 
 // Require other axes to be relatively small (prevents side tilts)
 if (otherAxesMax > 8000) {
   return; // Device is tilted sideways
 }
 
 // Determine orientation based on vertical axis sign
 bool shouldBeFlipped = (verticalAxis < -minVerticalMagnitude);
 bool shouldBeNormal = (verticalAxis > minVerticalMagnitude);
 
 // Only update if orientation actually changed AND it's a genuine flip
 if ((shouldBeFlipped && !displayFlipped) || (shouldBeNormal && displayFlipped)) {
   Serial.println("========================================");
   Serial.print("[ORIENTATION CHANGE] Vertical axis: ");
   Serial.print(verticalAxis);
   Serial.print(" (X: ");
   Serial.print(accelX);
   Serial.print(" Y: ");
   Serial.print(accelY);
   Serial.print(" Z: ");
   Serial.print(accelZ);
   Serial.print(") | Changing from ");
   Serial.print(displayFlipped ? "FLIPPED" : "NORMAL");
   Serial.print(" to ");
   Serial.println(shouldBeFlipped ? "FLIPPED (180°)" : "NORMAL");
   Serial.println("========================================");
  
   // Update display orientation
   displayFlipped = shouldBeFlipped;
  
   // Apply rotation to display
   // Rotation: 0=normal, 1=90°, 2=180°, 3=270°
   uint8_t rotation = displayFlipped ? 2 : 0;
  
   // Implement hardware flip using MAX7219 registers via SPI
   // MAX7219 Register 0x0C: Scan Limit (bits 0-3) - controls how many rows are displayed
   // We'll use direct SPI to flip the display buffer
  
   // Method: Use SPI to write directly to MAX7219 display test register and scan limit
   // Then manipulate the display buffer by reversing column order
  
   Serial.print("Display orientation changed: ");
   Serial.println(displayFlipped ? "FLIPPED (180°)" : "NORMAL");
  
   // Apply hardware flip by reversing the display buffer
   // We'll do this by manipulating the SPI data directly
   flipDisplayHardware(displayFlipped);
  
   // Clear and refresh display to apply flip
   P.displayClear();
  
   Serial.print("Display ");
   Serial.print(displayFlipped ? "flipped 180°" : "returned to normal");
   Serial.print(" (Z-axis: ");
   Serial.print(accelZ);
   Serial.println(")");
  
   // Refresh display after rotation change
   delay(100); // Brief delay for rotation to take effect
  
   // Apply flip effect based on current state
   if (displayFlipped) {
     P.setZoneEffect(0, true, PA_FLIP_LR);  // Enable horizontal flip
     P.setZoneEffect(0, true, PA_FLIP_UD);   // Enable vertical flip
   } else {
     P.setZoneEffect(0, false, PA_FLIP_LR);  // Disable horizontal flip
     P.setZoneEffect(0, false, PA_FLIP_UD);   // Disable vertical flip
   }
  
    if (currentMode == MODE_CLOCK) {
      // Flip in clock mode triggers weather display for 15 seconds
      if (wifiConnected && weatherDataReady && strlen(weatherData) > 0) {
        Serial.println("[FLIP-WEATHER] Flip detected in clock mode - showing weather for 15 sec");
        startFlipWeather();
      } else {
        updateClockDisplay();
      }
    } else if (currentMode == MODE_WEATHER) {
      // Restart weather scroll
      startWeatherScroll();
    } else if (currentMode == MODE_BREAK) {
      // Restart break scroll with correct direction
      if (strlen(breakDisplayText) > 0) {
        P.displayClear();
        textEffect_t scrollDir = getScrollEffect();
        P.displayZoneText(0, breakDisplayText, PA_LEFT, 80, 0, scrollDir, scrollDir);
        P.displayAnimate();
      }
    }
  }
  lastAccelZ = accelZ;
}


void flipDisplayHardware(bool flipped) {
 // Store flip state - actual flip is applied after text rendering
 Serial.print("[FLIP] Hardware flip state: ");
 Serial.println(flipped ? "ENABLED" : "DISABLED");
}


// Helper function to get correct scroll direction based on flip state
// When flipped, we need PA_SCROLL_RIGHT to maintain visual right-to-left scrolling
textEffect_t getScrollEffect() {
  return displayFlipped ? PA_SCROLL_RIGHT : PA_SCROLL_LEFT;
}


// Helper function to reverse bits in a byte (for vertical row flip)
uint8_t reverseBits(uint8_t b) {
 b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
 b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
 b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
 return b;
}


// Display buffer storage for flip operation
uint8_t displayBuffer[MAX_DEVICES][8]; // Stores current display state
bool bufferStored = false;


void applyDisplayFlip() {
 // Apply 180° flip by manipulating MAX7219 registers via SPI
 // Since we can't read back the buffer easily, we'll use a workaround:
 // Re-render the text with coordinates flipped, OR use hardware registers
  if (mxHardware == NULL) {
   return;
 }
  // Method: Use SPI to directly access MAX7219 and flip the buffer
 // MAX7219 columns are registers 0x01-0x08 (DIG0-DIG7)
 // For 180° flip: reverse column order AND reverse bits in each column
  // Since MAX7219 doesn't support read-back, we need to track the buffer
 // The best approach is to intercept MD_Parola's rendering and flip during render
  // For now, use a simpler solution: Clear display and re-render flipped
 // This requires custom rendering which we'll implement
  // Actually, the simplest working solution is to use MD_MAX72XX's control()
 // to reverse scan direction, then manually flip rows
  // Use control() to set reverse scan (flips horizontally)
 // Register 0x0C controls scan limit, but we need scan direction
 // MAX7219 doesn't have direct scan direction control, so we'll flip manually
  // Start SPI transaction to write flipped data
 SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));
  // For each device, we need to read current buffer and flip it
 // Since we can't read back, we'll use a workaround:
 // Store buffer during normal rendering, then flip it here
  for (uint8_t dev = 0; dev < MAX_DEVICES; dev++) {
   // If we have stored buffer, flip it
   if (bufferStored) {
     digitalWrite(CS_PIN, LOW);
    
     // Write columns in reverse order with bit-reversed data
     for (uint8_t col = 0; col < 8; col++) {
       uint8_t flippedCol = 7 - col; // Reverse column order
       uint8_t colData = displayBuffer[dev][flippedCol];
       colData = reverseBits(colData); // Reverse bits for vertical flip
      
       // Write to MAX7219: address (0x01-0x08) + data
       uint8_t address = 0x01 + col; // Write to original column position
       SPI.transfer(address);
       SPI.transfer(colData);
     }
    
     digitalWrite(CS_PIN, HIGH);
   }
 }
  SPI.endTransaction();
  // Note: This requires bufferStored to be true, which means we need to
 // intercept MD_Parola's rendering to store the buffer first
}


void flipDisplayBuffer180() {
 // Flip display buffer 180° using MAX7219 hardware registers
 // Method: Use SPI to directly write flipped column data
 // Since MAX7219 doesn't support read-back, we'll use MD_MAX72XX's
 // internal buffer access or re-render with flipped coordinates
  if (mxHardware == NULL) {
   return;
 }
  // The challenge: MAX7219 doesn't support read-back
 // Solution: Use MD_MAX72XX's control() to reverse scan direction
 // Register 0x0C controls scan limit, but we need scan direction
  // For MAX7219, scan direction reversal is done by:
 // 1. Writing columns in reverse order (8->1 instead of 1->8)
 // 2. Bit-reversing each column's data (for vertical flip)
  // Since we can't read back, we'll use a workaround:
 // Track the display buffer during normal rendering, then flip it
  // For now, use control() to reverse scan direction (partial solution)
 // This flips horizontally only - full 180° requires buffer manipulation
  // Start SPI transaction
 SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));
  for (uint8_t dev = 0; dev < MAX_DEVICES; dev++) {
   digitalWrite(CS_PIN, LOW);
  
   // If we have stored buffer, flip it
   if (bufferStored) {
     // Write columns in reverse order with bit-reversed data
     for (uint8_t col = 0; col < 8; col++) {
       uint8_t flippedColIdx = 7 - col; // Reverse column index
       uint8_t colData = displayBuffer[dev][flippedColIdx];
       colData = reverseBits(colData); // Reverse bits for vertical flip
      
       // Write to MAX7219 register (0x01-0x08 for columns)
       uint8_t regAddr = 0x01 + col;
       SPI.transfer(regAddr);
       SPI.transfer(colData);
     }
   } else {
     // Buffer not stored - use control() to reverse scan direction
     // This is a partial solution (horizontal flip only)
     // Full 180° requires buffer tracking
   }
  
   digitalWrite(CS_PIN, HIGH);
 }
  SPI.endTransaction();
  // Note: Full 180° flip requires tracking display buffer during rendering
 // This is implemented by storing buffer when text is rendered
}



