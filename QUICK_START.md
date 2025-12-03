# Quick Start Guide

## First Time Setup

1. **Flash the firmware** to your ESP32-C3 Super Mini
2. **Wait 10-15 seconds** for the Access Point to initialize
3. **Connect to WiFi network**: `ESP32-Clock` (password: `clock123`)
4. **Open browser**: `http://192.168.4.1`
5. **Configure settings** via the web interface:
   - WiFi SSID and password
   - Timezone (GMT offset)
   - Weather API key and location
   - Display brightness
   - Reminders (up to 3)
   - Pomodoro timer phases (4 phases, duration only)
6. **Click "Save & Apply"**
7. Device will reconnect and use your settings

## Troubleshooting

### AP Not Connecting
- Wait 15-20 seconds after power on
- Try disconnecting and reconnecting your WiFi
- Check Serial Monitor for AP status
- Restart the ESP32 if needed

### Configuration Not Saving
- Make sure you click "Save & Apply" button
- Check Serial Monitor for "Configuration saved" message
- If issues persist, try erasing EEPROM and reconfiguring

### Using secrets_template.h (Optional - For Development Only)

**You don't need this for normal use!** The web interface is the recommended method.

If you want to use `secrets_template.h` for local development:

1. Copy `secrets_template.h` to `secrets.h`
2. Fill in your credentials in `secrets.h`
3. In Arduino IDE, go to: `File` → `Preferences` → `Compiler warnings: None`
4. Add this line at the top of `deskmate.ino` (before includes):
   ```cpp
   #define USE_SECRETS
   ```
5. Compile and upload

**Important**: 
- `secrets.h` is gitignored - never commit it
- Only use for development/testing
- For production, always use web interface

## Features

- **Clock Mode**: 12h/24h format with NTP sync
- **Weather Mode**: Press button to view weather
- **Pomodoro Mode**: 4-phase productivity timer
  - Flip device 180° to advance phase
  - Lift device to pause, put back to resume
- **Reminders**: Up to 3 time-based reminders

## Button Controls

- **From Clock**: Press → Weather mode
- **From Weather**: Press → Pomodoro mode  
- **From Pomodoro**: Press → Clock mode

