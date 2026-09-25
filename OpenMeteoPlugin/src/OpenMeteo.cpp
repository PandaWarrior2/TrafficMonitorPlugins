// OpenMeteo — TrafficMonitor plugin: current weather plus a forecast for today and tomorrow from Open-Meteo.com.
//
// Data: https://open-meteo.com (CC BY 4.0), no API key needed. The city is looked up through
// geocoding-api.open-meteo.com in the Options… dialog. Settings live in <plugins folder>\OpenMeteo.ini.

#include <nlohmann/json.hpp>   // before windows.h; NOMINMAX is set in the project

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <winhttp.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "PluginInterface.h"
#include "resource.h"

using json = nlohmann::json;
using Microsoft::WRL::ComPtr;

extern "C" IMAGE_DOS_HEADER __ImageBase;   // HINSTANCE of this DLL, for the dialog resource

namespace
{
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr long long kTicksPerMinute = 60LL * 10'000'000;   // FILETIME: 100 ns
constexpr ULONGLONG kRetryMs = 60 * 1000;                  // after a network error
constexpr ULONGLONG kThrottleMs = 15 * 60 * 1000;          // after 429 and server errors
constexpr int kDefaultIntervalMin = 15;                    // Open-Meteo updates current conditions every 15 minutes
constexpr UINT WM_APP_SEARCH_DONE = WM_APP + 1;

// ---------------------------------------------------------------------------------------------
// Strings, numbers, time

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string WideToUtf8(const std::wstring& s)
{
    if (s.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Trim(const std::wstring& s)
{
    const size_t b = s.find_first_not_of(L" \t");
    if (b == std::wstring::npos)
        return {};
    return s.substr(b, s.find_last_not_of(L" \t") - b + 1);
}

std::wstring UrlEncode(const std::wstring& s)
{
    std::wstring out;
    for (const unsigned char c : WideToUtf8(s))
    {
        const bool plain = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           c == '-' || c == '_' || c == '.' || c == '~';
        if (plain)
        {
            out += static_cast<wchar_t>(c);
        }
        else
        {
            wchar_t buf[4];
            swprintf_s(buf, L"%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// Always with a dot, regardless of the locale (used in URLs, the ini and the UI).
std::wstring FormatFixed(double v, int digits)
{
    char buf[32];
    const auto result = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, digits);
    return std::wstring(buf, result.ptr);
}

bool ParseCoord(const std::wstring& s, double& out)
{
    const std::string a = WideToUtf8(Trim(s));
    const auto result = std::from_chars(a.data(), a.data() + a.size(), out);
    return !a.empty() && result.ec == std::errc() && result.ptr == a.data() + a.size();
}

long long NowTicks()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// Clock time in the Windows regional format: "22:07" or "10:07 PM".
std::wstring FormatClock(WORD hour, WORD minute)
{
    SYSTEMTIME st{};
    st.wHour = hour;
    st.wMinute = minute;
    wchar_t buf[32];
    if (!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, buf, 32))
        swprintf_s(buf, L"%02d:%02d", hour, minute);
    return buf;
}

// Local time of this computer.
std::wstring FormatLocalTime(long long ticks)
{
    const FILETIME ft{ static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32) };
    SYSTEMTIME utc{}, local{};
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
        return L"?";
    return FormatClock(local.wHour, local.wMinute);
}

// ---------------------------------------------------------------------------------------------
// Settings

enum class TempUnit { Celsius, Fahrenheit };
enum class WindUnit { Ms, Kmh, Mph };
enum class PressureUnit { MmHg, HPa };
enum class ConditionStyle { ColorIcon, MonoIcon, Text };   // how the Weather and Condition cells show the sky

struct Location
{
    std::wstring name;   // "Moscow, Russia"
    double lat = kNaN;
    double lon = kNaN;

    bool Valid() const { return !std::isnan(lat) && !std::isnan(lon); }
};

bool SameLocation(const Location& a, const Location& b)
{
    return a.Valid() == b.Valid() && (!a.Valid() || (a.lat == b.lat && a.lon == b.lon));
}

struct Settings
{
    Location location;
    TempUnit temp = TempUnit::Celsius;
    WindUnit wind = WindUnit::Kmh;
    PressureUnit pressure = PressureUnit::HPa;
    ConditionStyle condition = ConditionStyle::ColorIcon;
    int intervalMin = kDefaultIntervalMin;
};

struct IntervalChoice
{
    int minutes;
    const wchar_t* text;
};
constexpr IntervalChoice kIntervalChoices[] = {
    { 10, L"every 10 min" }, { 15, L"every 15 min" }, { 30, L"every 30 min" }, { 60, L"every hour" },
};

std::wstring g_iniPath;   // empty until TrafficMonitor passes the config folder; UI thread only

std::wstring IniRead(const wchar_t* key, const wchar_t* def)
{
    wchar_t buf[512];
    GetPrivateProfileStringW(L"config", key, def, buf, static_cast<DWORD>(std::size(buf)), g_iniPath.c_str());
    return buf;
}

Settings ReadSettingsFile()
{
    Settings s;
    s.location.name = IniRead(L"city", L"");
    double lat = 0, lon = 0;
    if (ParseCoord(IniRead(L"latitude", L""), lat) && ParseCoord(IniRead(L"longitude", L""), lon) &&
        std::abs(lat) <= 90 && std::abs(lon) <= 180)
    {
        s.location.lat = lat;
        s.location.lon = lon;
    }
    s.temp = _wcsicmp(IniRead(L"temperature_unit", L"C").c_str(), L"F") == 0 ? TempUnit::Fahrenheit : TempUnit::Celsius;
    const std::wstring wind = IniRead(L"wind_unit", L"kmh");
    s.wind = _wcsicmp(wind.c_str(), L"ms") == 0    ? WindUnit::Ms
           : _wcsicmp(wind.c_str(), L"mph") == 0 ? WindUnit::Mph
                                                  : WindUnit::Kmh;
    s.pressure = _wcsicmp(IniRead(L"pressure_unit", L"hpa").c_str(), L"mmhg") == 0 ? PressureUnit::MmHg : PressureUnit::HPa;
    const std::wstring condition = IniRead(L"condition_style", L"color");
    s.condition = _wcsicmp(condition.c_str(), L"mono") == 0   ? ConditionStyle::MonoIcon
                : _wcsicmp(condition.c_str(), L"text") == 0 ? ConditionStyle::Text
                                                             : ConditionStyle::ColorIcon;
    s.intervalMin = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"config", L"interval_min", kDefaultIntervalMin,
                                                                       g_iniPath.c_str())),
                               5, 24 * 60);
    return s;
}

void WriteSettingsFile(const Settings& s)
{
    if (g_iniPath.empty())
        return;
    // WritePrivateProfileStringW writes Unicode only if the file already exists as UTF-16 with a BOM;
    // otherwise the city name goes through the ANSI code page and may get mangled. Create it ourselves.
    if (GetFileAttributesW(g_iniPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        HANDLE f = CreateFileW(g_iniPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
            const BYTE bom[] = { 0xFF, 0xFE };
            DWORD written = 0;
            WriteFile(f, bom, sizeof(bom), &written, nullptr);
            CloseHandle(f);
        }
    }
    auto write = [](const wchar_t* key, const std::wstring& value) {
        WritePrivateProfileStringW(L"config", key, value.c_str(), g_iniPath.c_str());
    };
    write(L"city", s.location.name);
    write(L"latitude", s.location.Valid() ? FormatFixed(s.location.lat, 5) : L"");
    write(L"longitude", s.location.Valid() ? FormatFixed(s.location.lon, 5) : L"");
    write(L"temperature_unit", s.temp == TempUnit::Fahrenheit ? L"F" : L"C");
    write(L"wind_unit", s.wind == WindUnit::Ms ? L"ms" : s.wind == WindUnit::Mph ? L"mph" : L"kmh");
    write(L"pressure_unit", s.pressure == PressureUnit::MmHg ? L"mmhg" : L"hpa");
    write(L"condition_style", s.condition == ConditionStyle::MonoIcon ? L"mono"
                              : s.condition == ConditionStyle::Text   ? L"text"
                                                                      : L"color");
    write(L"interval_min", std::to_wstring(s.intervalMin));
}

// ---------------------------------------------------------------------------------------------
// Weather. Internally always metric (°C, m/s, hPa): a units change in the settings takes effect
// immediately, without another request.

struct Day
{
    double tmin = kNaN, tmax = kNaN;
    double precipProb = kNaN;   // %
    double precipSum = kNaN;    // mm
    int code = -1;              // WMO weather code
    std::wstring sunrise, sunset;
};

struct Weather
{
    double temp = kNaN, feels = kNaN;
    double humidity = kNaN, cloud = kNaN;   // %
    double pressure = kNaN;                 // hPa, reduced to sea level
    double wind = kNaN, gusts = kNaN;       // m/s
    double windDir = kNaN;                  // degrees, where the wind blows from
    double precip = kNaN;                   // mm over the last 15 minutes
    int code = -1;
    bool isDay = true;                      // picks the sun or moon icon
    Day today, tomorrow;
};

std::mutex g_mutex;               // guards everything below
Settings g_settings;
unsigned g_locationVersion = 0;   // bumped on a city change: a response for the old city is dropped
Weather g_weather;
long long g_updatedAt = 0;        // time of the last successful response, UTC ticks
std::wstring g_error = L"loading…";

double Num(const json& obj, const char* key)
{
    const auto it = obj.find(key);
    return it != obj.end() && it->is_number() ? it->get<double>() : kNaN;
}

double NumAt(const json& obj, const char* key, size_t i)
{
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array() || i >= it->size() || !(*it)[i].is_number())
        return kNaN;
    return (*it)[i].get<double>();
}

std::wstring Str(const json& obj, const char* key)
{
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? Utf8ToWide(it->get<std::string>()) : L"";
}

// "2026-09-25T06:20" -> clock time in the regional format
std::wstring TimeAt(const json& obj, const char* key, size_t i)
{
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array() || i >= it->size() || !(*it)[i].is_string())
        return {};
    WORD hour = 0, minute = 0;
    const std::string s = (*it)[i].get<std::string>();
    const size_t t = s.find('T');
    if (t == std::string::npos || sscanf_s(s.c_str() + t + 1, "%hu:%hu", &hour, &minute) != 2)
        return {};
    return FormatClock(hour, minute);
}

int Code(double v)
{
    return std::isnan(v) ? -1 : static_cast<int>(v);
}

Day ReadDay(const json& daily, size_t i)
{
    Day d;
    d.tmin = NumAt(daily, "temperature_2m_min", i);
    d.tmax = NumAt(daily, "temperature_2m_max", i);
    d.precipProb = NumAt(daily, "precipitation_probability_max", i);
    d.precipSum = NumAt(daily, "precipitation_sum", i);
    d.code = Code(NumAt(daily, "weather_code", i));
    d.sunrise = TimeAt(daily, "sunrise", i);
    d.sunset = TimeAt(daily, "sunset", i);
    return d;
}

Weather ParseForecast(const json& j)
{
    Weather w;
    const json& c = j.at("current");
    w.temp = Num(c, "temperature_2m");
    w.feels = Num(c, "apparent_temperature");
    w.humidity = Num(c, "relative_humidity_2m");
    w.cloud = Num(c, "cloud_cover");
    w.pressure = Num(c, "pressure_msl");
    w.wind = Num(c, "wind_speed_10m");
    w.gusts = Num(c, "wind_gusts_10m");
    w.windDir = Num(c, "wind_direction_10m");
    w.precip = Num(c, "precipitation");
    w.code = Code(Num(c, "weather_code"));
    w.isDay = Num(c, "is_day") != 0;   // NaN (field missing) counts as day
    if (const auto daily = j.find("daily"); daily != j.end() && daily->is_object())
    {
        w.today = ReadDay(*daily, 0);
        w.tomorrow = ReadDay(*daily, 1);
    }
    return w;
}

// ---------------------------------------------------------------------------------------------
// WMO weather codes: short text for the cell (up to 13 characters), full text for the tooltip,
// and emoji icons for day and night (drawn with the Segoe UI Emoji color font).

constexpr const wchar_t* kSun = L"☀️";                  // ☀️
constexpr const wchar_t* kMoon = L"\U0001F319";                   // 🌙
constexpr const wchar_t* kSunSmallCloud = L"\U0001F324️";    // 🌤️
constexpr const wchar_t* kSunCloud = L"⛅";                   // ⛅
constexpr const wchar_t* kCloud = L"☁️";                // ☁️
constexpr const wchar_t* kFog = L"\U0001F32B️";              // 🌫️
constexpr const wchar_t* kSunRain = L"\U0001F326️";          // 🌦️
constexpr const wchar_t* kRain = L"\U0001F327️";             // 🌧️
constexpr const wchar_t* kSnow = L"\U0001F328️";             // 🌨️
constexpr const wchar_t* kStorm = L"⛈️";                // ⛈️

struct WmoText
{
    int code;
    const wchar_t* cell;
    const wchar_t* full;
    const wchar_t* dayIcon;
    const wchar_t* nightIcon;
};
constexpr WmoText kWmo[] = {
    { 0, L"Clear", L"clear sky", kSun, kMoon },
    { 1, L"Mostly clear", L"mainly clear", kSunSmallCloud, kMoon },
    { 2, L"Partly cloudy", L"partly cloudy", kSunCloud, kCloud },
    { 3, L"Overcast", L"overcast", kCloud, kCloud },
    { 45, L"Fog", L"fog", kFog, kFog },
    { 48, L"Rime fog", L"depositing rime fog", kFog, kFog },
    { 51, L"Drizzle", L"light drizzle", kSunRain, kRain },
    { 53, L"Drizzle", L"moderate drizzle", kSunRain, kRain },
    { 55, L"Drizzle", L"dense drizzle", kRain, kRain },
    { 56, L"Frz. drizzle", L"light freezing drizzle", kRain, kRain },
    { 57, L"Frz. drizzle", L"dense freezing drizzle", kRain, kRain },
    { 61, L"Light rain", L"slight rain", kRain, kRain },
    { 63, L"Rain", L"moderate rain", kRain, kRain },
    { 65, L"Heavy rain", L"heavy rain", kRain, kRain },
    { 66, L"Frz. rain", L"light freezing rain", kRain, kRain },
    { 67, L"Frz. rain", L"heavy freezing rain", kRain, kRain },
    { 71, L"Light snow", L"slight snowfall", kSnow, kSnow },
    { 73, L"Snow", L"moderate snowfall", kSnow, kSnow },
    { 75, L"Heavy snow", L"heavy snowfall", kSnow, kSnow },
    { 77, L"Snow grains", L"snow grains", kSnow, kSnow },
    { 80, L"Showers", L"slight rain showers", kSunRain, kRain },
    { 81, L"Showers", L"moderate rain showers", kSunRain, kRain },
    { 82, L"Hvy showers", L"violent rain showers", kRain, kRain },
    { 85, L"Snow showers", L"slight snow showers", kSnow, kSnow },
    { 86, L"Snow showers", L"heavy snow showers", kSnow, kSnow },
    { 95, L"T-storm", L"thunderstorm", kStorm, kStorm },
    { 96, L"T-storm, hail", L"thunderstorm with slight hail", kStorm, kStorm },
    { 99, L"T-storm, hail", L"thunderstorm with heavy hail", kStorm, kStorm },
};

const WmoText* FindWmo(int code)
{
    for (const WmoText& w : kWmo)
        if (w.code == code)
            return &w;
    return nullptr;
}

std::wstring CellCondition(int code)
{
    const WmoText* w = FindWmo(code);
    return w ? w->cell : L"—";
}

std::wstring FullCondition(int code)
{
    const WmoText* w = FindWmo(code);
    return w ? w->full : L"";
}

// nullptr for an unknown code
const wchar_t* ConditionIcon(int code, bool isDay)
{
    const WmoText* w = FindWmo(code);
    return w ? (isDay ? w->dayIcon : w->nightIcon) : nullptr;
}

// ---------------------------------------------------------------------------------------------
// Formatting with the selected units

long TempValue(double c, TempUnit u)
{
    return std::lround(u == TempUnit::Fahrenheit ? c * 9 / 5 + 32 : c);
}

std::wstring FormatTemp(double c, TempUnit u)
{
    if (std::isnan(c))
        return L"—";
    return std::to_wstring(TempValue(c, u)) + (u == TempUnit::Fahrenheit ? L"°F" : L"°C");
}

// "8…17°C"
std::wstring FormatRange(double lo, double hi, TempUnit u)
{
    if (std::isnan(lo) || std::isnan(hi))
        return L"—";
    return std::to_wstring(TempValue(lo, u)) + L"…" + FormatTemp(hi, u);
}

std::wstring WindUnitText(WindUnit u)
{
    switch (u)
    {
    case WindUnit::Ms: return L"m/s";
    case WindUnit::Mph: return L"mph";
    default: return L"km/h";
    }
}

long WindValue(double ms, WindUnit u)
{
    switch (u)
    {
    case WindUnit::Ms: return std::lround(ms);
    case WindUnit::Mph: return std::lround(ms * 2.236936);
    default: return std::lround(ms * 3.6);
    }
}

// Where the wind blows from: N, NE, E, …
std::wstring Compass(double deg)
{
    static const wchar_t* const kNames[] = { L"N", L"NE", L"E", L"SE", L"S", L"SW", L"W", L"NW" };
    if (std::isnan(deg))
        return {};
    return kNames[std::lround(std::fmod(deg + 360.0, 360.0) / 45.0) % 8];
}

// "14 km/h SE" or "calm"
std::wstring FormatWind(double ms, double deg, WindUnit u)
{
    if (std::isnan(ms))
        return L"—";
    const long v = WindValue(ms, u);
    if (v == 0)
        return L"calm";
    std::wstring s = std::to_wstring(v) + L" " + WindUnitText(u);
    const std::wstring dir = Compass(deg);
    if (!dir.empty())
        s += L" " + dir;
    return s;
}

std::wstring FormatPressure(double hpa, PressureUnit u)
{
    if (std::isnan(hpa))
        return L"—";
    if (u == PressureUnit::MmHg)
        return std::to_wstring(std::lround(hpa * 0.750062)) + L" mmHg";
    return std::to_wstring(std::lround(hpa)) + L" hPa";
}

std::wstring Percent(double v)
{
    return std::isnan(v) ? L"—" : std::to_wstring(std::lround(v)) + L"%";
}

std::wstring Millimeters(double v)
{
    return FormatFixed(v, 1) + L" mm";
}

// ---------------------------------------------------------------------------------------------
// Network

// GET https://host/path. Returns the HTTP status, 0 on a network error.
DWORD HttpGet(const wchar_t* host, const std::wstring& path, std::string& body)
{
    DWORD status = 0, size = sizeof(status);
    HINTERNET session = WinHttpOpen(L"TrafficMonitor-OpenMeteo/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session)
        WinHttpSetTimeouts(session, 10000, 10000, 10000, 15000);
    HINTERNET connect = session ? WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    if (request &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr))
    {
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        char buf[8192];
        DWORD read = 0;
        while (WinHttpReadData(request, buf, sizeof(buf), &read) && read > 0)
            body.append(buf, read);
    }
    for (HINTERNET h : { request, connect, session })
        if (h)
            WinHttpCloseHandle(h);
    return status;
}

enum class FetchResult { Ok, NoLocation, Retry, Throttled };

// Runs on a background thread.
FetchResult FetchWeather()
{
    Location location;
    unsigned version = 0;
    {
        std::lock_guard lock(g_mutex);
        location = g_settings.location;
        version = g_locationVersion;
        if (!location.Valid())
        {
            g_error = L"no city";
            return FetchResult::NoLocation;
        }
    }

    const std::wstring path =
        L"/v1/forecast?latitude=" + FormatFixed(location.lat, 5) + L"&longitude=" + FormatFixed(location.lon, 5) +
        L"&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,precipitation,weather_code,"
        L"cloud_cover,pressure_msl,wind_speed_10m,wind_direction_10m,wind_gusts_10m"
        L"&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,precipitation_sum,"
        L"sunrise,sunset"
        L"&timezone=auto&forecast_days=2&wind_speed_unit=ms";
    std::string body;
    const DWORD status = HttpGet(L"api.open-meteo.com", path, body);

    Weather weather;
    std::wstring error;
    if (status == 0)
        error = L"offline";
    else if (status != 200)
        error = L"HTTP " + std::to_wstring(status);
    else
    {
        try
        {
            weather = ParseForecast(json::parse(body));
        }
        catch (const std::exception&)
        {
            error = L"bad response";
        }
    }

    std::lock_guard lock(g_mutex);
    if (version != g_locationVersion)
        return FetchResult::Ok;   // the city changed during the request; a new one is already scheduled
    g_error = error;
    if (error.empty())
    {
        g_weather = weather;
        g_updatedAt = NowTicks();
        return FetchResult::Ok;
    }
    return status == 429 || status >= 500 ? FetchResult::Throttled : FetchResult::Retry;
}

struct GeoResult
{
    std::vector<Location> places;
    std::wstring error;
};

// "Springfield, Illinois, United States"; the region is added when it differs from the name.
std::wstring PlaceName(const json& p)
{
    std::wstring name = Str(p, "name");
    for (const char* key : { "admin1", "country" })
    {
        const std::wstring part = Str(p, key);
        if (!part.empty() && part != name)
            name += L", " + part;
    }
    return name;
}

GeoResult SearchCity(const std::wstring& query)
{
    GeoResult r;
    std::string body;
    const DWORD status = HttpGet(L"geocoding-api.open-meteo.com",
                                 L"/v1/search?count=10&language=en&format=json&name=" + UrlEncode(query), body);
    if (status == 0)
        r.error = L"No network connection.";
    else if (status != 200)
        r.error = L"Server error: HTTP " + std::to_wstring(status) + L".";
    else
    {
        try
        {
            const json j = json::parse(body);
            if (const auto results = j.find("results"); results != j.end() && results->is_array())
                for (const json& p : *results)
                {
                    Location loc;
                    loc.lat = Num(p, "latitude");
                    loc.lon = Num(p, "longitude");
                    loc.name = PlaceName(p);
                    if (loc.Valid() && !loc.name.empty())
                        r.places.push_back(std::move(loc));
                }
            if (r.places.empty())
                r.error = L"Nothing found.";
        }
        catch (const std::exception&)
        {
            r.error = L"Couldn't parse the server response.";
        }
    }
    return r;
}

// ---------------------------------------------------------------------------------------------
// The functions below read g_* — call them under g_mutex.

bool HasFreshData()
{
    const long long staleAfter = std::max(45, 3 * g_settings.intervalMin) * kTicksPerMinute;
    return g_updatedAt != 0 && NowTicks() - g_updatedAt < staleAfter;
}

std::wstring DayLine(const wchar_t* title, const Day& d, const Settings& s)
{
    if (std::isnan(d.tmin))
        return {};
    std::wstring line = L"\r\n" + std::wstring(title) + L": " + FormatRange(d.tmin, d.tmax, s.temp);
    const std::wstring cond = FullCondition(d.code);
    if (!cond.empty())
        line += L", " + cond;
    if (!std::isnan(d.precipProb))
    {
        line += L", " + Percent(d.precipProb) + L" chance of precipitation";
        if (!std::isnan(d.precipSum) && d.precipSum >= 0.05)
            line += L" (" + Millimeters(d.precipSum) + L")";
    }
    return line;
}

std::wstring TooltipText()
{
    const Settings& s = g_settings;
    if (!s.location.Valid())
        return L"Open-Meteo: no city selected.\r\nChoose one in Plug-in Manage → Open-Meteo → Options…";

    std::wstring tip = s.location.name.empty() ? L"Weather" : s.location.name;
    if (HasFreshData())
    {
        const Weather& w = g_weather;
        const std::wstring cond = FullCondition(w.code);
        if (!cond.empty())
            tip += L" — " + cond;
        tip += L"\r\nTemperature " + FormatTemp(w.temp, s.temp) + L", feels like " + FormatTemp(w.feels, s.temp);
        tip += L"\r\nWind " + FormatWind(w.wind, w.windDir, s.wind);
        if (!std::isnan(w.gusts) && WindValue(w.gusts, s.wind) > WindValue(w.wind, s.wind))
            tip += L", gusts up to " + std::to_wstring(WindValue(w.gusts, s.wind)) + L" " + WindUnitText(s.wind);
        tip += L"\r\nHumidity " + Percent(w.humidity) + L", cloud cover " + Percent(w.cloud) + L", pressure " +
               FormatPressure(w.pressure, s.pressure);
        if (!std::isnan(w.precip) && w.precip >= 0.05)
            tip += L"\r\nPrecipitation now: " + Millimeters(w.precip);
        tip += DayLine(L"Today", w.today, s);
        tip += DayLine(L"Tomorrow", w.tomorrow, s);
        if (!w.today.sunrise.empty() && !w.today.sunset.empty())
            tip += L"\r\nSunrise " + w.today.sunrise + L", sunset " + w.today.sunset + L" (local time)";
        tip += L"\r\nUpdated at " + FormatLocalTime(g_updatedAt) + L" · data by Open-Meteo.com";
    }
    if (!g_error.empty())
        tip += L"\r\nError: " + g_error;
    return tip;
}

// ---------------------------------------------------------------------------------------------
// Weather icons. GDI can't draw color fonts, so the emoji goes through a Direct2D DC render target.
// Where the target DC is a 32-bit DIB (TrafficMonitor's Direct2D taskbar on Windows 11 hands out
// such a DC in ExecuteGdiOperation), the icon is drawn with premultiplied alpha: TrafficMonitor keeps
// those pixels as they are, so the edges stay smooth over a transparent taskbar. Any other DC gets
// an ignore-alpha target. Used only on the UI thread.

class CIconRenderer
{
public:
    void Draw(HDC hdc, const RECT& box, const wchar_t* emoji, float emSize, bool color, COLORREF mono)
    {
        const bool alpha = IsDib32(hdc);
        ComPtr<ID2D1DCRenderTarget>& target = m_targets[alpha ? 1 : 0];
        if (!target && !CreateTarget(alpha, target))
            return;
        ComPtr<IDWriteTextFormat> format = Format(emSize, color);
        ComPtr<ID2D1SolidColorBrush> brush;
        if (!format || FAILED(target->BindDC(hdc, &box)) ||
            FAILED(target->CreateSolidColorBrush(
                D2D1::ColorF(GetRValue(mono) / 255.0f, GetGValue(mono) / 255.0f, GetBValue(mono) / 255.0f), &brush)))
            return;
        target->BeginDraw();
        target->DrawText(emoji, static_cast<UINT32>(wcslen(emoji)), format.Get(),
                         D2D1::RectF(0, 0, static_cast<float>(box.right - box.left), static_cast<float>(box.bottom - box.top)),
                         brush.Get(), color ? D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT : D2D1_DRAW_TEXT_OPTIONS_NONE);
        if (target->EndDraw() == D2DERR_RECREATE_TARGET)
            target.Reset();
    }

private:
    static bool IsDib32(HDC hdc)
    {
        DIBSECTION ds{};
        HGDIOBJ bitmap = GetCurrentObject(hdc, OBJ_BITMAP);
        return bitmap && GetObjectW(bitmap, sizeof(ds), &ds) == sizeof(ds) && ds.dsBm.bmBitsPixel == 32;
    }

    bool CreateTarget(bool alpha, ComPtr<ID2D1DCRenderTarget>& target)
    {
        if (!m_d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2d.GetAddressOf())))
            return false;
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, alpha ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE),
            96.0f, 96.0f);   // 1 DIP = 1 pixel: sizes come from the DC's font, already scaled for DPI
        if (FAILED(m_d2d->CreateDCRenderTarget(&props, &target)))
            return false;
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        return true;
    }

    // Color icons come from Segoe UI Emoji. For monochrome ones Segoe UI Symbol has cleaner glyphs:
    // the plain glyphs in Segoe UI Emoji mix outlines with filled shapes.
    ComPtr<IDWriteTextFormat> Format(float emSize, bool color)
    {
        if (!m_dwrite && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                    reinterpret_cast<IUnknown**>(m_dwrite.GetAddressOf()))))
            return nullptr;
        ComPtr<IDWriteTextFormat>& format = m_formats[color ? 1 : 0];
        float& size = m_formatSizes[color ? 1 : 0];
        if (!format || size != emSize)
        {
            format.Reset();
            if (FAILED(m_dwrite->CreateTextFormat(color ? L"Segoe UI Emoji" : L"Segoe UI Symbol", nullptr,
                                                  DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                  DWRITE_FONT_STRETCH_NORMAL, emSize, L"en-us", &format)))
                return nullptr;
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            size = emSize;
        }
        return format;
    }

    ComPtr<ID2D1Factory> m_d2d;
    ComPtr<IDWriteFactory> m_dwrite;
    ComPtr<ID2D1DCRenderTarget> m_targets[2];   // [0] ignore alpha, [1] premultiplied alpha
    ComPtr<IDWriteTextFormat> m_formats[2];     // [0] monochrome, [1] color
    float m_formatSizes[2] = {};
};

CIconRenderer g_icons;
COLORREF g_valueColor = RGB(255, 255, 255);   // value text color TrafficMonitor passes before DrawItem(); UI thread

// ---------------------------------------------------------------------------------------------
// Cells. Each one can be turned on separately in TrafficMonitor's display settings.

// Cells that can show the sky as an icon (unless the Condition setting is Text).
enum class IconKind { None, WithTemp, Only };

struct ItemDef
{
    const wchar_t* id;
    const wchar_t* name;
    const wchar_t* label;
    const wchar_t* sample;   // TrafficMonitor sizes the cell from this text
    std::wstring (*format)(const Weather&, const Settings&);
    double (*graph)(const Weather&);   // 0…1 for the taskbar bar; nullptr = no bar
    IconKind icon = IconKind::None;
};

const ItemDef kItems[] = {
    { L"OpenMeteoWeather", L"Open-Meteo: weather", L"Weather: ", L"-25°C Partly cloudy",
      [](const Weather& w, const Settings& s) { return FormatTemp(w.temp, s.temp) + L" " + CellCondition(w.code); },
      nullptr, IconKind::WithTemp },
    { L"OpenMeteoTemperature", L"Open-Meteo: temperature", L"Temp: ", L"-25°C",
      [](const Weather& w, const Settings& s) { return FormatTemp(w.temp, s.temp); }, nullptr },
    { L"OpenMeteoFeelsLike", L"Open-Meteo: feels like", L"Feels: ", L"-25°C",
      [](const Weather& w, const Settings& s) { return FormatTemp(w.feels, s.temp); }, nullptr },
    { L"OpenMeteoCondition", L"Open-Meteo: condition", L"Now: ", L"Partly cloudy",
      [](const Weather& w, const Settings&) { return CellCondition(w.code); }, nullptr, IconKind::Only },
    { L"OpenMeteoWind", L"Open-Meteo: wind", L"Wind: ", L"100 km/h NW",
      [](const Weather& w, const Settings& s) { return FormatWind(w.wind, w.windDir, s.wind); }, nullptr },
    { L"OpenMeteoHumidity", L"Open-Meteo: humidity", L"Humidity: ", L"100%",
      [](const Weather& w, const Settings&) { return Percent(w.humidity); },
      [](const Weather& w) { return w.humidity / 100; } },
    { L"OpenMeteoPressure", L"Open-Meteo: pressure", L"Pressure: ", L"1013 hPa",
      [](const Weather& w, const Settings& s) { return FormatPressure(w.pressure, s.pressure); }, nullptr },
    { L"OpenMeteoToday", L"Open-Meteo: today's low/high", L"Today: ", L"-25…-15°C",
      [](const Weather& w, const Settings& s) { return FormatRange(w.today.tmin, w.today.tmax, s.temp); }, nullptr },
    { L"OpenMeteoPrecipitation", L"Open-Meteo: chance of precipitation today", L"Precip: ", L"100%",
      [](const Weather& w, const Settings&) { return Percent(w.today.precipProb); },
      [](const Weather& w) { return w.today.precipProb / 100; } },
};

// TrafficMonitor calls DataRequired() from its monitoring thread, while cell text and the tooltip
// are requested from the UI thread. So the shared state lives in g_* behind a mutex, and the
// strings whose pointers we return are written only by the thread that reads them.
class CWeatherItem : public IPluginItem
{
public:
    explicit CWeatherItem(const ItemDef& def) : m_def(&def) {}

    const wchar_t* GetItemName() const override { return m_def->name; }
    const wchar_t* GetItemId() const override { return m_def->id; }
    const wchar_t* GetItemLableText() const override { return m_def->label; }
    const wchar_t* GetItemValueSampleText() const override { return m_def->sample; }

    const wchar_t* GetItemValueText() const override
    {
        std::lock_guard lock(g_mutex);
        if (HasFreshData())
            m_text = m_def->format(g_weather, g_settings);
        else
            m_text = g_error.empty() ? L"—" : g_error;
        return m_text.c_str();
    }

    int IsDrawResourceUsageGraph() const override { return m_def->graph ? 1 : 0; }

    float GetResourceUsageGraphValue() const override
    {
        if (!m_def->graph)
            return 0.0f;
        std::lock_guard lock(g_mutex);
        const double v = HasFreshData() ? m_def->graph(g_weather) : kNaN;
        return std::isnan(v) ? 0.0f : static_cast<float>(std::clamp(v, 0.0, 1.0));
    }

    // In icon mode the cell is drawn by the plugin: [icon] 21°C or just [icon]. The icon takes the
    // place of the label. TrafficMonitor re-measures cells every second, so switching between icon
    // and text in the settings takes effect without a restart.
    bool IsCustomDraw() const override
    {
        if (m_def->icon == IconKind::None)
            return false;
        std::lock_guard lock(g_mutex);
        return g_settings.condition != ConditionStyle::Text;
    }

    // hDC has TrafficMonitor's font selected.
    int GetItemWidthEx(void* hDC) const override
    {
        const HDC dc = static_cast<HDC>(hDC);
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        if (m_def->icon == IconKind::Only)
            return IconWidth(tm);
        SIZE text{};
        GetTextExtentPoint32W(dc, L"-25°C", 5, &text);
        return IconWidth(tm) + Gap(tm) + text.cx;
    }

    void DrawItem(void* hDC, int x, int y, int w, int h, bool /*dark_mode*/) override
    {
        const wchar_t* icon = nullptr;
        bool color = true;
        std::wstring text;
        {
            std::lock_guard lock(g_mutex);
            if (HasFreshData())
            {
                icon = ConditionIcon(g_weather.code, g_weather.isDay);
                color = g_settings.condition == ConditionStyle::ColorIcon;
                if (m_def->icon == IconKind::WithTemp)
                    text = FormatTemp(g_weather.temp, g_settings.temp);
                else if (!icon)
                    text = CellCondition(g_weather.code);
            }
            else
            {
                text = g_error.empty() ? L"—" : g_error;
            }
        }

        const HDC dc = static_cast<HDC>(hDC);
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        RECT rc{ x, y, x + w, y + h };
        if (icon)
        {
            RECT box = rc;
            if (m_def->icon == IconKind::Only)
                box.left = x + std::max(0, (w - IconWidth(tm)) / 2);
            box.right = box.left + IconWidth(tm);
            g_icons.Draw(dc, box, icon, static_cast<float>(tm.tmHeight), color, g_valueColor);
            rc.left = box.right + Gap(tm);
        }
        if (!text.empty())
        {
            // In Direct2D taskbar mode TrafficMonitor intercepts DrawTextW and renders the text itself.
            const COLORREF oldColor = SetTextColor(dc, g_valueColor);
            const int oldMode = SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, text.c_str(), -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
            SetBkMode(dc, oldMode);
            SetTextColor(dc, oldColor);
        }
    }

private:
    // The emoji is drawn at the text height; its glyph is a bit wider than tall.
    static int IconWidth(const TEXTMETRICW& tm) { return tm.tmHeight * 115 / 100; }
    static int Gap(const TEXTMETRICW& tm) { return std::max(2, static_cast<int>(tm.tmAveCharWidth / 2)); }

    const ItemDef* m_def;
    mutable std::wstring m_text;
};

// ---------------------------------------------------------------------------------------------
// The Options… dialog

struct OptionsState
{
    Settings settings;               // the copy being edited
    std::vector<Location> results;   // results of the last search
    unsigned searchId = 0;           // responses to outdated searches are ignored
};

struct SearchDone
{
    unsigned id;
    GeoResult result;
};

std::wstring PlaceLine(const Location& loc)
{
    return loc.name + L" (" + FormatFixed(loc.lat, 2) + L", " + FormatFixed(loc.lon, 2) + L")";
}

void ShowSelected(HWND dlg, const Location& loc)
{
    const std::wstring text = loc.Valid() ? L"Selected: " + PlaceLine(loc) : L"No city selected.";
    SetDlgItemTextW(dlg, IDC_SELECTED, text.c_str());
}

void InitCombo(HWND dlg, int id, std::initializer_list<const wchar_t*> items, int selected)
{
    for (const wchar_t* item : items)
        SendDlgItemMessageW(dlg, id, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    SendDlgItemMessageW(dlg, id, CB_SETCURSEL, selected, 0);
}

int ComboSel(HWND dlg, int id)
{
    return static_cast<int>(SendDlgItemMessageW(dlg, id, CB_GETCURSEL, 0, 0));
}

void StartSearch(HWND dlg, OptionsState& st)
{
    wchar_t buf[256];
    GetDlgItemTextW(dlg, IDC_QUERY, buf, static_cast<int>(std::size(buf)));
    const std::wstring query = Trim(buf);
    if (query.empty())
        return;
    const unsigned id = ++st.searchId;
    st.results.clear();
    SendDlgItemMessageW(dlg, IDC_RESULTS, LB_RESETCONTENT, 0, 0);
    SetDlgItemTextW(dlg, IDC_STATUS, L"Searching…");
    // Request in the background so TrafficMonitor doesn't freeze; the result arrives as a message.
    std::thread([dlg, id, query] {
        auto done = std::make_unique<SearchDone>(SearchDone{ id, SearchCity(query) });
        if (PostMessageW(dlg, WM_APP_SEARCH_DONE, 0, reinterpret_cast<LPARAM>(done.get())))
            done.release();
    }).detach();
}

INT_PTR CALLBACK OptionsDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* st = reinterpret_cast<OptionsState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        SetWindowLongPtrW(dlg, DWLP_USER, lParam);
        st = reinterpret_cast<OptionsState*>(lParam);
        const Settings& s = st->settings;
        InitCombo(dlg, IDC_TEMP_UNIT, { L"°C", L"°F" }, static_cast<int>(s.temp));
        InitCombo(dlg, IDC_WIND_UNIT, { L"m/s", L"km/h", L"mph" }, static_cast<int>(s.wind));
        InitCombo(dlg, IDC_PRESSURE_UNIT, { L"mmHg", L"hPa" }, static_cast<int>(s.pressure));
        InitCombo(dlg, IDC_CONDITION, { L"Color icon", L"Mono icon", L"Text" }, static_cast<int>(s.condition));

        HWND interval = GetDlgItem(dlg, IDC_INTERVAL);
        auto addInterval = [interval, &s](int minutes, const std::wstring& text) {
            const LRESULT i = SendMessageW(interval, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            SendMessageW(interval, CB_SETITEMDATA, i, minutes);
            if (minutes == s.intervalMin)
                SendMessageW(interval, CB_SETCURSEL, i, 0);
        };
        for (const IntervalChoice& c : kIntervalChoices)
            addInterval(c.minutes, c.text);
        if (SendMessageW(interval, CB_GETCURSEL, 0, 0) == CB_ERR)   // a value typed into the ini by hand
            addInterval(s.intervalMin, L"every " + std::to_wstring(s.intervalMin) + L" min");

        // Pre-fill the search box with the current city, without region and country.
        const std::wstring name = s.location.name.substr(0, s.location.name.find(L','));
        SetDlgItemTextW(dlg, IDC_QUERY, name.c_str());
        SetDlgItemTextW(dlg, IDC_STATUS, L"Enter a city name and click Search.");
        ShowSelected(dlg, s.location);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_SEARCH:
            StartSearch(dlg, *st);
            return TRUE;
        case IDC_RESULTS:
            if (HIWORD(wParam) == LBN_SELCHANGE)
            {
                const int i = static_cast<int>(SendDlgItemMessageW(dlg, IDC_RESULTS, LB_GETCURSEL, 0, 0));
                if (i >= 0 && i < static_cast<int>(st->results.size()))
                {
                    st->settings.location = st->results[i];
                    ShowSelected(dlg, st->settings.location);
                }
            }
            return TRUE;
        case IDOK:
            if (GetFocus() == GetDlgItem(dlg, IDC_QUERY))   // Enter in the search box means Search
            {
                StartSearch(dlg, *st);
                return TRUE;
            }
            st->settings.temp = static_cast<TempUnit>(std::max(0, ComboSel(dlg, IDC_TEMP_UNIT)));
            st->settings.wind = static_cast<WindUnit>(std::max(0, ComboSel(dlg, IDC_WIND_UNIT)));
            st->settings.pressure = static_cast<PressureUnit>(std::max(0, ComboSel(dlg, IDC_PRESSURE_UNIT)));
            st->settings.condition = static_cast<ConditionStyle>(std::max(0, ComboSel(dlg, IDC_CONDITION)));
            if (const int i = ComboSel(dlg, IDC_INTERVAL); i != CB_ERR)
                st->settings.intervalMin =
                    static_cast<int>(SendDlgItemMessageW(dlg, IDC_INTERVAL, CB_GETITEMDATA, i, 0));
            EndDialog(dlg, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_APP_SEARCH_DONE:
    {
        std::unique_ptr<SearchDone> done(reinterpret_cast<SearchDone*>(lParam));
        if (done->id != st->searchId)
            return TRUE;
        st->results = std::move(done->result.places);
        for (const Location& loc : st->results)
            SendDlgItemMessageW(dlg, IDC_RESULTS, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(PlaceLine(loc).c_str()));
        const std::wstring status = !done->result.error.empty()
                                        ? done->result.error
                                        : L"Found: " + std::to_wstring(st->results.size()) + L". Pick a city from the list.";
        SetDlgItemTextW(dlg, IDC_STATUS, status.c_str());
        return TRUE;
    }

    case WM_DESTROY:
    {
        MSG m;   // search results that arrived right before the dialog closed
        while (PeekMessageW(&m, dlg, WM_APP_SEARCH_DONE, WM_APP_SEARCH_DONE, PM_REMOVE))
            delete reinterpret_cast<SearchDone*>(m.lParam);
        break;
    }
    }
    return FALSE;
}

// ---------------------------------------------------------------------------------------------
// Plugin

class COpenMeteoPlugin : public ITMPlugin
{
public:
    COpenMeteoPlugin()
    {
        m_items.reserve(std::size(kItems));
        for (const ItemDef& def : kItems)
            m_items.emplace_back(def);
    }

    IPluginItem* GetItem(int index) override
    {
        return index >= 0 && index < static_cast<int>(m_items.size()) ? &m_items[index] : nullptr;
    }

    // Called about once a second. No network here: the request goes to a separate thread.
    void DataRequired() override
    {
        if (m_fetching)
            return;
        const ULONGLONG last = m_lastFetch;
        if (!m_refreshNow && last != 0 && GetTickCount64() - last < WaitAfter(m_lastResult))
            return;
        m_refreshNow = false;
        m_fetching = true;
        std::thread([this] {
            m_lastResult = FetchWeather();
            m_lastFetch = GetTickCount64();
            m_fetching = false;
        }).detach();
    }

    OptionReturn ShowOptionsDialog(void* hParent) override
    {
        OptionsState st;
        {
            std::lock_guard lock(g_mutex);
            st.settings = g_settings;
        }
        if (DialogBoxParamW(reinterpret_cast<HINSTANCE>(&__ImageBase), MAKEINTRESOURCEW(IDD_OPTIONS),
                            static_cast<HWND>(hParent), OptionsDlgProc, reinterpret_cast<LPARAM>(&st)) != IDOK)
            return OR_OPTION_UNCHANGED;

        bool locationChanged = false;
        {
            std::lock_guard lock(g_mutex);
            locationChanged = !SameLocation(st.settings.location, g_settings.location);
            g_settings = st.settings;
            if (locationChanged)
            {
                ++g_locationVersion;
                g_updatedAt = 0;
                g_error = L"loading…";
            }
        }
        WriteSettingsFile(st.settings);
        if (locationChanged)
            m_refreshNow = true;
        return OR_OPTION_CHANGED;
    }

    const wchar_t* GetInfo(PluginInfoIndex index) override
    {
        switch (index)
        {
        case TMI_NAME: return L"Open-Meteo";
        case TMI_DESCRIPTION: return L"Weather from Open-Meteo.com: temperature, wind, humidity, pressure, and a forecast for today and tomorrow.";
        case TMI_AUTHOR: return L"Moonl1ght";
        case TMI_COPYRIGHT: return L"Data: Open-Meteo.com (CC BY 4.0)";
        case TMI_VERSION: return L"1.2";
        case TMI_URL: return L"https://open-meteo.com/";
        default: return L"";
        }
    }

    const wchar_t* GetTooltipInfo() override
    {
        std::lock_guard lock(g_mutex);
        m_tooltip = TooltipText();
        return m_tooltip.c_str();
    }

    // TrafficMonitor passes the config folder right after loading the plugin, before the first DataRequired(),
    // and the value text color right before each DrawItem().
    void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override
    {
        if (data == nullptr || *data == L'\0')
            return;
        if (index == EI_VALUE_TEXT_COLOR)
            g_valueColor = static_cast<COLORREF>(wcstoul(data, nullptr, 10));
        if (index != EI_CONFIG_DIR)
            return;
        g_iniPath = data;
        if (g_iniPath.back() != L'\\' && g_iniPath.back() != L'/')
            g_iniPath += L'\\';
        g_iniPath += L"OpenMeteo.ini";
        const Settings s = ReadSettingsFile();
        std::lock_guard lock(g_mutex);
        g_settings = s;
        ++g_locationVersion;
    }

    // Command in the cell's context menu.
    int GetCommandCount() override { return 1; }
    const wchar_t* GetCommandName(int index) override { return index == 0 ? L"Refresh weather" : nullptr; }
    void OnPluginCommand(int index, void*, void*) override
    {
        if (index == 0)
            m_refreshNow = true;
    }

private:
    static ULONGLONG WaitAfter(FetchResult r)
    {
        ULONGLONG interval = 0;
        {
            std::lock_guard lock(g_mutex);
            interval = static_cast<ULONGLONG>(g_settings.intervalMin) * 60 * 1000;
        }
        switch (r)
        {
        case FetchResult::Retry: return kRetryMs;
        case FetchResult::Throttled: return std::max(kThrottleMs, interval);
        default: return interval;
        }
    }

    std::vector<CWeatherItem> m_items;
    std::atomic<bool> m_fetching{ false };
    std::atomic<bool> m_refreshNow{ false };
    std::atomic<FetchResult> m_lastResult{ FetchResult::Ok };
    std::atomic<ULONGLONG> m_lastFetch{ 0 };   // GetTickCount64() at the end of the last request, 0 = none yet
    std::wstring m_tooltip;
};
} // namespace

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance()
{
    // On exit TrafficMonitor unloads plugins with FreeLibrary while a background request may still
    // be running. Pin the DLL in the process so its code isn't unmapped under that thread.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&TMPluginGetInstance), &self);

    static COpenMeteoPlugin instance;
    return &instance;
}
