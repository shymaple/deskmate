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


// --- REMINDER STRUCTURE ---
struct Reminder {
 bool enabled;
 int hour;
 int minute;
 char message[51]; // 50 chars + null terminator
};


// --- POMODORO PHASE STRUCTURE ---
struct PomodoroPhase {
 int durationMinutes; // Duration in minutes
 char name[16]; // Phase name (e.g., "FOCUS", "BREAK")
};


// --- CONFIGURATION STRUCTURE ---
// CONFIG_VERSION: Increment this only when config structure changes in incompatible way
// This ensures EEPROM data persists across firmware updates
#define CONFIG_VERSION 1

struct Config {
 uint8_t version; // Config version for migration/compatibility checking
 char ssid[32];
 char password[64];
 char ntpServer[64];
 long gmtOffset_sec;
 int daylightOffset_sec;
 char weatherApiKey[64];
 char lat[16];
 char lon[16];
 char cityName[32];
 int brightness;
 bool clockFormat24h; // true = 24h, false = 12h
 Reminder reminders[3]; // Max 3 reminders
 PomodoroPhase pomodoroPhases[4]; // 4 Pomodoro phases
 bool valid;
};


Config config;


// --- EEPROM SETTINGS ---
#define EEPROM_SIZE 1024  // Increased for reminders
#define EEPROM_ADDR 0


// --- WEB SERVER ---
WebServer server(80);


// --- ACCESS POINT SETTINGS ---
const char* ap_ssid = "ESP32-Clock";
const char* ap_password = "clock123";


// --- GLOBAL STATE ---
struct tm timeinfo;
char timeYesterday[4], timeLastHour[3];
// RTC time management
unsigned long lastNTPSync = 0;
const unsigned long NTP_SYNC_INTERVAL = 86400000; // 24 hours in milliseconds
bool ntpSyncScheduled = false; // Flag for scheduled sync at 1:11:11 AM
int lastSyncedDay = -1; // Track last sync day


// Display Mode State
enum DisplayMode {
 MODE_CLOCK = 0,
 MODE_WEATHER = 1,
 MODE_REMINDER = 2,
 MODE_POMODORO = 3
};
DisplayMode currentMode = MODE_CLOCK;


// Blinking Colon for Clock
unsigned long previousMillisColon = 0;
const long colonBlinkInterval = 1000;
bool colonVisible = true;


// Weather data buffer
char weatherData[100] = "Loading...";
bool weatherDataReady = false;


// Reminder display buffer
char reminderDisplayText[60] = "";


// Reminder tracking
int lastCheckedMinute = -1;
int lastCheckedHour = -1;
int reminderScrollCount = 0; // Track how many times reminder has scrolled
const int REMINDER_SCROLL_LOOPS = 3; // Show reminder 3 times


// Pomodoro timer state
int currentPomodoroPhase = 0; // Current phase index (0-3)
unsigned long pomodoroStartTime = 0; // When current phase started (millis)
unsigned long pomodoroRemainingSeconds = 0; // Remaining seconds in current phase
bool pomodoroPaused = false; // Pause state
bool pomodoroTimerExpired = false; // Timer reached 00:00
unsigned long pomodoroExpiredTime = 0; // When timer expired (for auto-return)
const unsigned long POMODORO_AUTO_RETURN_MS = 300000; // 5 minutes = 300000ms
unsigned long lastPomodoroUpdate = 0; // Last display update time
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
         <input type="password" id="password" name="password">
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
       <div class="section-title">Reminders (Max 5)</div>
       <div id="remindersContainer">
         <!-- Reminders will be added here by JavaScript -->
       </div>
     </div>
    
     <div class="section">
       <div class="section-title">Pomodoro Timer</div>
       <div class="form-group">
         <label>Phase 1: Duration (minutes)</label>
         <input type="number" id="pomodoroPhase0Duration" name="pomodoroPhase0Duration" min="1" max="120" value="60">
       </div>
       <div class="form-group">
         <label>Phase 2: Duration (minutes)</label>
         <input type="number" id="pomodoroPhase1Duration" name="pomodoroPhase1Duration" min="1" max="120" value="5">
       </div>
       <div class="form-group">
         <label>Phase 3: Duration (minutes)</label>
         <input type="number" id="pomodoroPhase2Duration" name="pomodoroPhase2Duration" min="1" max="120" value="30">
       </div>
       <div class="form-group">
         <label>Phase 4: Duration (minutes)</label>
         <input type="number" id="pomodoroPhase3Duration" name="pomodoroPhase3Duration" min="1" max="120" value="10">
       </div>
       <div class="help" style="margin-top: 10px;">
         <strong>Usage:</strong> Press button to cycle: Clock to Weather to Pomodoro to Clock<br>
         <strong>Flip device 180 degrees</strong> to advance to next phase<br>
         <strong>Lift and put back</strong> to pause/resume timer
       </div>
     </div>
    
     <button type="submit" class="btn"> Save & Apply</button>
   </form>
 </div>
  <script>
   function showStatus(msg, type) {
     const status = document.getElementById('status');
     status.className = 'status ' + type;
     status.textContent = msg;
     setTimeout(() => status.className = 'status', 5000);
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
        
         // Load reminders
         if (data.reminders) {
           loadReminders(data.reminders);
         } else {
           createReminderForms();
         }
         
         // Load Pomodoro phases
         if (data.pomodoroPhases && Array.isArray(data.pomodoroPhases)) {
           for (let i = 0; i < 4; i++) {
             if (data.pomodoroPhases[i]) {
               document.getElementById('pomodoroPhase' + i + 'Duration').value = data.pomodoroPhases[i].durationMinutes || (i === 0 ? 60 : i === 1 ? 5 : i === 2 ? 30 : 10);
             }
           }
         }
       })
       .catch(err => {
         console.log('Error loading config:', err);
         createReminderForms();
       });
   };
  
   function createReminderForms() {
     const container = document.getElementById('remindersContainer');
     container.innerHTML = "";
     for (let i = 0; i < 3; i++) {
       const reminderDiv = document.createElement('div');
       reminderDiv.className = 'form-group';
       reminderDiv.style.border = '1px solid #e0e0e0';
       reminderDiv.style.padding = '15px';
       reminderDiv.style.borderRadius = '6px';
       reminderDiv.style.marginBottom = '10px';
       reminderDiv.innerHTML = 
         "<div style=\"display: flex; align-items: center; margin-bottom: 10px;\">" +
           "<input type=\"checkbox\" id=\"reminder" + i + "Enabled\" name=\"reminder" + i + "Enabled\" style=\"width: auto; margin-right: 8px;\">" +
           "<label style=\"margin: 0; font-weight: 600;\">Reminder " + (i + 1) + "</label>" +
         "</div>" +
         "<div style=\"display: grid; grid-template-columns: 1fr 1fr 2fr; gap: 10px;\">" +
           "<div>" +
             "<label style=\"font-size: 11px;\">Hour (0-23)</label>" +
             "<input type=\"number\" id=\"reminder" + i + "Hour\" name=\"reminder" + i + "Hour\" min=\"0\" max=\"23\" value=\"0\" style=\"width: 100%;\">" +
           "</div>" +
           "<div>" +
             "<label style=\"font-size: 11px;\">Minute (0-59)</label>" +
             "<input type=\"number\" id=\"reminder" + i + "Minute\" name=\"reminder" + i + "Minute\" min=\"0\" max=\"59\" value=\"0\" style=\"width: 100%;\">" +
           "</div>" +
           "<div>" +
             "<label style=\"font-size: 11px;\">Message (max 50 chars)</label>" +
             "<input type=\"text\" id=\"reminder" + i + "Message\" name=\"reminder" + i + "Message\" maxlength=\"50\" placeholder=\"Reminder message\" style=\"width: 100%;\">" +
           "</div>" +
         "</div>";
       container.appendChild(reminderDiv);
     }
   }
  
   function loadReminders(reminders) {
     createReminderForms();
     if (reminders && Array.isArray(reminders)) {
       reminders.forEach((reminder, i) => {
         if (reminder) {
           document.getElementById('reminder' + i + 'Enabled').checked = reminder.enabled || false;
           document.getElementById('reminder' + i + 'Hour').value = reminder.hour || 0;
           document.getElementById('reminder' + i + 'Minute').value = reminder.minute || 0;
           document.getElementById('reminder' + i + 'Message').value = reminder.message || "";
         }
       });
     }
   }
  
   document.getElementById('configForm').onsubmit = function(e) {
     e.preventDefault();
     const formData = new FormData(this);
    
     fetch('/save', {
       method: 'POST',
       body: formData
     })
     .then(r => r.text())
     .then(data => {
       if (data.includes('success')) {
         showStatus('Saved! Device will reconnect...', 'success');
         setTimeout(() => updateStatus(), 2000);
       } else {
         showStatus('Error saving', 'error');
       }
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
void showReminder(const char* message);
void initMPU6050();
bool checkMPU6050();
void checkOrientation();
int16_t readMPU6050Accel(int reg);
void flipDisplayHardware(bool flipped);
void handleRoot();
void handleConfig();
void handleSave();
void handleStatus();
// Pomodoro functions
void startPomodoroMode();
void updatePomodoroDisplay();
void advancePomodoroPhase();
void pausePomodoro();
void resumePomodoro();
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
 server.begin();
 Serial.println("Web server started");
  // Show AP info immediately
 Serial.println("\n=== Access Point Started ===");
 Serial.print("AP SSID: ");
 Serial.println(ap_ssid);
 Serial.print("AP Password: ");
 Serial.println(ap_password);
 Serial.print("AP IP: ");
 Serial.println(WiFi.softAPIP());
 Serial.println("Open: http://192.168.4.1");
 Serial.println("===========================\n");
  P.displayZoneText(0, "CONFIG", PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
 P.displayAnimate();
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
   updateClockDisplay();
 } else {
   Serial.println("\nWiFi connection failed - AP mode active");
   Serial.println("Connect to WiFi network: ESP32-Clock");
   Serial.println("Password: clock123");
   Serial.println("Then open: http://192.168.4.1");
  
   // Show "CONFIG" on display
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
  // --- WEATHER MODE ---
 else if (currentMode == MODE_WEATHER) {
   if (P.getZoneStatus(0)) {
     Serial.println("Weather scroll complete, returning to clock");
     currentMode = MODE_CLOCK;
     P.displayClear();
     colonVisible = true;
     updateClockDisplay();
   }
 }
  // --- REMINDER MODE ---
 else if (currentMode == MODE_REMINDER) {
   if (P.getZoneStatus(0)) {
     reminderScrollCount++;
     Serial.print("Reminder scroll complete (loop ");
     Serial.print(reminderScrollCount);
     Serial.print(" of ");
     Serial.print(REMINDER_SCROLL_LOOPS);
     Serial.println(")");
    
     if (reminderScrollCount >= REMINDER_SCROLL_LOOPS) {
       // All loops complete, return to clock
       Serial.println("Reminder display finished, returning to clock");
       reminderScrollCount = 0;
       currentMode = MODE_CLOCK;
       P.displayClear();
       colonVisible = true;
       updateClockDisplay();
     } else {
       // Restart scroll for next loop
       delay(500); // Brief pause between loops
       // Restart the scroll with the stored reminder text
       if (strlen(reminderDisplayText) > 0) {
         P.displayClear();
         P.displayZoneText(0, reminderDisplayText, PA_LEFT, 100, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
         P.displayAnimate();
         Serial.print("Restarting reminder scroll (loop ");
         Serial.print(reminderScrollCount + 1);
         Serial.println(")");
       }
     }
   }
 }
  // --- POMODORO MODE ---
 else if (currentMode == MODE_POMODORO) {
   // Check for lift (pause/resume) - only every 2 seconds to save battery
   if (currentMillis - lastOrientationCheck >= orientationCheckInterval) {
     lastOrientationCheck = currentMillis;
     checkPomodoroLift();
     checkPomodoroFlip();
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
  // Check for reminders (only in clock mode)
 if (currentMode == MODE_CLOCK && getLocalTime(&timeinfo)) {
   int currentHour = timeinfo.tm_hour;
   int currentMinute = timeinfo.tm_min;
  
   // Check once per minute
   if (currentMinute != lastCheckedMinute || currentHour != lastCheckedHour) {
     lastCheckedMinute = currentMinute;
     lastCheckedHour = currentHour;
    
     // Check all reminders
     for (int i = 0; i < 3; i++) {
       if (config.reminders[i].enabled &&
           config.reminders[i].hour == currentHour &&
           config.reminders[i].minute == currentMinute) {
         // Trigger reminder
         Serial.print("Reminder triggered: ");
         Serial.println(config.reminders[i].message);
         showReminder(config.reminders[i].message);
         break; // Only show one reminder at a time
       }
     }
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
   // Initialize reminders
   for (int i = 0; i < 3; i++) {
     config.reminders[i].enabled = false;
     config.reminders[i].hour = 0;
     config.reminders[i].minute = 0;
     strcpy(config.reminders[i].message, "");
   }
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
 // Disconnect any existing connections first
 WiFi.disconnect(true);
 delay(200);
  
 // Set to AP_STA mode to allow both AP and Station simultaneously
 WiFi.mode(WIFI_AP_STA);
 delay(200);
  
 // Configure AP with explicit channel and max connections
 // Use channel 1, not hidden, max 4 connections
 bool apStarted = WiFi.softAP(ap_ssid, ap_password, 1, 0, 4);
  
 if (!apStarted) {
   Serial.println("ERROR: AP setup failed! Retrying...");
   delay(500);
   // Try again with AP mode only
   WiFi.mode(WIFI_AP);
   delay(200);
   apStarted = WiFi.softAP(ap_ssid, ap_password);
 }
  
 if (apStarted) {
   // Give AP more time to fully initialize
   delay(1000);
   
   // Verify AP is actually running
   int retries = 0;
   while (WiFi.softAPIP().toString() == "0.0.0.0" && retries < 10) {
     delay(200);
     retries++;
   }
   
   IPAddress IP = WiFi.softAPIP();
   if (IP.toString() != "0.0.0.0") {
     Serial.print("✓ AP started successfully!");
     Serial.print(" SSID: ");
     Serial.print(ap_ssid);
     Serial.print(" IP: ");
     Serial.println(IP);
     Serial.print("AP MAC: ");
     Serial.println(WiFi.softAPmacAddress());
   } else {
     Serial.println("ERROR: AP started but IP not assigned!");
   }
 } else {
   Serial.println("ERROR: AP failed to start after retry!");
 }
}


void connectWiFi() {
 Serial.print("Connecting to WiFi: ");
 Serial.println(config.ssid);
  // Ensure we're in AP_STA mode so AP stays active
 if (WiFi.getMode() != WIFI_AP_STA) {
   WiFi.mode(WIFI_AP_STA);
   delay(100);
   // Restart AP if needed
   if (!WiFi.softAP(ap_ssid, ap_password)) {
     Serial.println("Warning: Could not restart AP");
   }
 }
  // Begin WiFi connection (non-blocking, AP still works)
 WiFi.begin(config.ssid, config.password);
  unsigned long startAttempt = millis();
 int attempts = 0;
 while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
   delay(500);
   showWiFiConnecting();
   Serial.print(".");
   attempts++;
  
   // Keep web server responsive
   server.handleClient();
 }
  if (WiFi.status() == WL_CONNECTED) {
   wifiConnected = true;
   Serial.println("\n✓ WiFi connected!");
   Serial.print("Station IP: ");
   Serial.println(WiFi.localIP());
   Serial.print("AP still active at: ");
   Serial.println(WiFi.softAPIP());
 } else {
   wifiConnected = false;
   Serial.println("\n✗ WiFi connection failed");
   Serial.println("AP mode remains active for configuration");
   Serial.print("AP IP: ");
   Serial.println(WiFi.softAPIP());
 }
}


// --- WEB SERVER HANDLERS ---


void handleRoot() {
 server.send(200, "text/html", htmlPage);
}


void handleConfig() {
 DynamicJsonDocument doc(3072); // Increased for reminders and Pomodoro
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
  // Add reminders array
 JsonArray reminders = doc.createNestedArray("reminders");
 for (int i = 0; i < 3; i++) {
   JsonObject reminder = reminders.createNestedObject();
   reminder["enabled"] = config.reminders[i].enabled;
   reminder["hour"] = config.reminders[i].hour;
   reminder["minute"] = config.reminders[i].minute;
   reminder["message"] = config.reminders[i].message;
 }
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
   if (strcmp(config.ssid, newSSID.c_str()) != 0) {
     wifiChanged = true;
   }
   strncpy(config.ssid, newSSID.c_str(), sizeof(config.ssid) - 1);
   config.ssid[sizeof(config.ssid) - 1] = '\0';
 }
  if (server.hasArg("password")) {
   String newPassword = server.arg("password");
   if (strcmp(config.password, newPassword.c_str()) != 0) {
     wifiChanged = true;
   }
   strncpy(config.password, newPassword.c_str(), sizeof(config.password) - 1);
   config.password[sizeof(config.password) - 1] = '\0';
 }
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
  // Handle reminders (0-2)
 for (int i = 0; i < 3; i++) {
   String enabledArg = "reminder" + String(i) + "Enabled";
   String hourArg = "reminder" + String(i) + "Hour";
   String minuteArg = "reminder" + String(i) + "Minute";
   String messageArg = "reminder" + String(i) + "Message";
  
   config.reminders[i].enabled = server.hasArg(enabledArg);
  
   if (server.hasArg(hourArg)) {
     config.reminders[i].hour = server.arg(hourArg).toInt();
     if (config.reminders[i].hour < 0) config.reminders[i].hour = 0;
     if (config.reminders[i].hour > 23) config.reminders[i].hour = 23;
   }
  
   if (server.hasArg(minuteArg)) {
     config.reminders[i].minute = server.arg(minuteArg).toInt();
     if (config.reminders[i].minute < 0) config.reminders[i].minute = 0;
     if (config.reminders[i].minute > 59) config.reminders[i].minute = 59;
   }
  
   if (server.hasArg(messageArg)) {
     String msg = server.arg(messageArg);
     // Convert to uppercase before saving
     msg.toUpperCase();
     // Trim whitespace
     msg.trim();
     strncpy(config.reminders[i].message, msg.c_str(), sizeof(config.reminders[i].message) - 1);
     config.reminders[i].message[sizeof(config.reminders[i].message) - 1] = '\0';
    
     Serial.print("Saved reminder ");
     Serial.print(i);
     Serial.print(" message: [");
     Serial.print(config.reminders[i].message);
     Serial.print("] Length: ");
     Serial.println(strlen(config.reminders[i].message));
   }
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


// --- CLOCK FUNCTIONS ---


void handleButton() {
 bool currentState = (digitalRead(MODE_BUTTON_PIN) == LOW);
  if (currentState && !buttonPressed) {
   if (millis() - lastButtonPress > debounceDelay) {
     lastButtonPress = millis();
     buttonPressed = true;
    
     if (currentMode == MODE_CLOCK) {
       Serial.println("Button pressed: Switching to weather");
       startWeatherScroll();
     } else if (currentMode == MODE_WEATHER) {
       Serial.println("Button pressed: Switching to Pomodoro");
       startPomodoroMode();
     } else if (currentMode == MODE_POMODORO) {
       Serial.println("Button pressed: Exiting Pomodoro, returning to clock");
       currentMode = MODE_CLOCK;
       P.displayClear();
       colonVisible = true;
       updateClockDisplay();
     } else if (currentMode == MODE_REMINDER) {
       // Button press during reminder - return to clock
       Serial.println("Button pressed: Exiting reminder, returning to clock");
       reminderScrollCount = REMINDER_SCROLL_LOOPS; // Force exit
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
 P.displayZoneText(0, displayText, PA_LEFT, 100, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
 P.displayAnimate();
}


void showReminder(const char* message) {
 currentMode = MODE_REMINDER;
 reminderScrollCount = 0; // Reset counter
 P.displayClear();
  // Ensure message is valid and not empty
 if (message == NULL || strlen(message) == 0) {
   Serial.println("Error: Empty reminder message");
   return;
 }
  Serial.print("Raw reminder message received: [");
 Serial.print(message);
 Serial.print("] Length: ");
 Serial.println(strlen(message));
  // Format reminder message with smooth scroll
 memset(reminderDisplayText, 0, sizeof(reminderDisplayText)); // Clear buffer
  // Copy message and convert to uppercase
 int srcIdx = 0;
 int dstIdx = 0;
 int maxLen = sizeof(reminderDisplayText) - 10; // Leave room for trailing spaces
  while (message[srcIdx] != '\0' && dstIdx < maxLen) {
   char c = message[srcIdx];
   // Convert to uppercase
   if (c >= 'a' && c <= 'z') {
     c = c - 32;
   }
   // Only copy printable ASCII characters (32-126) and common punctuation
   if ((c >= 32 && c <= 126) || c == '\0') {
     reminderDisplayText[dstIdx] = c;
     dstIdx++;
   }
   srcIdx++;
 }
 reminderDisplayText[dstIdx] = '\0';
  // Ensure string is properly terminated
 if (dstIdx == 0) {
   Serial.println("Error: No valid characters in reminder message");
   return;
 }
  // Add trailing spaces for scroll effect (enough to scroll completely)
 int currentLen = strlen(reminderDisplayText);
 int spacesToAdd = 32; // Add spaces to ensure full scroll
 int availableSpace = sizeof(reminderDisplayText) - currentLen - 1;
 int spacesAdded = (spacesToAdd < availableSpace) ? spacesToAdd : availableSpace;
  for (int j = 0; j < spacesAdded; j++) {
   reminderDisplayText[currentLen + j] = ' ';
 }
 reminderDisplayText[currentLen + spacesAdded] = '\0';
  Serial.print("Formatted reminder: [");
 Serial.print(reminderDisplayText);
 Serial.print("] Length: ");
 Serial.println(strlen(reminderDisplayText));
  // Ensure display is ready
 delay(50);
  // Apply flip effect if needed
 if (displayFlipped) {
   P.setZoneEffect(0, true, PA_FLIP_LR);  // Enable horizontal flip
   P.setZoneEffect(0, true, PA_FLIP_UD);   // Enable vertical flip
 } else {
   P.setZoneEffect(0, false, PA_FLIP_LR);  // Disable horizontal flip
   P.setZoneEffect(0, false, PA_FLIP_UD);   // Disable vertical flip
 }
  // Display with scroll animation (slower speed for readability)
 P.displayZoneText(0, reminderDisplayText, PA_LEFT, 100, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
 P.displayAnimate();
  Serial.println("Reminder display started");
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
 pomodoroBlinkState = false;
 
 Serial.print("Phase: ");
 Serial.print(config.pomodoroPhases[currentPomodoroPhase].name);
 Serial.print(" (");
 Serial.print(durationMinutes);
 Serial.println(" minutes)");
 
 // Update display immediately
 updatePomodoroDisplay();
}


void updatePomodoroDisplay() {
 static char timerText[10];
 
 // If timer expired, blink display
 if (pomodoroTimerExpired) {
   unsigned long currentMillis = millis();
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
   unsigned long currentMillis = millis();
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
 
 // Optional: Add phase indicator (e.g., "1" for phase 1)
 // For now, just show timer
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


void checkPomodoroLift() {
 if (!checkMPU6050()) {
   return; // MPU6050 not available
 }
 
 // Read all axes to detect lift
 int16_t accelX = readMPU6050Accel(MPU6050_ACCEL_XOUT_H);
 int16_t accelY = readMPU6050Accel(MPU6050_ACCEL_YOUT_H);
 int16_t accelZ = readMPU6050Accel(MPU6050_ACCEL_ZOUT_H);
 
 // Find which axis is vertical (has largest magnitude)
 int16_t absX = abs(accelX);
 int16_t absY = abs(accelY);
 int16_t absZ = abs(accelZ);
 
 int16_t maxAxis = max(absX, max(absY, absZ));
 
 // Calculate total magnitude
 float magnitude = sqrt(accelX * accelX + accelY * accelY + accelZ * accelZ);
 
 // Use relative change detection instead of absolute thresholds
 // Track baseline values when device is stable on desk
 static float baselineMagnitude = 0.0;
 static int16_t baselineMaxAxis = 0;
 static unsigned long baselineUpdateTime = 0;
 static bool baselineEstablished = false;
 static bool baselineFrozen = false; // Freeze baseline when device is lifted
 
 // Determine current lift state
 static bool currentlyLifted = false;
 static bool currentlyOnDesk = false;
 
 // First, determine current state using previous baseline (or absolute thresholds)
 if (baselineEstablished && !baselineFrozen) {
   float magnitudeRatio = magnitude / baselineMagnitude;
   float axisRatio = (float)maxAxis / (float)baselineMaxAxis;
   
   // More sensitive thresholds: lifted if below 85% of baseline
   currentlyLifted = (magnitudeRatio < 0.85) || (axisRatio < 0.85);
   currentlyOnDesk = (magnitudeRatio > 0.90) && (axisRatio > 0.90); // Must be 90%+ of baseline
 } else if (baselineEstablished && baselineFrozen) {
   // When baseline is frozen, use it to detect state
   float magnitudeRatio = magnitude / baselineMagnitude;
   float axisRatio = (float)maxAxis / (float)baselineMaxAxis;
   
   currentlyLifted = (magnitudeRatio < 0.85) || (axisRatio < 0.85);
   currentlyOnDesk = (magnitudeRatio > 0.90) && (axisRatio > 0.90);
 } else {
   // Fallback to absolute thresholds if baseline not established
   const int16_t onDeskAxisMin = 12000;
   const int16_t liftedAxisMax = 8000;
   const float onDeskMagnitudeMin = 14000.0;
   const float liftedMagnitudeMax = 10000.0;
   
   currentlyLifted = (maxAxis < liftedAxisMax) || (magnitude < liftedMagnitudeMax);
   currentlyOnDesk = (maxAxis > onDeskAxisMin) && (magnitude > onDeskMagnitudeMin);
 }
 
 // Update baseline only when device is stable on desk (every 2 seconds)
 // AND baseline is not frozen (device not lifted)
 // Use absolute thresholds to determine if device is on desk (not relative to baseline)
 bool isOnDeskByAbsolute = (magnitude > 14000 && magnitude < 18000 && maxAxis > 12000 && maxAxis < 18000);
 
 if (!baselineFrozen && millis() - baselineUpdateTime > 2000) {
   // If magnitude and axis are in expected "on desk" range, update baseline
   if (isOnDeskByAbsolute) {
     if (!baselineEstablished) {
       baselineMagnitude = magnitude;
       baselineMaxAxis = maxAxis;
       baselineEstablished = true;
       Serial.print("[LIFT] Baseline established: Magnitude=");
       Serial.print(baselineMagnitude);
       Serial.print(", MaxAxis=");
       Serial.println(baselineMaxAxis);
     } else {
       // Smooth baseline update (moving average) when device appears to be on desk
       baselineMagnitude = (baselineMagnitude * 0.95) + (magnitude * 0.05);
       baselineMaxAxis = (baselineMaxAxis * 0.95) + (maxAxis * 0.05);
     }
   }
   baselineUpdateTime = millis();
 }
 
 // Freeze/unfreeze baseline based on lift state
 if (currentlyLifted && !baselineFrozen) {
   baselineFrozen = true;
   Serial.println("[LIFT] Baseline frozen (device lifted)");
 } else if (currentlyOnDesk && baselineFrozen) {
   baselineFrozen = false;
   Serial.println("[LIFT] Baseline unfrozen (device on desk)");
 }
 
 // Debug output for lift detection (every 500ms)
 static unsigned long lastLiftDebugTime = 0;
 if (millis() - lastLiftDebugTime > 500) {
   Serial.print("[LIFT DEBUG] Magnitude: ");
   Serial.print(magnitude);
   Serial.print(", MaxAxis: ");
   Serial.print(maxAxis);
   if (baselineEstablished) {
     Serial.print(" | Baseline: M=");
     Serial.print(baselineMagnitude);
     Serial.print(", A=");
     Serial.print(baselineMaxAxis);
     Serial.print(" | Ratio: M=");
     Serial.print(magnitude / baselineMagnitude, 2);
     Serial.print(", A=");
     Serial.print((float)maxAxis / (float)baselineMaxAxis, 2);
   }
   Serial.print(" | Lifted: ");
   Serial.print(currentlyLifted ? "YES" : "NO");
   Serial.print(", OnDesk: ");
   Serial.print(currentlyOnDesk ? "YES" : "NO");
   Serial.print(" | Paused: ");
   Serial.println(pomodoroPaused ? "YES" : "NO");
   lastLiftDebugTime = millis();
 }
 
 // State machine for lift detection (separate from baseline state machine above)
 static bool lastLiftState = false;
 static unsigned long liftStartTime = 0;
 static bool liftActionTaken = false; // Track if we've taken action for this lift cycle
 static unsigned long lastPutBackTime = 0;
 
 // Detect state transitions
 if (currentlyLifted != lastLiftState) {
   // State changed
   if (currentlyLifted) {
     // Just lifted
     liftStartTime = millis();
     liftActionTaken = false;
     Serial.print("[POMODORO] Device lifted detected (magnitude: ");
     Serial.print(magnitude);
     Serial.print(", max axis: ");
     Serial.print(maxAxis);
     if (baselineEstablished) {
       Serial.print(", ratio: ");
       Serial.print(magnitude / baselineMagnitude, 2);
     }
     Serial.println(")");
   } else if (currentlyOnDesk && lastLiftState) {
     // Just put back down - only act if it was actually lifted before
     unsigned long liftDuration = millis() - liftStartTime;
     unsigned long timeSinceLastPutBack = millis() - lastPutBackTime;
     
     Serial.print("[POMODORO] Device put back (lift duration: ");
     Serial.print(liftDuration);
     Serial.print("ms, time since last: ");
     Serial.print(timeSinceLastPutBack);
     Serial.print("ms, action taken: ");
     Serial.print(liftActionTaken ? "YES" : "NO");
     Serial.println(")");
     
     // Require minimum lift duration (reduced to 300ms for better responsiveness)
     // and minimum time since last put-back to prevent double-triggers
     if (liftDuration >= 300 && timeSinceLastPutBack >= 500 && !liftActionTaken) {
       // Device was lifted and put back - toggle pause/resume
       liftActionTaken = true;
       lastPutBackTime = millis();
       
       if (pomodoroPaused) {
         resumePomodoro();
         Serial.println("[POMODORO] Device put back - timer RESUMED");
       } else {
         pausePomodoro();
         Serial.println("[POMODORO] Device put back - timer PAUSED");
       }
     } else {
       Serial.print("[POMODORO] Put-back ignored: ");
       if (liftDuration < 300) Serial.print("lift too short ("); Serial.print(liftDuration); Serial.print("ms) ");
       if (timeSinceLastPutBack < 500) Serial.print("too soon since last ("); Serial.print(timeSinceLastPutBack); Serial.print("ms) ");
       if (liftActionTaken) Serial.print("action already taken ");
       Serial.println();
     }
   }
   lastLiftState = currentlyLifted;
 }
 
 lastAccelY = accelY;
}


void checkPomodoroFlip() {
 if (!checkMPU6050()) {
   return; // MPU6050 not available
 }
 
 // Read all three axes
 int16_t accelX = readMPU6050Accel(MPU6050_ACCEL_XOUT_H);
 int16_t accelY = readMPU6050Accel(MPU6050_ACCEL_YOUT_H);
 int16_t accelZ = readMPU6050Accel(MPU6050_ACCEL_ZOUT_H);
 
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
 // Normal: vertical axis positive and large
 // Flipped: vertical axis negative and large
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
 bool isFlipped180 = (verticalAxis < -minVerticalMagnitude);
 bool isNormal = (verticalAxis > minVerticalMagnitude);
 
 // Track state changes for flip detection
 static int16_t lastVerticalAxis = 0;
 static bool lastWasFlipped = false;
 static unsigned long lastFlipTime = 0;
 static bool phaseAdvancePending = false; // Track if we need to advance phase
 
 // Detect ANY flip transition: normal -> flipped OR flipped -> normal
 // Both should advance the phase
 bool orientationChanged = false;
 
 if (isFlipped180 && !lastWasFlipped && lastVerticalAxis > minVerticalMagnitude) {
   // Just flipped from normal to flipped
   orientationChanged = true;
   lastWasFlipped = true;
 } else if (isNormal && lastWasFlipped && lastVerticalAxis < -minVerticalMagnitude) {
   // Just returned from flipped to normal - this is also a flip!
   orientationChanged = true;
   lastWasFlipped = false;
 } else if (isFlipped180) {
   lastWasFlipped = true;
 } else if (isNormal) {
   lastWasFlipped = false;
 }
 
 // If orientation changed, advance phase (with debounce)
 if (orientationChanged) {
   unsigned long timeSinceLastFlip = millis() - lastFlipTime;
   
   // Prevent rapid double-flips (minimum 800ms between phase changes)
   if (timeSinceLastFlip > 800) {
     Serial.print("[POMODORO] 180° flip detected (axis: ");
     Serial.print(verticalAxis);
     Serial.print(" -> ");
     Serial.print(lastVerticalAxis);
     Serial.print(", state: ");
     Serial.print(isFlipped180 ? "FLIPPED" : "NORMAL");
     Serial.println("), advancing phase");
     
     advancePomodoroPhase();
     lastFlipTime = millis();
     flipDetected = false; // Reset flip detection flag
   }
 }
 
 // Update display orientation based on current state (for display flip)
 displayFlipped = isFlipped180;
 
 // Store current state for next check
 if (isNormal || isFlipped180) {
   lastVerticalAxis = verticalAxis;
 }
 lastAccelZ = accelZ;
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
     updateClockDisplay();
   } else if (currentMode == MODE_WEATHER) {
     // Restart weather scroll
     startWeatherScroll();
   } else if (currentMode == MODE_REMINDER) {
     // Restart reminder scroll
     if (strlen(reminderDisplayText) > 0) {
       P.displayClear();
       P.displayZoneText(0, reminderDisplayText, PA_LEFT, 100, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
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



