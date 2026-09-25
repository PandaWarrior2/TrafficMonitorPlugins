// Loads ClaudeUsage.dll the same way TrafficMonitor does, calls DataRequired() for a few seconds and
// prints the cell text and the tooltip. Lets you check the plugin without restarting TrafficMonitor.
// The plugin stores its settings next to SmokeTest.exe (ClaudeUsage.ini).
//
//     SmokeTest.exe [--options] [path\to\ClaudeUsage.dll]
//
//     --options   open the plugin's settings dialog first, like the Options… button in TrafficMonitor

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#include "PluginInterface.h"

// Same as TrafficMonitor.exe: Common Controls v6, so the plugin's settings dialog looks the same.
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static void Print(const wchar_t* fmt, ...)
{
    wchar_t wbuf[4096];
    va_list args;
    va_start(args, fmt);
    vswprintf_s(wbuf, fmt, args);
    va_end(args);
    char buf[16384];
    const int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, sizeof(buf), nullptr, nullptr);
    if (n > 1)
        fwrite(buf, 1, n - 1, stdout);
}

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    const wchar_t* path = L"ClaudeUsage.dll";
    bool showOptions = false;
    for (int i = 1; i < argc; ++i)
    {
        if (wcscmp(argv[i], L"--options") == 0)
            showOptions = true;
        else
            path = argv[i];
    }

    HMODULE dll = LoadLibraryW(path);
    if (!dll)
    {
        Print(L"LoadLibrary(%s) failed: %lu\n", path, GetLastError());
        return 1;
    }
    using GetInstanceFn = ITMPlugin* (*)();
    auto getInstance = reinterpret_cast<GetInstanceFn>(GetProcAddress(dll, "TMPluginGetInstance"));
    if (!getInstance)
    {
        Print(L"TMPluginGetInstance is not exported\n");
        return 1;
    }
    ITMPlugin* plugin = getInstance();
    Print(L"%s %s (API %d)\n%s\n\n", plugin->GetInfo(ITMPlugin::TMI_NAME), plugin->GetInfo(ITMPlugin::TMI_VERSION),
          plugin->GetAPIVersion(), plugin->GetInfo(ITMPlugin::TMI_DESCRIPTION));

    // Like TrafficMonitor: the config folder is passed right after loading.
    wchar_t configDir[MAX_PATH];
    GetModuleFileNameW(nullptr, configDir, MAX_PATH);
    *(wcsrchr(configDir, L'\\') + 1) = L'\0';
    plugin->OnExtenedInfo(ITMPlugin::EI_CONFIG_DIR, configDir);

    if (showOptions)
    {
        const ITMPlugin::OptionReturn r = plugin->ShowOptionsDialog(nullptr);
        Print(L"ShowOptionsDialog: %s\n\n", r == ITMPlugin::OR_OPTION_CHANGED     ? L"OR_OPTION_CHANGED"
                                            : r == ITMPlugin::OR_OPTION_UNCHANGED ? L"OR_OPTION_UNCHANGED"
                                                                                  : L"OR_OPTION_NOT_PROVIDED");
    }

    // Like TrafficMonitor: DataRequired() once a second; wait until the first cell's text changes.
    IPluginItem* first = plugin->GetItem(0);
    const std::wstring initial = first->GetItemValueText();
    for (int sec = 0; sec < 20; ++sec)
    {
        plugin->DataRequired();
        Sleep(1000);
        if (first->GetItemValueText() != initial)
            break;
    }

    for (int i = 0; IPluginItem* item = plugin->GetItem(i); ++i)
        Print(L"[%s] %s%s   graph=%.2f\n", item->GetItemId(), item->GetItemLableText(), item->GetItemValueText(),
              item->GetResourceUsageGraphValue());
    Print(L"\n--- tooltip ---\n%s\n", plugin->GetTooltipInfo());
    return 0;
}
