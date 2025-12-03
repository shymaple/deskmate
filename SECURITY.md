# Security Guide for Deskmate Clock Firmware

## 🔒 Security Best Practices

This firmware is designed with security in mind. **No credentials are hardcoded by default.**

## Sensitive Information

The following information should **NEVER** be committed to version control:

1. **WiFi Credentials**
   - SSID (network name)
   - Password

2. **API Keys**
   - OpenWeatherMap API key
   - Any other third-party API keys

3. **Personal Location Data**
   - Latitude/Longitude coordinates
   - City name (optional, less sensitive)

## Configuration Methods

### Method 1: Web Interface (Recommended for Production)

1. Flash the firmware to your ESP32
2. On first boot, the device creates an Access Point: `ESP32-Clock`
3. Connect to this network (password: `clock123`)
4. Open browser to `http://192.168.4.1`
5. Configure all settings via the web interface
6. Credentials are stored securely in EEPROM

**Advantages:**
- ✅ No credentials in source code
- ✅ Safe to share code publicly
- ✅ Easy to reconfigure without reflashing

### Method 2: Local Development (Optional)

For local development/testing only:

1. Copy `secrets_template.h` to `secrets.h`
2. Fill in your credentials in `secrets.h`
3. In Arduino IDE, add `#define USE_SECRETS` in your build flags or at the top of the file
4. Compile and upload

**Important:**
- ⚠️ `secrets.h` is gitignored - never commit it
- ⚠️ Only use for local development
- ⚠️ Don't share `secrets.h` with others

## File Structure

```
Deskmate/
├── deskmate.ino          # Main firmware (safe to commit)
├── secrets_template.h     # Template (safe to commit)
├── secrets.h              # Your credentials (gitignored - DO NOT COMMIT)
├── .gitignore            # Excludes secrets.h
└── SECURITY.md           # This file
```

## What's Protected

The `.gitignore` file automatically excludes:
- `secrets.h` and any `*_secrets.h` files
- Build artifacts
- IDE configuration files

## If You Accidentally Committed Credentials

If you've already committed credentials to a repository:

1. **Immediately rotate/change all exposed credentials:**
   - Change WiFi password
   - Regenerate API keys
   - Update location if concerned about privacy

2. **Remove from Git history:**
   ```bash
   git filter-branch --force --index-filter \
     "git rm --cached --ignore-unmatch deskmate.ino" \
     --prune-empty --tag-name-filter cat -- --all
   ```

3. **Force push (if using remote):**
   ```bash
   git push origin --force --all
   ```

4. **Consider the repository compromised** and create a new one if it's public.

## Security Checklist

Before sharing your code:

- [ ] No WiFi passwords in source code
- [ ] No API keys in source code
- [ ] No personal location data (if sensitive)
- [ ] `secrets.h` is in `.gitignore`
- [ ] `secrets.h` is not committed
- [ ] All credentials configured via web interface

## Questions?

If you have security concerns or find vulnerabilities, please:
1. Do not create a public issue
2. Contact the maintainer privately
3. Describe the issue without exposing credentials

