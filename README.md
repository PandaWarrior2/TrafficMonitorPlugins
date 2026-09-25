# TrafficMonitor plugins: Claude Usage and Open-Meteo

Two plugins for [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor), the Windows network/hardware monitor that lives in the taskbar:

| Plugin | What it shows |
|---|---|
| [**Claude Usage**](#claude-usage) | How much of your Claude Pro/Max subscription limits is left — the 5-hour session, the week, and the weekly Fable limit — and when each one resets. |
| [**Open-Meteo**](#open-meteo) | Current weather and a forecast for today and tomorrow from [Open-Meteo.com](https://open-meteo.com). The sky can be shown as a color or monochrome icon. No API key needed. |

Both are native x64 DLLs with no dependencies beyond Windows itself.

## Requirements

- Windows 10 or 11, **x64**
- TrafficMonitor **x64**, 1.86 or later (plugin API 7; tested with 1.86)
- Claude Usage only: a Claude Pro or Max subscription and [Claude Code](https://code.claude.com) signed in on the same Windows account

## Installation

1. Get the DLLs: build them from source (see [Building](#building)) or download them from this repository's Releases page, if it has one.
2. Close TrafficMonitor and copy `ClaudeUsage.dll` and/or `OpenMeteo.dll` into the `plugins` folder next to `TrafficMonitor.exe`.
3. Start TrafficMonitor. Right-click it → **Display Settings…** and turn on the cells you want (they're listed as `Claude: …` and `Open-Meteo: …`).
4. To change a plugin's settings: right-click → **Other Functions** → **Plugin-in Manage…** → select the plugin → **Options…**.

Each plugin keeps its settings in an `.ini` file (`ClaudeUsage.ini`, `OpenMeteo.ini`) in TrafficMonitor's plugin config folder. For the portable version that's the same `plugins` folder.

---

## Claude Usage

Shows what's left of your Claude subscription limits, using the same data as the `/usage` command in Claude Code.

### Cells

| Cell in Display Settings | Label | Example |
|---|---|---|
| Claude: 5-hour limit | `Claude 5h:` | `96% · 3h 41m` |
| Claude: weekly limit | `Claude 7d:` | `97% · 5d 01h` |
| Claude: weekly Fable limit | `Fable 7d:` | `97% · 5d 01h` |

Each cell shows the **remaining** percentage and the time until that limit resets. The countdown is recalculated every second; the percentage is refreshed at the polling interval. If TrafficMonitor's resource usage graph is enabled for the taskbar, the bar under each cell shows what's left. The Fable cell shows `—` if your plan has no separate Fable limit.

Hovering over TrafficMonitor shows a summary, including any other per-model weekly limits:

```
Claude — remaining limits
Session (5h): 96% left, resets in 3h 41m (Sep 26, 2:10)
Week: 97% left, resets in 5d 01h (Oct 1, 0:00)
Week, Fable: 97% left, resets in 5d 01h (Oct 1, 0:00)
Updated: Sep 25, 22:28 (polling every 10 min)
```

### Settings

**Options… → Refresh limits:** every 1, 2, 3, 5, 10, 15 or 30 minutes, or every hour. The default is **5 minutes**. The `.ini` key is `poll_interval_min`.

The endpoint is rate-limited aggressively, and polling more often than every 3 minutes may start returning HTTP 429. After a 429 or a server error the plugin waits 5 minutes (or the polling interval, if that's longer) before trying again.

### How it gets the data

- The plugin reads the OAuth access token that Claude Code stores in `%USERPROFILE%\.claude\.credentials.json` (or in `%CLAUDE_CONFIG_DIR%` if that variable is set). Every user sees their own limits; no token is built into the DLL.
- It calls `GET https://api.anthropic.com/api/oauth/usage` over HTTPS. The token is sent only to `api.anthropic.com` and is never written anywhere.
- The plugin **never refreshes the token**. Claude Code does that whenever it runs, and the plugin re-reads the file before every request. If you don't use Claude Code for a while, the token expires and the cells show `token expired` until you start Claude Code again.

### Messages in the cells

| Message | Meaning |
|---|---|
| `loading…` | The first request hasn't finished yet. |
| `not signed in` | No token file. Sign in to Claude Code (`claude /login`). |
| `token expired`, `token rejected` | Start Claude Code so it refreshes the sign-in. |
| `offline` | No network connection. |
| `HTTP 429` and other codes | The server returned an error; the plugin retries automatically. |
| `bad response` | The response format has changed; the plugin needs an update. |

After a failed request the last good values stay on screen for up to 10 minutes (or 3 polling intervals, if that's longer); the error is shown in the tooltip.

> [!IMPORTANT]
> `/api/oauth/usage` is an **undocumented** endpoint used by Claude Code. Anthropic may change or restrict it at any time, and this plugin may stop working. This project is not affiliated with or endorsed by Anthropic. Claude is a trademark of Anthropic.

---

## Open-Meteo

Current weather plus today's and tomorrow's forecast from [Open-Meteo](https://open-meteo.com), a free weather API that needs no key.

### Cells

| Cell in Display Settings | Label | Example |
|---|---|---|
| Open-Meteo: weather | `Weather:` | ☁️ `20°C` (icon) or `20°C Overcast` (text) |
| Open-Meteo: temperature | `Temp:` | `20°C` |
| Open-Meteo: feels like | `Feels:` | `19°C` |
| Open-Meteo: condition | `Now:` | ☁️ (icon) or `Overcast` (text) |
| Open-Meteo: wind | `Wind:` | `13 km/h N`, `calm` |
| Open-Meteo: humidity | `Humidity:` | `55%` |
| Open-Meteo: pressure | `Pressure:` | `1018 hPa` |
| Open-Meteo: today's low/high | `Today:` | `14…24°C` |
| Open-Meteo: chance of precipitation today | `Precip:` | `0%` |

- Wind direction is where the wind blows **from** (N, NE, E, …). Pressure is reduced to sea level.
- Humidity and chance of precipitation also feed TrafficMonitor's resource usage graph.
- In icon mode the **Weather** and **Condition** cells are drawn by the plugin: the icon takes the place of the label, so a custom label set in TrafficMonitor's Display Text settings doesn't apply to these two cells.

The tooltip has the full picture:

```
London, England, United Kingdom — overcast
Temperature 21°C, feels like 19°C
Wind 13 km/h N, gusts up to 30 km/h
Humidity 55%, cloud cover 100%, pressure 1018 hPa
Today: 14…24°C, overcast, 0% chance of precipitation
Tomorrow: 13…20°C, overcast, 0% chance of precipitation
Sunrise 6:51, sunset 18:52 (local time)
Updated at 22:29 · data by Open-Meteo.com
```

Sunrise and sunset are in the city's local time; "Updated at" is your computer's time. Times follow your Windows regional format (12- or 24-hour).

The right-click menu of a weather cell has a **Refresh weather** command that fetches new data right away.

### Weather icons

Icons come from fonts that ship with Windows: Segoe UI Emoji for color icons and Segoe UI Symbol for monochrome ones (drawn in TrafficMonitor's text color). Clear and mostly clear skies get a moon at night.

| Weather (WMO code) | Day | Night |
|---|---|---|
| Clear sky (0) | ☀️ | 🌙 |
| Mainly clear (1) | 🌤️ | 🌙 |
| Partly cloudy (2) | ⛅ | ☁️ |
| Overcast (3) | ☁️ | ☁️ |
| Fog (45, 48) | 🌫️ | 🌫️ |
| Light or moderate drizzle (51, 53), rain showers (80, 81) | 🌦️ | 🌧️ |
| Dense or freezing drizzle, rain, freezing rain, violent showers (55–67, 82) | 🌧️ | 🌧️ |
| Snow, snow grains, snow showers (71–77, 85, 86) | 🌨️ | 🌨️ |
| Thunderstorm, with or without hail (95, 96, 99) | ⛈️ | ⛈️ |

Icons render with proper transparency on both the classic GDI taskbar and TrafficMonitor's Direct2D taskbar on Windows 11 with a transparent background.

### Settings

| Option | Choices | Default | `.ini` key and values |
|---|---|---|---|
| City | search by name (Open-Meteo geocoding) | none | `city`, `latitude`, `longitude` |
| Temperature | °C, °F | °C | `temperature_unit` = `C` / `F` |
| Wind | m/s, km/h, mph | km/h | `wind_unit` = `ms` / `kmh` / `mph` |
| Pressure | mmHg, hPa | hPa | `pressure_unit` = `mmhg` / `hpa` |
| Sky | color icon, mono icon, text | color icon | `condition_style` = `color` / `mono` / `text` |
| Refresh | every 10, 15, 30 min or every hour | 15 min | `interval_min` (5–1440) |

To pick a city, type its name, press **Search** (or Enter), and choose one of the matches; each shows its region, country and coordinates. Changing units or the sky style takes effect immediately; changing the city fetches new data right away. The `.ini` file is saved as UTF-16, so city names in any language are preserved.

Until a city is selected, the cells show `no city`. After a failed request the plugin retries in 1 minute (15 minutes after HTTP 429 or a server error); the last good values stay on screen for up to 45 minutes (or 3 refresh intervals, if that's longer).

### Data and terms

Weather data by [Open-Meteo.com](https://open-meteo.com), licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). The free API is intended for non-commercial use; see Open-Meteo's [terms](https://open-meteo.com/en/terms). At the default 15-minute interval the plugin makes about 100 requests a day, well within the free tier.

---

## Building

### Prerequisites

- Visual Studio 2026 or Build Tools for Visual Studio 2026 with the **Desktop development with C++** workload (MSVC v145 toolset and a Windows SDK).
- With Visual Studio 2022, change `<PlatformToolset>v145</PlatformToolset>` to `v143` in the `.vcxproj` files. This hasn't been tested.

### Build, test, install

Each plugin folder has a `build.ps1` that finds MSBuild through `vswhere` and builds Release x64:

```powershell
cd OpenMeteoPlugin          # or ClaudeUsagePlugin

.\build.ps1                 # build bin\Release\OpenMeteo.dll and SmokeTest.exe
.\build.ps1 -Test           # build, then run SmokeTest (makes a real network request)
.\build.ps1 -Install -TrafficMonitorDir "C:\Tools\TrafficMonitor"
```

- If PowerShell refuses to run the script, use `powershell -ExecutionPolicy Bypass -File .\build.ps1`.
- Without `-TrafficMonitorDir`, `-Install` assumes TrafficMonitor lives in the folder that contains the plugin folder.
- `-Install` works while TrafficMonitor is running: the loaded DLL can't be overwritten, so the script moves it to `obj\previous` and copies the new one in its place. Restart TrafficMonitor to load it.
- If TrafficMonitor is installed under `Program Files`, run the script from an elevated PowerShell.

The DLLs link the C runtime statically, so users don't need the Visual C++ Redistributable.

### SmokeTest

`tools\SmokeTest` loads a plugin DLL the same way TrafficMonitor does, calls `DataRequired()` once a second until the first cell changes, and prints every cell and the tooltip. It's the quickest way to check a change without restarting TrafficMonitor.

```powershell
bin\Release\SmokeTest.exe                        # load the DLL next to it and print the cells
bin\Release\SmokeTest.exe --options              # open the plugin's Options dialog first
bin\Release\SmokeTest.exe --render cells.bmp     # Open-Meteo: draw the icon cells into a bitmap
```

SmokeTest keeps the plugin's `.ini` next to itself in `bin\Release`, so it doesn't touch your TrafficMonitor settings. `--render` draws the custom-drawn cells on dark and light backgrounds in three modes: a 32-bit DIB, a device-dependent bitmap, and a simulation of TrafficMonitor's Direct2D transparent taskbar.

### Repository layout

```
.
├── README.md
├── ClaudeUsagePlugin/
│   ├── ClaudeUsage.vcxproj
│   ├── build.ps1
│   ├── src/                 ClaudeUsage.cpp, ClaudeUsage.rc, resource.h
│   ├── include/             PluginInterface.h (TrafficMonitor 1.86, plugin API 7)
│   ├── third_party/         nlohmann/json.hpp 3.12.0
│   └── tools/               SmokeTest.cpp, SmokeTest.vcxproj
└── OpenMeteoPlugin/
    ├── OpenMeteo.vcxproj
    ├── build.ps1
    ├── src/                 OpenMeteo.cpp, OpenMeteo.rc, resource.h
    ├── include/             PluginInterface.h
    ├── third_party/         nlohmann/json.hpp
    └── tools/               SmokeTest.cpp, SmokeTest.vcxproj
```

Each plugin folder is self-contained and builds on its own.

### Implementation notes

- **Threading.** TrafficMonitor calls `DataRequired()` about once a second on its monitoring thread and asks for cell text and tooltips on the UI thread. The plugins never touch the network in `DataRequired()`: requests run on a background thread (WinHTTP), and the results are shared under a mutex.
- **Unloading.** On exit TrafficMonitor calls `FreeLibrary` on plugins while a request may still be in flight, so each DLL pins itself in the process (`GetModuleHandleEx` with `GET_MODULE_HANDLE_EX_FLAG_PIN`).
- **Icons.** GDI can't draw color fonts, so Open-Meteo draws icons with a Direct2D DC render target. On a 32-bit DIB (which is what TrafficMonitor's Direct2D taskbar hands to plugins) it uses premultiplied alpha, which TrafficMonitor keeps as is; on other DCs it ignores alpha. Text is drawn with `DrawTextW`, which TrafficMonitor intercepts in Direct2D mode to render it with transparency.

## Third-party components and data

- **nlohmann/json** 3.12.0 (`third_party/nlohmann/json.hpp`) — © Niels Lohmann, [MIT License](https://github.com/nlohmann/json/blob/develop/LICENSE.MIT).
- **PluginInterface.h** (`include/PluginInterface.h`) — the plugin interface of [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor), © zhongyang219, distributed under the ["Anti 996" License 1.0](https://github.com/zhongyang219/TrafficMonitor/blob/master/LICENSE).
- **Weather data** — [Open-Meteo.com](https://open-meteo.com), [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
- **Claude usage data** — Anthropic's undocumented `api/oauth/usage` endpoint; see the note in [Claude Usage](#claude-usage).
