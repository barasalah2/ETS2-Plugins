# ETS2 City Overlay

An in-game plugin for Euro Truck Simulator 2 that shows the name of the city you're driving through at the top of the screen, and says it out loud with the Windows voice. There's no separate app or window. The game loads it itself, like any other telemetry plugin.

- **In a city:** a banner with the city and country appears for a few seconds, then stays as a dimmed label.
- **Between cities:** a small "Near Poznań · 4 km" label.
- **Low fuel (under 100 L):** a warning with the nearest fuel station ahead of you, its distance, and an arrow pointing at it.
- **Tiredness:** when the game's sleep timer drops under 2 in-game hours, a warning with the nearest place you can sleep ahead (truck stop, parking, rest area or hotel).
- **Plans one stop ahead:** if your fuel range won't reach the station *after* the next one (with a 30 L reserve), you get **REFUEL AT NEXT STATION**, even with more than 100 L in the tank. The same goes for sleep: if the sleep timer won't last to the rest place after the next one, you get **SLEEP AT NEXT STOP**. If you can't even reach the next one, it turns red and says so. With a GPS route that your range (or sleep timer) comfortably covers, these stay quiet.
- **Urgent warnings:** both warnings turn red and pulse when it gets urgent. A warning stays on the place it named until you drive past it, refuel or sleep.
- **Job route:** when you take a job, the plugin works out the route to the company from the game's own road network (the pick-up first, if the cargo isn't loaded yet). The fuel and sleep planning then uses the stops that are really on your route and their road distances, instead of "nearest place ahead".
- **Upcoming-stops strip:** a panel on the right lists what's coming on your route: cities, truck stops, fuel stations, places to sleep, ferries/trains and the destination, each with road km and driving time. The stop a warning names is highlighted. **Ctrl+F10** shows or hides it.
- **Spoken alerts in a natural voice:** a neural voice (Piper, running on your PC, free and offline) announces fuel and sleep warnings, reminders before the stop they name, ferries, border crossings ("Welcome to the Czech Republic. The truck speed limit is 80 kilometres per hour."), and "your destination is 5 kilometres ahead". The game's own navigation voice only has fixed phrases like "turn left", with no city names, so it can't be used for this.
- **Early exit and lane warnings:** on your job route, motorway exits and forks are announced about 15 seconds ahead ("In 6 kilometres, take the exit on the right. Use the right lane.") and again about 6 seconds ahead. Turns and roundabouts get the close call ("at the roundabout, take the second exit"). A panel under the city name shows the next maneuver with its distance and a lane diagram. This only runs while the plugin's route matches your GPS.
- **AI assistant (Gemini, free):** an onboard assistant, like a car's built-in voice assistant, that knows the time, where you are, your cargo and job, what's ahead on the route, and your fuel and sleep. It remembers the conversation. It speaks up by itself for a route briefing when a job starts, facts about towns you drive into and countries you're about to enter, a delivery summary, a note after a fine or a ferry, and a progress update now and then. **Tap Ctrl+F11** to ask about where you are. **Hold Ctrl+F11 and talk** to ask it anything, and let go to send. Its replies are read out in a fish.audio voice (your own clone, for example), a Google voice or the local voice. Needs your Gemini API key (see below).
- **Hidden while paused:** the label disappears in the menu, the world map and other paused screens.
- **Ctrl+F9:** shows or hides the overlay. **Ctrl+F10:** the stops strip. **Ctrl+F11:** the assistant (tap = about here, hold = talk).

## How it works

ETS2 loads every DLL in `bin\win_x64\plugins\` and calls its `scs_telemetry_init` (the official SCS Telemetry SDK). The plugin:

1. Reads the truck's world position from the `truck.world.placement` telemetry channel.
2. Checks it against each city's boundary rectangles, taken from the game's own map data (`data/city_areas.csv`).
3. Hooks the game's DirectX 11 `Present` with MinHook and draws the label with Dear ImGui. This is the same approach other in-game overlays use (for example PrismTextureStreamer).
4. Reads fuel (`truck.fuel.amount`, `truck.fuel.range`) and the sleep timer (`rest.stop`). When either runs low, it looks up the nearest fuel station or sleeping place within ±60° of the truck's heading (`data/pois.csv`, also from the game's map data).
5. With a job, computes the route on a background thread from `data/route_graph.bin` (the game's road graph, ~195,000 road points). It uses the fastest route by the truck speed limits, like the game's GPS default. It follows the truck along the route and re-routes if you leave it for more than a few seconds. Fuel stations and parking count as "on the route" if you can visit them and get back on with a short detour, so a station on the other side of a motorway doesn't count.
6. Learns from your deliveries. On each `job.delivered` event it records where the truck is against the destination city the game reports. This covers cities added by future map DLCs. Learned points go to `learned.csv`.

## Build

You need Visual Studio 2022 (with the "Desktop development with C++" workload) and CMake 3.24 or newer. Dear ImGui and MinHook are downloaded automatically.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The output goes to `build\Release\`: `ets2_city_overlay.dll` plus an `ets2_city_overlay\` data folder.

## Install

```
pwsh -File install.ps1
```

This finds ETS2 in your Steam libraries and copies the files to:

```
Euro Truck Simulator 2\bin\win_x64\plugins\
    ets2_city_overlay.dll
    ets2_city_overlay\cities.csv, city_areas.csv, pois.csv, route_graph.bin, ets2_city_overlay.ini
```

To install by hand, copy the same files there. Each time you start the game it shows an "Advanced SDK features" notice. Click OK so the plugin can load.

The overlay works only with the **DirectX 11** renderer (the default). Check `Documents\Euro Truck Simulator 2\game.log.txt` for `Selected rendering device 'dx11'`.

## Checking that it loaded

Search `game.log.txt` for `[city_overlay]`. A healthy start looks like this:

```
[city_overlay] city table: 374 cities, 1131 boundary areas, 0 learned
[city_overlay] DXGI hooks installed (Present, Present1, ResizeBuffers)
[city_overlay] drawing on swapchain ... (hooked Present)
[city_overlay] entered 'Berlin' (berlin) at x=9905 z=-9929, 12 m from centre
```

If the name is wrong somewhere, the `entered` / `left` lines give the exact position and distance.

## Voice

The natural voice is [Piper](https://github.com/rhasspy/piper) (MIT licence) with a voice from [rhasspy/piper-voices](https://huggingface.co/rhasspy/piper-voices). It runs as a small helper program next to the game at low priority, so the game stays smooth. `install.ps1` downloads it (about 85 MB) into `plugins\ets2_city_overlay\voice\` the first time. To get another voice:

```
pwsh -File tools\get_voice.ps1 -Target "<ETS2>\bin\win_x64\plugins\ets2_city_overlay" -Voice en_US-lessac-high
```

Then set `voice_model=en_US-lessac-high` in the settings. `voice_samples\` has samples of five voices to compare. With `engine=windows`, or if Piper is missing, the Windows voice is used instead.

**Timing:** the game compresses time as well as distance (about 19×), so at 80 km/h the GPS distance drops about 0.4 km every real second. Exit calls are therefore timed in real seconds, not km: "In 6 kilometres" at motorway speed is only about 15 seconds away.

## AI assistant (Google Gemini)

1. Get a free API key at [Google AI Studio](https://aistudio.google.com/apikey).
2. Create `gemini_api_key.txt` in `plugins\ets2_city_overlay\` and paste the key on the first line. Alternatively, set the `GEMINI_API_KEY` environment variable. Keys are never written to the log.
3. Start the game. `game.log.txt` shows `co-driver: Gemini gemini-3.5-flash-lite, voice fish (...), N of 300 requests left today`.

**Models:** `model=gemini-3.5-flash-lite` writes every reply. On the free tier it allows 500 requests a day and 15 a minute, and it answers in about a second. `fallback_models=gemma-4-31b-it` (14,400 a day) is used only when that one is used up for the day or busy. The larger Flash models allow only 20 requests a day, so they aren't used. Google's limits reset at midnight Pacific time; AI Studio shows them under *Rate limits by model*.

**How it works:** each time something happens, Gemini gets a short situation report plus the last ten exchanges of the conversation. It replies with a sentence or two, which is shown on the left and read out. The report covers the game time, the place, speed, the job with cargo, distance and deadline, the next towns, borders and stops on the route, fuel, sleep, and recent fines and tolls. The fuel, sleep and turn warnings stay with the navigation voice, which never depends on the internet. The assistant is told not to give those. Things you tell it about yourself ("call me Bara") go to `codriver_notes.txt` and are remembered on later drives.

**When it speaks up by itself** (`[guide] auto=1`):
- a route briefing when a job starts;
- facts about a city when you drive into it;
- about 25 km before a border, a note about the country;
- after a fine, a ferry or train, or a delivery;
- a progress update every `chat_minutes` (10) of real time, when there's something useful to say. Not near a junction, and not during an urgent warning.

After its last words, it waits 15 seconds before reacting to an event and 30 seconds before a city, border or job briefing. A progress update waits four minutes.

**Talking to it:** hold Ctrl+F11, speak, and let go. The recording (16 kHz, from your default microphone) goes to Gemini, which hears it directly. "Listening…" and "Thinking…" show on the left. A reply takes about 5 to 10 seconds: first the words, then the voice. A quick tap asks about where you are instead. `listen=0` turns the microphone off, so every press becomes a tap.

**Voices for its replies** (`voice_engine=`):
- `fish` (default): [fish.audio](https://fish.audio), for example your own cloned voice. Put the voice's id in `fish_voice=` and the model in `fish_model=`. Put your fish.audio API key on the first line of `fish_audio_key.txt` in `plugins\ets2_city_overlay\`, or set `FISH_AUDIO_API_KEY`. fish.audio only reads the reply out; Gemini still writes it. Each reply takes about 3 to 8 seconds.
- `google`: a Google voice with the same Gemini key, such as `google_voice=Sulafat` (warm), Achird, Puck, Charon, Kore or Aoede. The free tier allows only 10 of these a day.
- `local`: the navigation voice.

If the chosen voice fails, that reply is read by the local voice.

**What gets sent to Google:**
- the situation report;
- the conversation so far;
- screenshots of the game picture (`screenshots=1`), without this plugin's panels;
- your voice while you hold the key (`listen=1`).

fish.audio receives only the text of each reply. On Google's free tier, prompts and answers may be used by Google to improve its products. Nothing is sent when `enabled=0`.

**Limits:** at most one text request every 4 seconds. `daily_limit=300` caps the Google requests a day, counted in `codriver_usage.txt`. Automatic remarks stop at the cap, but you can still ask. If a model hits its per-minute limit, the fallback answers for a while. If every model's free daily limit runs out, it says so and stays quiet until tomorrow.

**If it doesn't work, check the log line:**
- `rejected the API key`: the Gemini key is wrong.
- `model ... not found`: change `[guide] model=`.
- `no Gemini API key`: the file or variable isn't found.
- `fish.audio refused the request (401/402/...)`: wrong key, no credit, or a wrong `fish_voice`/`fish_model`.
- `no microphone could be opened`: Windows has no default recording device, or the plugin isn't allowed to use it (Settings → Privacy → Microphone → let desktop apps access it).

## Settings

Edit `plugins\ets2_city_overlay\ets2_city_overlay.ini`, then restart the game. The file has comments for every option. The main ones are:

| Section | Key | Default | Meaning |
|---|---|---|---|
| overlay | `toggle_key` / `toggle_ctrl` | `0x78` / `1` | Ctrl+F9 toggles the overlay |
| overlay | `font`, `font_size` | Segoe UI Bold, 34 | Font for the label; the size is at 1080p |
| overlay | `position_y` | 0.035 | Top of the label, as a fraction of screen height |
| overlay | `banner_seconds`, `always_show`, `idle_opacity` | 6, 1, 0.70 | How long the banner stays bright, whether a dimmed label stays after it, and how dim |
| voice | `speak` | 0 | Turn the spoken announcement on or off |
| voice | `text` | `Welcome to {city}` | What to say; `{city}` and `{country}` are filled in |
| voice | `voice`, `volume`, `rate` | default, 100, 0 | Voice name to use (e.g. `Zira`), volume, and speed |
| alerts | `enabled` | 1 | Turn the fuel and sleep warnings on or off |
| alerts | `fuel_warn_liters`, `fuel_critical_liters` | 100, 40 | Warn below this; turn red below that |
| alerts | `rest_warn_minutes` | 120 | Warn when sleep is due within this many in-game minutes |
| alerts | `search_angle`, `search_distance_km` | 60, 300 | How wide "ahead" is (degrees either side) and how far to look (game km) |
| alerts | `smart_fuel`, `smart_sleep` | 1, 1 | Plan one stop ahead (see above) |
| alerts | `detour_factor` | 1.3 | Road distance ÷ straight-line distance, used when planning |
| alerts | `reserve_liters` | 30 | Fuel to keep in the tank when planning |
| route | `enabled` | 1 | Work out the job route (used by the planning and the strip) |
| route | `strip`, `strip_key` | 1, `0x79` | Show the upcoming-stops strip; Ctrl + this key (F10) toggles it |
| route | `strip_items` | 7 | Rows in the strip |
| route | `strip_x`, `strip_y` | 0.985, 0.26 | Right edge and top of the strip, as fractions of the screen |
| route | `max_detour_km` | 8 | A stop counts as on your route if visiting it adds at most this many km |
| voice | `engine`, `voice_model` | piper, en_GB-jenny_dioco-medium | Natural voice (Piper) or `windows`; which Piper voice |
| announce | `enabled`, `fuel`, `sleep`, `ferry`, `border`, `destination`, `lanes` | all 1 | What the voice announces |
| announce | `border_km`, `destination_km` | 10, 5 | How far ahead (game km) to announce a border and the destination |
| route | `guidance` | 1 | Panel with the next exit/turn and its lanes |
| guide | `enabled`, `auto`, `key`, `model`, `fallback_models`, `speak`, `show` | 1, 1, `0x7A`, gemini-3.5-flash-lite, gemma-4-31b-it, 1, 1 | AI assistant; Ctrl+F11: tap = about here, hold = talk |
| guide | `voice_engine`, `google_voice`, `google_voice_model` | fish, Sulafat, gemini-3.8-flash-tts | Voice for its replies: fish, google or local |
| guide | `fish_voice`, `fish_model`, `fish_temperature`, `fish_top_p`, `fish_speed` | (your voice id), s2.1-pro-free, 0.7, 0.7, 1.0 | fish.audio voice (key in `fish_audio_key.txt`) |
| guide | `screenshots`, `listen`, `chat_minutes`, `daily_limit` | 1, 1, 10, 300 | Let it see the picture, hear you, chat on quiet stretches; request cap a day |
| detection | `near_km` | 250 | How far away (in game km) the "Near X" label still shows |
| detection | `learn_from_deliveries` | 1 | Record city positions when you deliver jobs |

To get more voices, install them in Windows Settings under Time & language → Speech.

## Distances and calibration

Map coordinates are compressed (about 1:19). The game counts fuel range, the odometer and GPS distance in its own km, and the plugin shows game km too. It learns the exact ratio while you drive by comparing the odometer with the distance covered on the map, separately for open road and cities (the game may compress cities less). The result is saved to `calibration.ini`, so it's right from the next start. The first few measurements go to `game.log.txt`:

```
[city_overlay] calibration (open road): drove 1000 map m = 19.00 odometer km (local.scale 19.0) -> 19.00 game km per map km; GPS left ... m / ... s, speed ... km/h
[city_overlay] route vs game GPS: ours 292 km, GPS 292 km (same route)
```

## Limits of the route

- **The plugin's own calculation:** it computes the route itself; it can't read the line your GPS draws. It picks the fastest route, which is usually the same as the game's. The game reports the distance left on your GPS, and the plugin compares it with its own route. If they differ by more than about 20% (your own waypoints, a different road choice), the strip says **"Your GPS takes a different route"** and the warnings switch back to "nearest place ahead".
- **No route for special transport jobs:** they have fixed routes and no company to route to.
- **Base game and SCS DLCs only:** the road graph comes from them. Map mods like ProMods aren't covered.
- **Game-km scale:** distances and times on the strip use the same learned game-km scale as everything else (see below). When the game's GPS agrees with the route, the strip is scaled to match the GPS exactly.

## Limits of the warnings

- **Straight-line distance:** the distance and arrow are as the crow flies, not along the road. The SDK doesn't expose the road network or your route, so planning multiplies distances by `detour_factor`. A station "18 km to the right" may still take a longer detour to reach, and it can be on the other side of a motorway.
- **Which is "next":** the next and the one after are the closest places in the cone ahead. If you turn off onto another road, the plugin re-plans from there.
- **Sleep planning:** uses your average speed while driving (starting at 65 km/h and learned over time) to turn distance into in-game minutes.
- **No GPS marker:** the plugin can't add a marker to the game's GPS or change the route, because the SDK is read-only. Use the arrow and distance, then set the route in the game yourself if needed.
- **Fatigue off:** if fatigue simulation is off in the game options, there is no sleep timer and no sleep warning.
- **Which sleep timer:** the sleep warnings follow the game's sleep (bed) timer, `rest.stop`. The mandatory break added in 1.60 (the "P" counter) doesn't appear to be in telemetry ([SimHub #2318](https://github.com/SHWotever/SimHub/issues/2318); not yet checked in-game), so the plugin can't warn about it yet.

## City data

`data/cities.csv`, `data/city_areas.csv`, `data/pois.csv` (fuel stations and places to sleep) and `data/route_graph.bin` (the road network) were extracted from the installed game (ETS2 1.61.1, with the base map and every map DLC up to Nordic Horizons). The tool used is the [truckermudgeon/maps](https://github.com/truckermudgeon/maps) parser.

To regenerate them after a map update:

```
git clone https://github.com/truckermudgeon/maps.git C:\tmmaps   (use a short path; MSVC hits the 260-char limit otherwise)
cd C:\tmmaps && npm install && npm run build -w packages/clis/parser
set NODE_OPTIONS=--max-old-space-size=16384
npx tsx packages/clis/parser/index.ts -i "<ETS2 folder>" -o out
python tools/make_map_data.py --parser-out C:\tmmaps\out

rem road graph for the job route, roundabouts and junction lanes:
npx tsx packages/clis/generator/index.ts graph -m europe -i out -o graphout
npx tsx packages/clis/generator/index.ts roundabouts -m europe -i out -g graphout -o graphout
npx tsx export-maneuvers.ts out graphout graphout/europe-maneuvers.csv
python tools/make_route_graph.py --graph C:\tmmaps\graphout\europe-graph.json --map-out C:\tmmaps\out --maneuvers C:\tmmaps\graphout\europe-maneuvers.csv
```

If `npx tsx` picks up a broken local copy inside the repo folder, run it from the parent folder: `cd C:\ && npx -y tsx@4 tmmaps\packages\clis\generator\index.ts graph -m europe -i tmmaps\out -o tmmaps\graphout`.

The 1.61 game files needed two small patches to the parser:

- Skip `def/world/model.stable.sii` in `game-files/def-parser.ts`.
- Wrap the `parseDds` icon call in `game-files/map-files-parser.ts` in a `try` block.

Neither affects city data. If you don't want to run the parser, `python tools/make_map_data.py` rebuilds the city files from Koenvh1's older public lists (MIT), which have positions only and no newer DLCs. It has no fuel or rest data. Learning from deliveries fills in the gaps for cities.

## Testing without the game

`build\Release\test_host.exe <dll> <screenshot dir>` stands in for ETS2. It loads the plugin through the SDK entry points, renders frames on a D3D11 flip-model swapchain, and feeds it positions, pause, resize and a job delivery. It saves a screenshot after each step.

## Files

```
src/plugin.cpp     SDK entry points, telemetry channels/events, city tracking
src/cities.cpp     city table, boundary lookup, learning (learned.csv)
src/pois.cpp       fuel stations / sleeping places, "next" and "after next" search
src/alerts.cpp     fuel/sleep rules (on the route and without one), map-to-game-km calibration
src/route.cpp      road graph, route search (background thread), stops on the route, junction maneuvers, tracking
src/speech.cpp     natural voice (Piper helper process) with Windows-voice fallback, message queue
src/announce.cpp   what the voice says and when (fuel, sleep, ferries, borders, destination, exits and lanes)
src/codriver.cpp   AI assistant: moments, conversation, situation, Gemini request, reply voices
src/gemini.cpp     HTTPS (WinHTTP), Gemini errors and limits, base64, key files
src/capture.cpp    the assistant's screenshot: back buffer copy, read back, JPEG (WIC)
src/mic.cpp        push-to-talk: tap vs hold, microphone recording (waveIn)
src/spoken.h       wording for distances, times, turns and lanes
tools/get_voice.ps1        downloads Piper and a voice
C:\tmmaps\export-maneuvers.ts   (in the parser folder) junction turns and lanes for maneuvers.bin
src/overlay.cpp    DXGI Present/ResizeBuffers hooks + ImGui drawing
src/voice.cpp      Windows text-to-speech on a background thread
src/config.cpp     ets2_city_overlay.ini
src/csv.cpp        CSV reader shared by the data files
tools/             city data generator, test host
third_party/       SCS Telemetry SDK headers
```
