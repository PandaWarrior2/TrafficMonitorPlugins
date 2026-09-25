// Loads OpenMeteo.dll the same way TrafficMonitor does, calls DataRequired() for a few seconds and
// prints the cell text and the tooltip. Lets you check the plugin without restarting TrafficMonitor.
// The plugin stores its settings next to SmokeTest.exe (OpenMeteo.ini).
//
//     SmokeTest.exe [--options] [--render out.bmp] [path\to\OpenMeteo.dll]
//
//     --options   open the plugin's settings dialog first, like the Options… button in TrafficMonitor
//     --render    draw the plugin's custom-drawn cells into out.bmp (see RenderCustomDrawnItems)

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>

#include "PluginInterface.h"

// Same as TrafficMonitor.exe: Common Controls v6, so the plugin's settings dialog looks the same.
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static void Print(const wchar_t* fmt, ...)
{
    wchar_t wbuf[8192];
    va_list args;
    va_start(args, fmt);
    vswprintf_s(wbuf, fmt, args);
    va_end(args);
    char buf[32768];
    const int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, sizeof(buf), nullptr, nullptr);
    if (n > 1)
        fwrite(buf, 1, n - 1, stdout);
}

static void SaveBmp(const wchar_t* path, const DWORD* pixels, int width, int height)
{
    BITMAPINFOHEADER bih{ sizeof(bih), width, -height, 1, 32, BI_RGB };
    const DWORD bytes = static_cast<DWORD>(width) * height * 4;
    BITMAPFILEHEADER bfh{ 0x4D42, static_cast<DWORD>(sizeof(bfh) + sizeof(bih) + bytes), 0, 0,
                          static_cast<DWORD>(sizeof(bfh) + sizeof(bih)) };
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f)
        return;
    fwrite(&bfh, sizeof(bfh), 1, f);
    fwrite(&bih, sizeof(bih), 1, f);
    fwrite(pixels, 1, bytes, f);
    fclose(f);
}

// Draws every custom-drawn cell the way TrafficMonitor would, one row per scenario:
//   rows 1–2: GDI taskbar with a 32-bit DIB back buffer (dark and light background);
//   rows 3–4: GDI taskbar with a device-dependent bitmap (CreateCompatibleBitmap);
//   rows 5–6: Direct2D taskbar on Windows 11 (transparent): the surface starts with alpha = 1/255,
//             and afterwards pixels still at 1/255 become transparent and pixels with alpha 0
//             (written by GDI) become opaque — the same rule as TrafficMonitor's
//             PsGdiTexturePostprocessor. TrafficMonitor also hooks DrawTextW in this mode; this test
//             doesn't, so text edges in rows 5–6 look darker than they will in the real taskbar.
static void RenderCustomDrawnItems(ITMPlugin* plugin, const wchar_t* path)
{
    constexpr int kRowHeight = 30, kWidth = 360, kRows = 6;
    struct Row { COLORREF bg, fg; int mode; };   // mode: 0 = DIB, 1 = DDB, 2 = Direct2D simulation
    const Row rows[kRows] = { { RGB(32, 32, 32), RGB(255, 255, 255), 0 }, { RGB(243, 243, 243), RGB(0, 0, 0), 0 },
                              { RGB(32, 32, 32), RGB(255, 255, 255), 1 }, { RGB(243, 243, 243), RGB(0, 0, 0), 1 },
                              { RGB(32, 32, 32), RGB(255, 255, 255), 2 }, { RGB(243, 243, 243), RGB(0, 0, 0), 2 } };

    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), kWidth, -kRowHeight, 1, 32, BI_RGB };
    HDC screen = GetDC(nullptr);
    HDC out = CreateCompatibleDC(screen);
    void* outBits = nullptr;
    BITMAPINFO outInfo = bi;
    outInfo.bmiHeader.biHeight = -kRowHeight * kRows;
    HBITMAP outBitmap = CreateDIBSection(out, &outInfo, DIB_RGB_COLORS, &outBits, nullptr, 0);
    SelectObject(out, outBitmap);
    auto* outPixels = static_cast<DWORD*>(outBits);

    // Segoe UI 9 pt at 150% scaling — roughly what the taskbar uses on a high-DPI screen.
    HFONT font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    for (int r = 0; r < kRows; ++r)
    {
        const Row& row = rows[r];
        HDC dc = CreateCompatibleDC(screen);
        void* bits = nullptr;
        HBITMAP bitmap = row.mode == 1 ? CreateCompatibleBitmap(screen, kWidth, kRowHeight)
                                       : CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        SelectObject(dc, bitmap);
        auto* pixels = static_cast<DWORD*>(bits);
        if (row.mode == 2)
        {
            for (int i = 0; i < kWidth * kRowHeight; ++i)
                pixels[i] = 0x01000000;   // black, alpha = 1/255: "not touched by GDI"
        }
        else
        {
            HBRUSH brush = CreateSolidBrush(row.bg);
            RECT all{ 0, 0, kWidth, kRowHeight };
            FillRect(dc, &all, brush);
            DeleteObject(brush);
        }
        SelectObject(dc, font);
        SetTextColor(dc, row.fg);

        int x = 8;
        for (int i = 0; IPluginItem* item = plugin->GetItem(i); ++i)
        {
            if (!item->IsCustomDraw())
                continue;
            plugin->OnExtenedInfo(ITMPlugin::EI_VALUE_TEXT_COLOR, std::to_wstring(row.fg).c_str());
            const int w = item->GetItemWidthEx(dc);
            item->DrawItem(dc, x, 3, w, kRowHeight - 6, row.bg == RGB(32, 32, 32));
            x += w + 24;
        }
        GdiFlush();

        if (row.mode == 2)
        {
            for (int i = 0; i < kWidth * kRowHeight; ++i)
            {
                DWORD p = pixels[i];
                DWORD a = p >> 24;
                if (a == 1)
                    p = a = 0;          // untouched: transparent
                else if (a == 0)
                    a = 255;            // written by GDI: opaque
                auto blend = [&](int shift, BYTE bg) {
                    return std::min<DWORD>(255, ((p >> shift) & 0xFF) + bg * (255 - a) / 255);
                };
                pixels[i] = 0xFF000000 | (blend(16, GetRValue(row.bg)) << 16) | (blend(8, GetGValue(row.bg)) << 8) |
                            blend(0, GetBValue(row.bg));
            }
        }
        BitBlt(out, 0, r * kRowHeight, kWidth, kRowHeight, dc, 0, 0, SRCCOPY);
        DeleteDC(dc);
        DeleteObject(bitmap);
    }
    GdiFlush();
    SaveBmp(path, outPixels, kWidth, kRowHeight * kRows);
    DeleteObject(font);
    DeleteDC(out);
    DeleteObject(outBitmap);
    ReleaseDC(nullptr, screen);
    Print(L"Rendered custom-drawn cells to %s\n", path);
}

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    const wchar_t* path = L"OpenMeteo.dll";
    const wchar_t* renderPath = nullptr;
    bool showOptions = false;
    for (int i = 1; i < argc; ++i)
    {
        if (wcscmp(argv[i], L"--options") == 0)
            showOptions = true;
        else if (wcscmp(argv[i], L"--render") == 0 && i + 1 < argc)
            renderPath = argv[++i];
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
    {
        Print(L"[%s] %s%s", item->GetItemId(), item->GetItemLableText(), item->GetItemValueText());
        if (item->IsDrawResourceUsageGraph())
            Print(L"   graph=%.2f", item->GetResourceUsageGraphValue());
        Print(L"\n");
    }
    Print(L"\n--- tooltip ---\n%s\n", plugin->GetTooltipInfo());
    if (renderPath)
        RenderCustomDrawnItems(plugin, renderPath);
    return 0;
}
