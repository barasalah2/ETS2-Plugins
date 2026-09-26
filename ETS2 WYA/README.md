# ETS2 Telemetry Overlay Plugin

A real-time overlay for Euro Truck Simulator 2 that displays your current city, road, country, and speed using Electron and the official SCS Telemetry SDK.

## Prerequisites

- **Node.js** (v14+): [Download](https://nodejs.org/)
- **Visual C++ Build Tools** (for native module compilation)
- **Euro Truck Simulator 2** (with telemetry plugin enabled)
- **SCS SDK Plugin**: [Download from RenCloud](https://github.com/Kniffen/scs-sdk-plugin/releases)

## Installation

### Step 1: Install the ETS2 Telemetry Plugin

1. Download the latest `scs_sdk_plugin.dll` from the SCS SDK Plugin releases
2. Locate your ETS2 installation folder (e.g., `C:\Program Files (x86)\Steam\steamapps\common\Euro Truck Simulator 2`)
3. Create a folder: `bin\win_x64\plugins\` (if it doesn't exist)
4. Copy `scs_sdk_plugin.dll` into the `plugins` folder
5. Launch ETS2 and accept the "SDK has been activated" prompt

### Step 2: Install Node Dependencies

```bash
npm install
```

This will install:
- `trucksim-telemetry`: Node.js wrapper for the telemetry SDK
- `node-fetch`: HTTP client for OpenStreetMap API
- `electron`: Framework for the overlay window

## Running the Overlay (Development)

1. Start **Euro Truck Simulator 2**
2. Begin driving or load a save
3. Run the overlay:
   ```bash
   npm start
   ```
4. An overlay window will appear in the top-left corner showing:
   - Current city/town
   - Current road/highway
   - Country
   - Real-time speed

## Building a Shareable Executable

To create a distributable version that others can easily install and run:

### Install Build Tools

```bash
npm install
```

### Build Options

**Option 1: Portable Executable (Recommended for sharing)**
```bash
npm run build:portable
```
This creates a single `.exe` file that runs without installation. Perfect for sharing via email, Discord, or file sharing services.

**Option 2: Windows Installer**
```bash
npm run build:installer
```
This creates an NSIS installer (`.exe`) that users can click to install like a normal Windows app.

**Option 3: Both (Installer + Portable)**
```bash
npm run build
```
This builds both formats.

### Output Location

Built files appear in: `dist/`
- `ETS2-Overlay-1.0.0-portable.exe` — Standalone executable (no installation needed)
- `ETS2-Overlay 1.0.0.exe` — Windows installer (installs to Program Files)

### Sharing Instructions

**For Portable Version:**
1. Share the `.exe` file directly
2. Users download and run it immediately—no installation needed
3. Game data is saved locally in the same folder

**For Installer Version:**
1. Share the installer `.exe`
2. Users run the installer
3. Creates Start Menu shortcuts and adds uninstall option

### Pre-Release Checklist

Before sharing, ensure:

1. ✅ Test on a fresh Windows PC (simulate user experience)
2. ✅ Verify the telemetry plugin is installed in ETS2
3. ✅ Update version number in `package.json` if needed:
   ```json
   "version": "1.0.0"
   ```
4. ✅ Update author name in `package.json`:
   ```json
   "author": "Your Name"
   ```
5. ✅ Create a `SETUP.txt` or include instructions with the `.exe` file:
   ```
   ETS2 Overlay Setup Instructions:
   
   1. Install the SCS SDK Plugin:
      - Download: https://github.com/Kniffen/scs-sdk-plugin/releases
      - Extract scs_sdk_plugin.dll to:
        C:\Program Files (x86)\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\plugins\
   
   2. Run ETS2 Overlay.exe
   3. Start ETS2 and begin driving
   4. The overlay will appear automatically
   ```

The overlay is:
- **Frameless** and **transparent**
- **Always on top** (stays visible while driving)
- **Click-through** (doesn't interfere with gameplay)
- **Real-time** (updates every telemetry frame)

## Coordinate Calibration

To improve accuracy, calibrate the coordinate system:

1. In-game, drive to a location you know the real-world coordinates of (e.g., a famous landmark)
2. Check the console output for the truck's X and Z coordinates
3. Update these values in `main.js` in the `createWindow()` function:

```js
const [lat, lon] = convertCoords(
  { x, z },
  { x: ORIGIN_X, z: ORIGIN_Z },      // Adjust these
  50.0,                                // Adjust latitude
  10.0,                                // Adjust longitude
  1 / 19                               // Scale factor (usually 1:19)
);
```

## Geocoding & Caching

- **OpenStreetMap API**: The app uses Nominatim for reverse geocoding (free, no API key needed)
- **Rate Limiting**: Max 1 request per second to comply with OSM usage policy
- **Caching**: All geocoded locations are cached in `geocodeCache.json` for offline use
- **Offline Mode**: If you lose internet, the overlay will display cached results

## File Structure

```
ets2-overlay/
├── main.js              # Electron main process, telemetry reader, geocoding
├── preload.js           # Secure IPC bridge
├── index.html           # Overlay UI
├── style.css            # Styling for the overlay
├── package.json         # Dependencies
├── geocodeCache.json    # (Auto-created) Cache of coordinates → addresses
└── README.md            # This file
```

## Troubleshooting

### "Telemetry Disconnected"
- Ensure ETS2 is running
- Verify the SCS SDK plugin DLL is in `bin/win_x64/plugins/`
- Restart both ETS2 and the overlay app

### No GPS data appearing
- Check that you're driving in-game (telemetry only streams when actively playing)
- Verify the coordinate origin and scale are correct for your ETS2 map region

### Geocoding showing "Geocoding failed"
- Check your internet connection (OSM Nominatim requires it)
- Verify the coordinates are valid lat/lon
- Check console logs for detailed errors

### High geocoding latency
- The 1 request/second limit is intentional (OSM policy)
- Cached results reuse instantly for the same location
- Pre-populate the cache by driving to different areas

## Performance

- **CPU**: Minimal (~1-2% on modern systems)
- **Memory**: ~150-200 MB (Electron baseline)
- **Network**: ~1 request/sec to OSM API (cached after first lookup)

## License

- **Code**: MIT
- **Dependencies**: MIT (Electron, trucksim-telemetry, node-fetch)
- **OSM Data**: ODbL (OpenStreetMap attribution required if redistributing)

## References

- [SCS SDK Plugin](https://github.com/Kniffen/scs-sdk-plugin)
- [TruckSim Telemetry](https://github.com/Kniffen/trucksim-telemetry)
- [OpenStreetMap Nominatim API](https://nominatim.org/)
- [Electron Documentation](https://www.electronjs.org/docs)

---

**Enjoy your drives with real-time location data!** 🚚
