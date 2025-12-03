# EEPROM Configuration Persistence

## Overview

The Deskmate clock firmware is designed to **preserve all user configuration across firmware updates**. This means when you flash a new version of the firmware, all your saved settings (WiFi credentials, weather API keys, reminders, Pomodoro phases, etc.) will be automatically preserved.

## How It Works

### Config Versioning

The firmware uses a **config version system** to ensure compatibility:

- **Config Version**: Defined as `CONFIG_VERSION` (currently version 1)
- **Version Field**: Each saved config includes a version number
- **Automatic Migration**: If version differs, config is preserved and version is updated

### EEPROM Storage

- **Location**: Configuration is stored in EEPROM starting at address 0
- **Size**: 1024 bytes (sufficient for all config data)
- **Persistence**: EEPROM data survives firmware flashes
- **Only Cleared**: When explicitly erased or when structure changes incompatibly

### What Gets Preserved

✅ **WiFi Credentials** (SSID, password)  
✅ **Time Settings** (NTP server, timezone, format)  
✅ **Weather Settings** (API key, location, city name)  
✅ **Display Settings** (brightness, clock format)  
✅ **Reminders** (all 3 reminders with times and messages)  
✅ **Pomodoro Phases** (all 4 phases with durations)  

## When Config is Reset

Configuration is **only reset** in these cases:

1. **First Time Setup**: No config exists in EEPROM
2. **Manual Erase**: User explicitly erases EEPROM
3. **Incompatible Change**: Config structure changes in a way that breaks compatibility (version mismatch with version = 0)

## Version Management

### Current Version
- **CONFIG_VERSION = 1** (defined in code)

### Updating Version

If you need to make **incompatible changes** to the Config structure:

1. Increment `CONFIG_VERSION` in the code
2. Add migration logic in `loadConfig()` if needed
3. Old configs will be preserved but version will be updated

### Example Migration

```cpp
// In loadConfig(), after version check:
if (config.version < 2) {
  // Migrate old config format to new format
  // Preserve all user data
  config.version = CONFIG_VERSION;
  saveConfig();
}
```

## OTA Updates

For **Over-The-Air (OTA) updates**:

1. ✅ Config automatically persists
2. ✅ No reconfiguration needed
3. ✅ User settings remain intact
4. ✅ Only new features need setup (if any)

## Testing Persistence

To verify config persistence:

1. **Save configuration** via web interface
2. **Flash new firmware** (even with code changes)
3. **Check Serial Monitor** - should show:
   ```
   Loaded config version: 1 (Current: 1)
   ✓ Valid configuration found - will persist across firmware updates
   ```
4. **Verify settings** - all should be intact

## Troubleshooting

### Config Not Persisting

If config is lost after update:

1. Check Serial Monitor for version mismatch messages
2. Verify `CONFIG_VERSION` hasn't changed
3. Check EEPROM commit success messages
4. Ensure EEPROM wasn't manually erased

### Force Reset (If Needed)

To force a complete reset (clears all config):

1. In Arduino IDE: `Tools` → `Erase Flash: All Flash Contents`
2. Or add temporary code to clear EEPROM:
   ```cpp
   EEPROM.begin(EEPROM_SIZE);
   for (int i = 0; i < EEPROM_SIZE; i++) {
     EEPROM.write(i, 0);
   }
   EEPROM.commit();
   ```

## Best Practices

1. **Never change CONFIG_VERSION** unless making incompatible structure changes
2. **Always preserve user data** when migrating configs
3. **Test updates** with existing configs before releasing
4. **Document changes** when incrementing version

## Technical Details

- **EEPROM Address**: 0 (start of EEPROM)
- **Config Size**: ~400 bytes (varies with string lengths)
- **EEPROM Total**: 1024 bytes (plenty of room for future expansion)
- **Commit Required**: Always call `EEPROM.commit()` after `EEPROM.put()`

