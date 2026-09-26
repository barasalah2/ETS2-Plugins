const { app, BrowserWindow } = require('electron');
const { truckSimTelemetry } = require('trucksim-telemetry');
const fetch = require('node-fetch');
const fs = require('fs');
const path = require('path');

// Load or init cache
let cache = {};
const cacheFile = path.join(__dirname, 'geocodeCache.json');
try {
  cache = JSON.parse(fs.readFileSync(cacheFile, 'utf-8'));
  console.log('Cache loaded with', Object.keys(cache).length, 'entries');
} catch (e) {
  console.log('No cache found, starting fresh');
}

let lastReq = 0;
let win = null;

function createWindow() {
  win = new BrowserWindow({
    width: 400,
    height: 200,
    x: 20,
    y: 20,
    frame: false,
    transparent: true,
    alwaysOnTop: true,
    focusable: false,
    hasShadow: false,
    skipTaskbar: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      nodeIntegration: false,
      contextIsolation: true,
      enableRemoteModule: false,
      sandbox: true
    }
  });

  win.loadFile('index.html');

  // Keep window on top forcefully
  setInterval(() => {
    if (win && !win.isDestroyed()) {
      win.setAlwaysOnTop(true, 'screen-saver');
    }
  }, 5000);

  // Initialize telemetry
  console.log('Initializing telemetry...');
  
  let telemetry = null;
  let connectionTimeout = null;
  let hasConnected = false;
  
  try {
    telemetry = truckSimTelemetry();
    console.log('Telemetry object created');
  } catch (e) {
    console.error('Failed to create telemetry object:', e.message);
    win.webContents.send('status', `Error: ${e.message}`);
    return;
  }

  // Set a timeout warning if not connected after 10 seconds
  connectionTimeout = setTimeout(() => {
    if (!hasConnected) {
      console.warn('⚠ Telemetry not connected after 10 seconds');
      console.warn('Possible causes:');
      console.warn('  1. ETS2 is not running');
      console.warn('  2. scs_sdk_plugin.dll not installed in bin/win_x64/plugins/');
      console.warn('  3. ETS2 needs to be RESTARTED after installing the plugin');
      console.warn('  4. You need to ACCEPT the SDK popup when ETS2 starts');
      win.webContents.send('status', '⚠ Waiting for ETS2 (check console)');
    }
  }, 10000);

  telemetry.on('connected', () => {
    hasConnected = true;
    clearTimeout(connectionTimeout);
    console.log('✓ Telemetry CONNECTED to ETS2');
    win.webContents.send('status', '✓ Connected - Ready!');
  });

  telemetry.on('disconnected', () => {
    console.log('✗ Telemetry DISCONNECTED');
    win.webContents.send('status', '✗ ETS2 Disconnected');
  });

  telemetry.on('error', (err) => {
    console.error('Telemetry error:', err);
    win.webContents.send('status', `Error: ${err.message}`);
  });

  telemetry.on('raw', (raw) => {
    console.log('[RAW EVENT] Raw telemetry:', JSON.stringify(raw, null, 2));
  });

  telemetry.on('data', async data => {
    try {
      console.log('[DATA EVENT] Full telemetry data:', JSON.stringify(data, null, 2));
      if (!data) {
        console.warn('[DATA EVENT] No data received');
        return;
      }
      // Defensive: check for truck and placement
      const x = data?.truck?.placement?.x;
      const z = data?.truck?.placement?.z;
      const speedVal = data?.truck?.speed?.kmh;
      console.log('[DATA EVENT] Extracted:', { x, z, speedVal });

      if (typeof x !== 'number' || typeof z !== 'number') {
        console.warn('[DATA EVENT] No truck position data available', { x, z });
        win.webContents.send('data', {
          road: '', city: '', country: '', speed: '', coords: ''
        });
        return;
      }

      // Convert coords (example origin at Germany and scale ~1:19)
      const [lat, lon] = convertCoords(
        { x, z },
        { x: 0, z: 0 },
        50.0,    // origin latitude
        10.0,    // origin longitude
        1 / 19   // scale factor
      );
      console.log('[DATA EVENT] Converted coords:', { lat, lon });

      const key = `${lat.toFixed(4)},${lon.toFixed(4)}`;

      // Rate-limited geocoding: max 1 request per second
      if (!cache[key] && Date.now() - lastReq > 1000) {
        lastReq = Date.now();
        try {
          const url = `https://nominatim.openstreetmap.org/reverse?format=json&lat=${lat}&lon=${lon}&zoom=18&addressdetails=1`;
          const res = await fetch(url, {
            headers: {
              'User-Agent': 'ETS2-Overlay/1.0 (telemetry-app)'
            }
          });

          if (!res.ok) throw new Error(`HTTP ${res.status}`);

          const json = await res.json();
          const addr = json.address || {};

          cache[key] = {
            road: addr.road || addr.highway || 'Unknown Road',
            city: addr.city || addr.town || addr.village || 'Unknown City',
            country: addr.country || 'Unknown'
          };

          // Save cache
          fs.writeFileSync(cacheFile, JSON.stringify(cache, null, 2));
          console.log('[DATA EVENT] Geocoded:', key, '→', cache[key]);
        } catch (e) {
          console.error('[DATA EVENT] Geocode error:', e.message);
          cache[key] = {
            road: `${lat.toFixed(4)}, ${lon.toFixed(4)}`,
            city: 'Geocoding failed',
            country: '?'
          };
        }
      }

      const info = cache[key] || { road: '---', city: '---', country: '---' };
      const speed = Math.round(speedVal || 0);

      console.log('[DATA EVENT] Sending to UI:', { road: info.road, city: info.city, country: info.country, speed, coords: `${lat.toFixed(4)}, ${lon.toFixed(4)}` });

      win.webContents.send('data', {
        road: info.road,
        city: info.city,
        country: info.country,
        speed: speed,
        coords: `${lat.toFixed(4)}, ${lon.toFixed(4)}`
      });
    } catch (e) {
      console.error('[DATA EVENT] Handler error:', e);
    }
  });
}

// Utility: affine coordinate conversion
function convertCoords(game, originGame, originLat, originLon, scale) {
  const metersPerDegreeLat = 111320; // ~meters per 1° latitude

  // Delta from origin
  const dZ = (game.z - originGame.z) / scale;
  const dX = (game.x - originGame.x) / scale;

  // Convert to lat/lon
  const lat = originLat + dZ / metersPerDegreeLat;
  const metersPerDegreeLon = metersPerDegreeLat * Math.cos((lat * Math.PI) / 180);
  const lon = originLon + dX / metersPerDegreeLon;

  return [lat, lon];
}

app.whenReady().then(createWindow);

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') {
    app.quit();
  }
});

app.on('activate', () => {
  if (BrowserWindow.getAllWindows().length === 0) {
    createWindow();
  }
});
