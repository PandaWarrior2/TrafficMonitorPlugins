// ClaudeUsage — TrafficMonitor plugin: remaining Claude subscription limits (Pro/Max).
//
// The data comes from the same endpoint that powers /usage in Claude Code:
//     GET https://api.anthropic.com/api/oauth/usage
// using the OAuth token from %USERPROFILE%\.claude\.credentials.json. The endpoint is undocumented
// and may change. The plugin only reads the token; Claude Code itself keeps it refreshed.

#include <nlohmann/json.hpp>   // before windows.h; NOMINMAX is set in the project

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "PluginInterface.h"
#include "resource.h"

using json = nlohmann::json;

extern "C" IMAGE_DOS_HEADER __ImageBase;   // HINSTANCE of this DLL, for the dialog resource

namespace
{
constexpr ULONGLONG kBackoffMs = 5 * 60 * 1000;          // after 429 and server errors
constexpr long long kTicksPerSecond = 10'000'000;        // FILETIME: 100 ns
constexpr long long kTicksPerMinute = 60 * kTicksPerSecond;
constexpr long long kUnixEpochTicks = 116'444'736'000'000'000; // 1970-01-01 in FILETIME ticks

// ---------------------------------------------------------------------------------------------
// Settings: polling interval, stored in <plugins folder>\ClaudeUsage.ini

struct IntervalChoice
{
    int minutes;
    const wchar_t* text;
};
constexpr IntervalChoice kIntervalChoices[] = {
    { 1, L"every minute" },      { 2, L"every 2 minutes" },   { 3, L"every 3 minutes" },
    { 5, L"every 5 minutes" },   { 10, L"every 10 minutes" }, { 15, L"every 15 minutes" },
    { 30, L"every 30 minutes" }, { 60, L"every hour" },
};
constexpr int kDefaultIntervalMin = 5;
constexpr int kMaxIntervalMin = 24 * 60;

std::atomic<int> g_intervalMin{ kDefaultIntervalMin };
std::wstring g_iniPath;   // empty until TrafficMonitor passes the config folder

void LoadSettings()
{
    if (g_iniPath.empty())
        return;
    const int minutes = GetPrivateProfileIntW(L"config", L"poll_interval_min", kDefaultIntervalMin, g_iniPath.c_str());
    g_intervalMin = std::clamp(minutes, 1, kMaxIntervalMin);
}

void SaveSettings()
{
    if (!g_iniPath.empty())
        WritePrivateProfileStringW(L"config", L"poll_interval_min", std::to_wstring(g_intervalMin.load()).c_str(),
                                   g_iniPath.c_str());
}

ULONGLONG IntervalMs()
{
    return static_cast<ULONGLONG>(g_intervalMin) * 60 * 1000;
}

// Without a successful response for longer than this, the data is hidden and the cell shows the error.
long long StaleAfterTicks()
{
    return std::max(10LL, 3LL * g_intervalMin) * kTicksPerMinute;
}

// ---------------------------------------------------------------------------------------------
// Time

long long ToTicks(const FILETIME& ft)
{
    return (static_cast<long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

long long NowTicks()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ToTicks(ft);
}

// "2026-09-25T23:09:59.702482+00:00" -> UTC ticks; 0 if it can't be parsed.
long long ParseIso8601(const std::string& s)
{
    SYSTEMTIME st{};
    FILETIME ft{};
    if (sscanf_s(s.c_str(), "%hu-%hu-%huT%hu:%hu:%hu", &st.wYear, &st.wMonth, &st.wDay,
                 &st.wHour, &st.wMinute, &st.wSecond) != 6 ||
        !SystemTimeToFileTime(&st, &ft))
        return 0;

    long long ticks = ToTicks(ft);
    const size_t tz = s.find_first_of("+-", 19);   // offset after the seconds: +HH:MM / -HH:MM
    int hh = 0, mm = 0;
    if (tz != std::string::npos && sscanf_s(s.c_str() + tz + 1, "%d:%d", &hh, &mm) == 2)
    {
        const long long offset = (hh * 60LL + mm) * kTicksPerMinute;
        ticks += s[tz] == '+' ? -offset : offset;
    }
    return ticks;
}

// Duration: "4h 07m" or "5d 02h".
std::wstring FormatDuration(long long ticks)
{
    const long long mins = std::max(0LL, ticks / kTicksPerMinute);
    wchar_t buf[32];
    if (mins >= 24 * 60)
        swprintf_s(buf, L"%lldd %02lldh", mins / (24 * 60), mins % (24 * 60) / 60);
    else
        swprintf_s(buf, L"%lldh %02lldm", mins / 60, mins % 60);
    return buf;
}

// Local time: "Sep 30, 23:59" — the clock part follows the Windows regional format (12/24 h).
std::wstring FormatLocalTime(long long ticks)
{
    static const wchar_t* const kMonths[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                              L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
    const FILETIME ft{ static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32) };
    SYSTEMTIME utc{}, local{};
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
        return L"?";
    wchar_t clock[32];
    if (!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr, clock, 32))
        swprintf_s(clock, L"%02d:%02d", local.wHour, local.wMinute);
    return std::wstring(kMonths[(local.wMonth + 11) % 12]) + L" " + std::to_wstring(local.wDay) + L", " + clock;
}

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Claude Code token

struct Credentials
{
    std::string token;
    long long expiresMs = 0;   // Unix time in ms, 0 = unknown
};

std::wstring CredentialsPath()
{
    wchar_t buf[MAX_PATH];
    std::wstring dir;
    const DWORD n = GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
        dir = buf;
    else if (ExpandEnvironmentStringsW(L"%USERPROFILE%\\.claude", buf, MAX_PATH))
        dir = buf;
    return dir + L"\\.credentials.json";
}

// The file is re-read before every request: Claude Code refreshes the token every few hours.
Credentials ReadCredentials()
{
    Credentials c;
    std::ifstream f(CredentialsPath());
    if (!f)
        return c;
    const json j = json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.contains("claudeAiOauth"))
        return c;
    const json& oauth = j.at("claudeAiOauth");
    if (auto t = oauth.find("accessToken"); t != oauth.end() && t->is_string())
        c.token = t->get<std::string>();
    if (auto e = oauth.find("expiresAt"); e != oauth.end() && e->is_number())
        c.expiresMs = e->get<long long>();
    return c;
}

// ---------------------------------------------------------------------------------------------
// HTTP

// GET /api/oauth/usage. Returns the HTTP status, 0 on a network error.
DWORD RequestUsage(const std::string& token, std::string& body)
{
    DWORD status = 0, size = sizeof(status);
    HINTERNET session = WinHttpOpen(L"TrafficMonitor-ClaudeUsage/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session)
        WinHttpSetTimeouts(session, 10000, 10000, 10000, 15000);
    HINTERNET connect = session ? WinHttpConnect(session, L"api.anthropic.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", L"/api/oauth/usage", nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;

    const std::wstring headers = L"Authorization: Bearer " + std::wstring(token.begin(), token.end()) +
                                 L"\r\nanthropic-beta: oauth-2025-04-20\r\nAccept: application/json";
    if (request &&
        WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
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

// ---------------------------------------------------------------------------------------------
// State shared by the background request and the UI

struct Limit
{
    std::wstring name;
    double used = -1;        // percent used, -1 = no data
    long long resetsAt = 0;  // UTC ticks, 0 = unknown
};

struct UsageState
{
    Limit session{ L"Session (5h)" };
    Limit weekly{ L"Week" };
    Limit fable{ L"Week, Fable" };      // copy of the Fable entry from `scoped`, for its own cell
    std::vector<Limit> scoped;          // weekly limits for individual models
    long long updatedAt = 0;            // time of the last successful response, UTC ticks
    std::wstring error = L"loading…";   // short, for the cell
    std::wstring hint;                  // longer, for the tooltip
};

std::mutex g_mutex;
UsageState g_state;

void ReadWindow(const json& j, const char* key, Limit& limit)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_object())
        return;
    if (auto u = it->find("utilization"); u != it->end() && u->is_number())
        limit.used = u->get<double>();
    if (auto r = it->find("resets_at"); r != it->end() && r->is_string())
        limit.resetsAt = ParseIso8601(r->get<std::string>());
}

void ParseUsage(const json& j, UsageState& out)
{
    ReadWindow(j, "five_hour", out.session);
    ReadWindow(j, "seven_day", out.weekly);

    // limits[] entries with kind == "weekly_scoped" are separate weekly limits, e.g. per model.
    const auto limits = j.find("limits");
    if (limits == j.end() || !limits->is_array())
        return;
    for (const json& l : *limits)
    {
        if (!l.is_object() || l.value("kind", json()) != "weekly_scoped")
            continue;
        Limit scoped{ L"Week" };
        const json scope = l.value("scope", json());
        const json model = scope.is_object() ? scope.value("model", json()) : json();
        const json name = model.is_object() ? model.value("display_name", json()) : json();
        const std::wstring modelName = name.is_string() ? Utf8ToWide(name.get<std::string>()) : L"";
        if (!modelName.empty())
            scoped.name += L", " + modelName;
        if (auto p = l.find("percent"); p != l.end() && p->is_number())
            scoped.used = p->get<double>();
        if (auto r = l.find("resets_at"); r != l.end() && r->is_string())
            scoped.resetsAt = ParseIso8601(r->get<std::string>());
        if (_wcsicmp(modelName.c_str(), L"Fable") == 0)
            out.fable = scoped;
        out.scoped.push_back(std::move(scoped));
    }
}

// Runs on a background thread. Returns true if the server asks us to slow down (429, 5xx).
bool FetchUsage()
{
    const Credentials cred = ReadCredentials();
    const long long nowMs = (NowTicks() - kUnixEpochTicks) / 10'000;

    std::wstring error, hint;
    std::string body;
    DWORD status = 0;
    if (cred.token.empty())
    {
        error = L"not signed in";
        hint = L"No token found in " + CredentialsPath() + L". Sign in to Claude Code: claude /login.";
    }
    else if (cred.expiresMs != 0 && cred.expiresMs <= nowMs)
    {
        error = L"token expired";
        hint = L"Start Claude Code — it will refresh the token, and the plugin will pick it up automatically.";
    }
    else
    {
        status = RequestUsage(cred.token, body);
        if (status == 401 || status == 403)
        {
            error = L"token rejected";
            hint = L"Start Claude Code to refresh the sign-in.";
        }
        else if (status == 0)
            error = L"offline";
        else if (status != 200)
            error = L"HTTP " + std::to_wstring(status);
    }

    UsageState parsed;
    if (error.empty())
    {
        try
        {
            ParseUsage(json::parse(body), parsed);
        }
        catch (const std::exception&)
        {
            error = L"bad response";
            hint = L"The response format has changed — the plugin needs an update.";
        }
    }

    std::lock_guard lock(g_mutex);
    g_state.error = error;
    g_state.hint = hint;
    if (error.empty())
    {
        g_state.session = parsed.session;
        g_state.weekly = parsed.weekly;
        g_state.fable = parsed.fable;
        g_state.scoped = std::move(parsed.scoped);
        g_state.updatedAt = NowTicks();
    }
    return status == 429 || status >= 500;
}

// The functions below read g_state: call them under g_mutex.

bool HasFreshData(const UsageState& s)
{
    return s.updatedAt != 0 && NowTicks() - s.updatedAt < StaleAfterTicks();
}

int RemainingPercent(const Limit& l)
{
    if (l.resetsAt != 0 && l.resetsAt <= NowTicks())
        return 100;   // the window has already reset; a fresh response hasn't arrived yet
    return static_cast<int>(std::clamp(100.0 - l.used, 0.0, 100.0) + 0.5);
}

// Cell text: "98% · 4h 47m" — what's left and the time until reset.
std::wstring CellText(const Limit& l, const UsageState& s)
{
    if (!HasFreshData(s))
        return s.error.empty() ? L"—" : s.error;
    if (l.used < 0)
        return L"—";
    std::wstring text = std::to_wstring(RemainingPercent(l)) + L"%";
    if (l.resetsAt != 0)
        text += L" · " + FormatDuration(l.resetsAt - NowTicks());
    return text;
}

std::wstring TooltipLine(const Limit& l)
{
    std::wstring line = l.name + L": " + std::to_wstring(RemainingPercent(l)) + L"% left";
    if (l.resetsAt != 0)
        line += L", resets in " + FormatDuration(l.resetsAt - NowTicks()) + L" (" + FormatLocalTime(l.resetsAt) + L")";
    return line;
}

std::wstring TooltipText(const UsageState& s)
{
    std::wstring tip = L"Claude — remaining limits";
    if (HasFreshData(s))
    {
        for (const Limit* l : { &s.session, &s.weekly })
            if (l->used >= 0)
                tip += L"\r\n" + TooltipLine(*l);
        for (const Limit& l : s.scoped)
            if (l.used >= 0)
                tip += L"\r\n" + TooltipLine(l);
        tip += L"\r\nUpdated: " + FormatLocalTime(s.updatedAt) + L" (polling every " +
               std::to_wstring(g_intervalMin.load()) + L" min)";
    }
    if (!s.error.empty())
        tip += L"\r\nError: " + s.error;
    if (!s.hint.empty())
        tip += L"\r\n" + s.hint;
    return tip;
}

// ---------------------------------------------------------------------------------------------
// The Options… dialog from Plug-in Manage. lParam points to an int with the interval in minutes.

INT_PTR CALLBACK OptionsDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        SetWindowLongPtrW(dlg, DWLP_USER, lParam);
        const int current = *reinterpret_cast<int*>(lParam);
        HWND combo = GetDlgItem(dlg, IDC_INTERVAL);
        auto add = [combo, current](int minutes, const std::wstring& text) {
            const LRESULT i = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            SendMessageW(combo, CB_SETITEMDATA, i, minutes);
            if (minutes == current)
                SendMessageW(combo, CB_SETCURSEL, i, 0);
        };
        for (const IntervalChoice& c : kIntervalChoices)
            add(c.minutes, c.text);
        if (SendMessageW(combo, CB_GETCURSEL, 0, 0) == CB_ERR)   // a value typed into the ini by hand
            add(current, L"every " + std::to_wstring(current) + L" min");
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
        {
            HWND combo = GetDlgItem(dlg, IDC_INTERVAL);
            const LRESULT i = SendMessageW(combo, CB_GETCURSEL, 0, 0);
            if (i != CB_ERR)
                *reinterpret_cast<int*>(GetWindowLongPtrW(dlg, DWLP_USER)) =
                    static_cast<int>(SendMessageW(combo, CB_GETITEMDATA, i, 0));
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------------------------
// Plugin
//
// TrafficMonitor calls DataRequired() from its monitoring thread, while cell text and the tooltip
// are requested from the UI thread. So the shared state lives in g_state behind a mutex, and the
// strings whose pointers we return are written only by the thread that reads them.

class CUsageItem : public IPluginItem
{
public:
    CUsageItem(const wchar_t* id, const wchar_t* name, const wchar_t* label, Limit UsageState::*limit)
        : m_id(id), m_name(name), m_label(label), m_limit(limit)
    {
    }

    const wchar_t* GetItemName() const override { return m_name; }
    const wchar_t* GetItemId() const override { return m_id; }
    const wchar_t* GetItemLableText() const override { return m_label; }
    const wchar_t* GetItemValueSampleText() const override { return L"100% · 23h 59m"; }

    const wchar_t* GetItemValueText() const override
    {
        std::lock_guard lock(g_mutex);
        m_text = CellText(g_state.*m_limit, g_state);
        return m_text.c_str();
    }

    // The taskbar "resource usage graph" bar shows what's left.
    int IsDrawResourceUsageGraph() const override { return 1; }

    float GetResourceUsageGraphValue() const override
    {
        std::lock_guard lock(g_mutex);
        const Limit& l = g_state.*m_limit;
        return HasFreshData(g_state) && l.used >= 0 ? RemainingPercent(l) / 100.0f : 0.0f;
    }

private:
    const wchar_t* m_id;
    const wchar_t* m_name;
    const wchar_t* m_label;
    Limit UsageState::*m_limit;
    mutable std::wstring m_text;
};

class CClaudeUsagePlugin : public ITMPlugin
{
public:
    IPluginItem* GetItem(int index) override
    {
        switch (index)
        {
        case 0: return &m_session;
        case 1: return &m_weekly;
        case 2: return &m_fable;
        default: return nullptr;
        }
    }

    // Called about once a second. No network here: the request goes to a separate thread.
    // The interval is re-read every time, so a changed setting takes effect immediately.
    void DataRequired() override
    {
        const ULONGLONG last = m_lastFetch;
        const ULONGLONG wait = m_backoff ? std::max(kBackoffMs, IntervalMs()) : IntervalMs();
        if (m_fetching || (last != 0 && GetTickCount64() - last < wait))
            return;
        m_fetching = true;
        std::thread([this] {
            m_backoff = FetchUsage();
            m_lastFetch = GetTickCount64();
            m_fetching = false;
        }).detach();
    }

    OptionReturn ShowOptionsDialog(void* hParent) override
    {
        int minutes = g_intervalMin;
        const INT_PTR result = DialogBoxParamW(reinterpret_cast<HINSTANCE>(&__ImageBase), MAKEINTRESOURCEW(IDD_OPTIONS),
                                               static_cast<HWND>(hParent), OptionsDlgProc,
                                               reinterpret_cast<LPARAM>(&minutes));
        if (result != IDOK || minutes == g_intervalMin)
            return OR_OPTION_UNCHANGED;
        g_intervalMin = minutes;
        SaveSettings();
        return OR_OPTION_CHANGED;
    }

    // TrafficMonitor passes the config folder right after loading the plugin, before the first DataRequired().
    void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override
    {
        if (index != EI_CONFIG_DIR || data == nullptr || *data == L'\0')
            return;
        g_iniPath = data;
        if (g_iniPath.back() != L'\\' && g_iniPath.back() != L'/')
            g_iniPath += L'\\';
        g_iniPath += L"ClaudeUsage.ini";
        LoadSettings();
    }

    const wchar_t* GetInfo(PluginInfoIndex index) override
    {
        switch (index)
        {
        case TMI_NAME: return L"Claude Usage";
        case TMI_DESCRIPTION: return L"Remaining Claude subscription limits: the 5-hour window, the week, and the Fable week.";
        case TMI_AUTHOR: return L"Moonl1ght";
        case TMI_COPYRIGHT: return L"";
        case TMI_VERSION: return L"1.2";
        case TMI_URL: return L"";
        default: return L"";
        }
    }

    const wchar_t* GetTooltipInfo() override
    {
        std::lock_guard lock(g_mutex);
        m_tooltip = TooltipText(g_state);
        return m_tooltip.c_str();
    }

private:
    CUsageItem m_session{ L"ClaudeUsage5h", L"Claude: 5-hour limit", L"Claude 5h: ", &UsageState::session };
    CUsageItem m_weekly{ L"ClaudeUsage7d", L"Claude: weekly limit", L"Claude 7d: ", &UsageState::weekly };
    CUsageItem m_fable{ L"ClaudeUsageFable7d", L"Claude: weekly Fable limit", L"Fable 7d: ", &UsageState::fable };
    std::atomic<bool> m_fetching{ false };
    std::atomic<bool> m_backoff{ false };
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

    static CClaudeUsagePlugin instance;
    return &instance;
}
