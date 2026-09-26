# ETS2 Overlay - User Setup Guide

This guide is for users who downloaded the **ETS2-Overlay.exe** file.

## Requirements

- Windows 10 or 11 (64-bit)
- Euro Truck Simulator 2 (Steam)
- ~300 MB free disk space
- Internet connection (for first-time geocoding)

## Step 1: Install the SCS Telemetry Plugin

The ETS2 overlay requires the official telemetry plugin to read game data.

### Download

1. Go to: https://github.com/Kniffen/scs-sdk-plugin/releases
2. Download the **latest** `scs_sdk_plugin.zip`
3. Extract the `.zip` file
4. Find `scs_sdk_plugin.dll` inside

### Install

1. Locate your ETS2 folder:
   - **Steam Path:** `C:\Program Files (x86)\Steam\steamapps\common\Euro Truck Simulator 2`
   
2. Navigate to: `bin\win_x64\plugins\`
   - If the `plugins` folder doesn't exist, **create it**

3. Copy `scs_sdk_plugin.dll` into the `plugins` folder

4. Launch ETS2 and **accept the SDK prompt** (shown at game startup)
   - You should see: *"SDK has been activated"*

## Step 2: Run the ETS2 Overlay

**Method 1: Using the Portable Executable (Recommended)**
- Double-click `ETS2-Overlay-portable.exe`
- No installation needed—it runs immediately

**Method 2: Using the Installer**
- Double-click `ETS2-Overlay-1.0.0.exe`
- Click "Install" and follow prompts
- Find the app in your Start Menu

## Step 3: Start Playing

1. Open **Euro Truck Simulator 2**
2. Load a save or start a new game
3. Begin driving
4. The overlay will appear in the **top-left corner** of your screen

### What You'll See

- **City:** Current city or town name
- **Road:** Current street, highway, or route
- **Country:** Current country (e.g., Germany, France)
- **Speed:** Real-time speed in km/h

## Calibration (Optional)

If the geocoding seems off or shows wrong locations:

1. Drive to a landmark you recognize (e.g., a famous city)
2. Check the console/debug output
3. Note the X and Z coordinates
4. Contact the developer with the coordinates for calibration

For most of Europe, the default calibration should work fine.

## Troubleshooting

### "Telemetry Disconnected" or overlay shows no data

**Solution:**
1. Verify `scs_sdk_plugin.dll` is in `Euro Truck Simulator 2\bin\win_x64\plugins\`
2. Restart ETS2 completely
3. Accept the SDK activation prompt when ETS2 launches
4. Restart the overlay app

### "Geocoding failed" or shows coordinates instead of city names

**Possible causes:**
- No internet connection (required for first lookup)
- Coordinates are outside mapped regions
- OpenStreetMap is temporarily unavailable

**Solution:**
- Check your internet connection
- Results are cached, so subsequent visits to the same location will be instant
- Wait a moment and try again

### Overlay is hidden or not visible

**Solution:**
1. The overlay might be behind other windows
2. Try moving your mouse to **top-left corner** of screen
3. Close and restart the overlay app

### Overlay crashes on startup

**Solution:**
1. Ensure Node.js and Visual C++ runtime are installed
2. Delete `geocodeCache.json` (the cache file) and restart
3. Update Windows to the latest version

## Performance Impact

- **CPU:** ~1-2% (minimal)
- **Memory:** ~150 MB
- **Disk:** Very low (cache file only)
- **Network:** ~1 request/second max (cached after first lookup)

## Privacy

- Your location data is only used for local reverse-geocoding
- Coordinates are sent to **OpenStreetMap Nominatim** (free, public service)
- No personal data is collected or stored
- All code is open-source (MIT license)

## Tips

- The overlay **doesn't interfere** with gameplay (click-through)
- It **stays on top** even if ETS2 loses focus
- Drive to new locations to populate the cache
- Cached results work **offline**—no internet needed after first lookup

## Support

If you encounter issues:

1. Check the troubleshooting section above
2. Verify the telemetry plugin is correctly installed
3. Contact the developer with your error details

---

**Enjoy your drives with live location data!** 🚚
