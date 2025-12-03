# Deskmate Clock Firmware

A feature-rich ESP32-based clock with weather, reminders, and Pomodoro timer functionality.

## Features

- ⏰ **Clock Display**: 12h/24h format with NTP time synchronization and ESP32 internal RTC
- 🌤️ **Weather Display**: Real-time weather from OpenWeatherMap API
- 🔔 **Reminders**: Up to 3 time-based reminders
- 🍅 **Pomodoro Timer**: 4-phase productivity timer with gesture controls
- 📱 **Web Configuration**: Easy setup via web interface
- 🔄 **Auto-Rotation**: MPU6050-based display orientation detection (180° flip)
- 🔋 **Battery Optimized**: Efficient MPU6050 polling and background NTP sync

## Hardware Requirements

- **ESP32-C3 Super Mini** (or compatible ESP32-C3 board)
- **MAX7219 LED Matrix** (4 modules, 32x8 resolution)
- **MPU6050 Accelerometer** (for auto-rotation and Pomodoro gestures)
- **Button** (for mode switching)
- **Wiring**:
  - MAX7219: SPI (CLK, DATA, CS pins)
  - MPU6050: I2C (SDA, SCL pins)
  - Button: GPIO pin (configurable)

## Building the Firmware

### Prerequisites

1. **Arduino IDE** (version 1.8.19 or later) or **PlatformIO**
2. **ESP32 Board Support**:
   - In Arduino IDE: Go to `File > Preferences`
   - Add to Additional Board Manager URLs: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   - Go to `Tools > Board > Boards Manager`, search for "ESP32" and install

### Installation Steps

#### Option 1: Arduino IDE

1. **Install Required Libraries**:
   - Open Arduino IDE
   - Go to `Sketch > Include Library > Manage Libraries`
   - Install the following libraries:
     - `MD_Parola` by MajicDesigns (version 3.7.0 or later)
     - `MD_MAX72XX` by MajicDesigns (version 3.3.0 or later)
     - `ArduinoJson` by Benoit Blanchon (version 6.x)

2. **Configure Board Settings**:
   - Go to `Tools > Board > ESP32 Arduino > ESP32C3 Dev Module`
   - Set the following:
     - **Upload Speed**: 921600
     - **CPU Frequency**: 160MHz
     - **Flash Frequency**: 80MHz
     - **Flash Mode**: QIO
     - **Flash Size**: 4MB (or your board's size)
     - **Partition Scheme**: Default 4MB with spiffs
     - **Core Debug Level**: None (or Info for debugging)
     - **Port**: Select your ESP32-C3's COM port

3. **Open and Upload**:
   - Open `deskmate.ino` in Arduino IDE
   - Click the Upload button (→) or press `Ctrl+U`
   - Wait for compilation and upload to complete

#### Option 2: PlatformIO

1. **Install PlatformIO**:
   - Install PlatformIO IDE extension in VS Code
   - Or install PlatformIO Core via pip: `pip install platformio`

2. **Create `platformio.ini`**:
   ```ini
   [env:esp32-c3-devkitm-1]
   platform = espressif32
   board = esp32-c3-devkitm-1
   framework = arduino
   monitor_speed = 115200
   
   lib_deps = 
       majicdesigns/MD_Parola@^3.7.0
       majicdesigns/MD_MAX72XX@^3.3.0
       bblanchon/ArduinoJson@^6.21.3
   ```

3. **Build and Upload**:
   ```bash
   pio run -t upload
   ```

### Optional: Local Development with Secrets

For local development/testing (NOT recommended for production):

1. Copy `secrets_template.h` to `secrets.h`
2. Fill in your credentials in `secrets.h`
3. Add `#define USE_SECRETS` at the top of `deskmate.ino` (before includes)
4. **Never commit `secrets.h` to version control** (it's in `.gitignore`)

## First-Time Setup

### 1. Initial Boot

After flashing the firmware:

1. The device will create an Access Point: **`ESP32-Clock`**
2. Default password: **`clock123`**
3. Connect your computer/phone to this WiFi network

### 2. Web Configuration

1. Open a web browser and navigate to: **`http://192.168.4.1`**
2. Configure the following:
   - **WiFi Credentials**: Your home/office WiFi SSID and password
   - **Time Settings**: NTP server, GMT offset, daylight saving
   - **Weather API**: 
     - Get a free API key from [OpenWeatherMap](https://openweathermap.org/api)
     - Enter your location (latitude, longitude, city name)
   - **Display Settings**: Brightness, clock format (12h/24h)
   - **Reminders**: Up to 3 time-based reminders
   - **Pomodoro Phases**: 4 phases with custom durations (Focus/Break cycles)

3. Click **Save Configuration**
4. The device will reboot and connect to your WiFi

### 3. Verify Connection

- The device should now display the current time
- Press the button to cycle through modes: **Clock → Weather → Pomodoro → Clock**
- Weather mode requires valid API key and location configuration

## Usage

### Clock Mode (Default)
- Displays current time in 12h or 24h format
- Auto-rotates display when device is flipped 180°
- Daily NTP sync at 1:11:11 AM

### Weather Mode
- Press button from Clock mode
- Scrolls current weather conditions
- Shows temperature, humidity, and conditions
- Auto-returns to Clock mode after display

### Pomodoro Mode
- Press button from Clock mode
- **Flip device 180°**: Advance to next phase (works in any direction)
- **Lift device and put back**: Toggle pause/resume
- **4 phases**: Typically Focus → Short Break → Focus → Long Break
- Timer displays in MM:SS format
- Auto-returns to Clock mode when all phases complete

## Configuration Persistence

All settings are stored in EEPROM and persist across:
- Reboots
- Power cycles
- Firmware updates (OTA-ready with versioning)

See [EEPROM_PERSISTENCE.md](EEPROM_PERSISTENCE.md) for technical details.

## File Structure

```
Deskmate/
├── deskmate.ino              # Main firmware file
├── secrets_template.h        # Template for local development
├── .gitignore               # Excludes sensitive files and build artifacts
├── LICENSE                  # MIT License
├── README.md                # This file
├── SECURITY.md              # Security guidelines
├── EEPROM_PERSISTENCE.md    # EEPROM configuration details
└── QUICK_START.md           # Quick setup guide
```

## Troubleshooting

### Device won't connect to WiFi
- Ensure you're using a **2.4GHz network** (ESP32 doesn't support 5GHz)
- Check SSID and password are correct in web interface
- Verify signal strength is adequate
- Try resetting the device and reconfiguring

### Weather not displaying
- Verify OpenWeatherMap API key is correct and active
- Check latitude/longitude coordinates are valid
- Ensure WiFi is connected (device must be online)
- Check Serial Monitor for API error messages

### Display not working
- Verify SPI connections (CLK, DATA, CS pins)
- Check MAX7219 modules are powered (5V)
- Verify brightness setting in web interface
- Check Serial Monitor for initialization errors

### Pomodoro gestures not working
- Verify MPU6050 is connected via I2C (SDA, SCL)
- Check Serial Monitor for MPU6050 initialization messages
- Ensure device is stable when flipping (180° flip required)
- For lift detection, wait for baseline to establish (see Serial Monitor)

### Access Point not appearing
- Hold reset button for 5 seconds to force AP mode
- Check Serial Monitor for AP creation messages
- Verify WiFi credentials aren't blocking AP mode

## Serial Monitor

For debugging, open Serial Monitor at **115200 baud**:
- Shows initialization status
- Displays MPU6050 readings
- Shows WiFi connection status
- Displays NTP sync messages
- Shows Pomodoro state changes

## Security

🔒 **Important**: This firmware does NOT contain hardcoded credentials by default.

- All sensitive data (WiFi passwords, API keys) must be configured via web interface
- Credentials are stored securely in EEPROM
- See [SECURITY.md](SECURITY.md) for detailed security guidelines
- For local development, see `secrets_template.h` (never commit `secrets.h`)

## Contributing

Contributions are welcome! When contributing:

- Follow security guidelines in [SECURITY.md](SECURITY.md)
- **Never commit credentials** or `secrets.h`
- Test changes thoroughly on hardware
- Update documentation as needed
- Follow existing code style

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- Built for ESP32-C3 Super Mini
- Uses MD_Parola and MD_MAX72XX libraries by MajicDesigns
- Weather data provided by OpenWeatherMap API

