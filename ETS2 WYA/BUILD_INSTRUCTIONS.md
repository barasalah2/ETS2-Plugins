# Build Instructions for Developers

This guide is for developers who want to build the ETS2 Overlay from source.

## Prerequisites

- **Node.js** v14+ (includes npm): https://nodejs.org/
- **Visual C++ Build Tools** (required for native modules):
  - Download: https://visualstudio.microsoft.com/downloads/
  - Select "Desktop development with C++"
- **Git** (optional, for version control): https://git-scm.com/

## Setup

### 1. Clone or Download the Repository

```bash
git clone https://github.com/yourusername/ets2-overlay.git
cd ets2-overlay
```

Or manually download all files into a folder.

### 2. Install Dependencies

```bash
npm install
```

This installs:
- `electron` — Framework for the overlay
- `electron-builder` — Tool for creating installers
- `trucksim-telemetry` — Reads game telemetry
- `node-fetch` — HTTP client for geocoding

## Development

### Run in Development Mode

```bash
npm start
```

This launches the overlay in debug mode. You can use DevTools:
- Press `Ctrl+Shift+I` in the overlay window to open Developer Tools

### Hot Reload (Optional)

For faster development, you can set up hot reload:

1. Install `electron-reload`:
   ```bash
   npm install --save-dev electron-reload
   ```

2. Add to top of `main.js`:
   ```js
   require('electron-reload')(__dirname);
   ```

## Building for Distribution

### Build Both Formats (Recommended)

```bash
npm run build
```

This creates:
- `dist/ETS2-Overlay-1.0.0-portable.exe` — Standalone (no install needed)
- `dist/ETS2-Overlay 1.0.0.exe` — Installer

### Build Only Portable

```bash
npm run build:portable
```

Output: `dist/ETS2-Overlay-1.0.0-portable.exe`

### Build Only Installer

```bash
npm run build:installer
```

Output: `dist/ETS2-Overlay 1.0.0.exe`

## Customization

### Change App Name/Version

Edit `package.json`:
```json
{
  "name": "ets2-overlay",
  "version": "1.0.1",
  "author": "Your Name"
}
```

### Change UI/Styling

Edit `style.css` for colors, fonts, and layout.

### Change Overlay Position/Size

Edit `main.js` in the `createWindow()` function:
```js
const win = new BrowserWindow({
  width: 400,      // Change width
  height: 150,     // Change height
  // ... other options
});
```

Position is controlled in `style.css`:
```css
#overlay {
  top: 15px;       /* Distance from top */
  left: 15px;      /* Distance from left */
}
```

### Add Coordinate Calibration

Edit the origin point in `main.js`:
```js
const [lat, lon] = convertCoords(
  { x, z },
  { x: 0, z: 0 },    // Change origin point
  50.0,              // Change latitude
  10.0,              // Change longitude
  1 / 19             // Change scale factor
);
```

Common calibration values:
- **Germany:** `lat: 50.0, lon: 10.0`
- **France:** `lat: 46.5, lon: 2.0`
- **UK:** `lat: 54.0, lon: -2.0`
- **Turkey:** `lat: 39.0, lon: 35.0`

## Distribution Checklist

Before releasing:

- [ ] Test on clean Windows 10/11 install
- [ ] Verify telemetry plugin installation works
- [ ] Test both .exe formats (portable and installer)
- [ ] Include `SETUP_GUIDE.md` with release
- [ ] Update version number in `package.json`
- [ ] Update `README.md` with any changes
- [ ] Test offline geocoding (cached results)
- [ ] Verify overlay positioning on different screen resolutions

## Troubleshooting Build Issues

### "electron-builder not found"

```bash
npm install --save-dev electron-builder
```

### "Build failed: missing files"

Ensure all these files exist in the project root:
- `main.js`
- `preload.js`
- `index.html`
- `style.css`
- `package.json`

### "Cannot find module: trucksim-telemetry"

```bash
npm install trucksim-telemetry
```

If it fails, ensure Visual C++ Build Tools are installed (requires native compilation).

### Large file size

Normal Electron app sizes:
- Portable `.exe`: 200-300 MB (includes Chromium)
- Installer: Similar (extracts on install)

This is expected. Most size is from Electron itself.

## Release Process

1. **Bump version** in `package.json`
2. **Test thoroughly** on Windows 10/11
3. **Run build**:
   ```bash
   npm run build
   ```
4. **Verify output** in `dist/` folder
5. **Create release** on GitHub:
   - Upload `.exe` files
   - Include `SETUP_GUIDE.md`
   - Add release notes
6. **Share link** with users

## Advanced: Code Signing

For production releases, you can sign the executable:

1. Obtain a code signing certificate
2. Add to `package.json` `build` section:
   ```json
   "certificateFile": "path/to/cert.pfx",
   "certificatePassword": "your-password"
   ```
3. Run `npm run build`

This prevents Windows Smartscreen warnings.

## Advanced: Auto-Updates

To add automatic updates:

1. Install `electron-updater`:
   ```bash
   npm install electron-updater
   ```

2. Configure in `main.js` (see electron-updater docs)

3. Host updates on GitHub Releases or S3

## Support

For issues with:
- **Electron:** https://www.electronjs.org/docs
- **electron-builder:** https://www.electron.build/
- **trucksim-telemetry:** https://github.com/Kniffen/trucksim-telemetry
- **OpenStreetMap:** https://nominatim.org/

---

Happy building! 🚀
