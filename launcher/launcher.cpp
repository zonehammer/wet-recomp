// WET Launcher: a small settings window that edits wet.ini (the same file the
// in-game F1 overlay uses) and starts WetRecomp.exe. Plain Win32 controls, no
// dependencies. Keys it doesn't know are kept as they are.
#ifndef UNICODE
#define UNICODE
#endif
#define NOMINMAX
#include "wet_version.h"
#include "keybinds.h"
#include "gpu_info.h"
#include "maintenance.h"
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winhttp.h>
#include <xinput.h>
#include <mmsystem.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    // ---- wet.ini: the lines as they are, values updated in place ----
    struct Ini
    {
        std::vector<std::string> lines;
        std::map<std::string, std::string> values;

        static std::string Trim(const std::string& s)
        {
            size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        }

        void Load(const fs::path& path)
        {
            std::ifstream in(path);
            std::string line;
            while (std::getline(in, line))
            {
                lines.push_back(line);
                std::string t = Trim(line);
                size_t eq = t.find('=');
                if (t.empty() || t[0] == '#' || eq == std::string::npos)
                    continue;
                values[Trim(t.substr(0, eq))] = Trim(t.substr(eq + 1));
            }
        }

        std::string GetString(const std::string& key, const std::string& fallback) const
        {
            auto it = values.find(key);
            return it == values.end() || it->second.empty() ? fallback : it->second;
        }

        int Get(const std::string& key, int fallback) const
        {
            auto it = values.find(key);
            return it == values.end() || it->second.empty() ? fallback : std::atoi(it->second.c_str());
        }

        void Set(const std::string& key, int value) { SetString(key, std::to_string(value)); }

        void SetString(const std::string& key, const std::string& v)
        {
            values[key] = v;
            for (auto& line : lines)
            {
                std::string t = Trim(line);
                size_t eq = t.find('=');
                if (!t.empty() && t[0] != '#' && eq != std::string::npos && Trim(t.substr(0, eq)) == key)
                {
                    line = key + " = " + v;
                    return;
                }
            }
            lines.push_back(key + " = " + v);
        }

        bool Save(const fs::path& path) const
        {
            std::ofstream out(path, std::ios::trunc);
            if (!out)
                return false;
            if (lines.empty())
                out << "# WET Recomp options (F1 in game opens the settings overlay)\n";
            for (auto& line : lines)
                out << line << "\n";
            return bool(out);
        }
    };

    // ---- Controls ----
    enum : int
    {
        kFps = 100, kResolution, kFullscreen, kShowFps,
        kFilter, kSharpness, kSharpnessValue, kAntialiasing, kGrain, kGrainValue, kFov, kFovValue, kWidescreen,
        kMouseAim, kSensitivity, kSensitivityValue, kInvertY, kVibration,
        kShaderCache, kVideoMode, kPopups, kAsyncShaders, kShaderPreload, kMotionBlur, kDepthOfField, kCameraShake, kLanguage,
        kStatus, kPlay, kSaves, kClearShaders, kKeyboard,
        kVsync, kHideBorders, kWideHud, kCheckUpdates, kReportBug, kUpdateText, kPrompts,
        kMonitor, kPadTest, kAchievements,
        kMenuOpenSaves = 500, kMenuBackup, kMenuImportZip, kMenuImportFolder, kMenuClearShaders, kMenuFreeSpace,
        kKeyFirst = 300,                       // one combo per keybinds action
        kKeysOk = kKeyFirst + wet::keybinds::kActionCount, kKeysReset, kKeysCancel,
    };

    HWND s_window = nullptr, s_keysWindow = nullptr;
    HFONT s_font = nullptr;
    double s_scale = 1.0;
    fs::path s_dir, s_iniPath, s_gameDir, s_hddDir;
    Ini s_ini;

    // 0 = match the monitor; a rate from wet.ini that isn't listed gets its own item.
    std::vector<int> s_fpsValues = { 30, 60, 120, 144, 165, 240, 360, 0 };

    int S(int v) { return int(v * s_scale + 0.5); }

    HWND s_parent = nullptr;   // where Make() puts controls (the main window or the keyboard window)

    int s_bannerHeight = 0;
    constexpr int kEffectsRow = 26;    // logical pixels above the main window's controls (0 = no banner)

    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id)
    {
        if (s_parent == s_window)
        {
            // Everything below the Display / Graphics boxes moves down for the
            // effects row at the bottom of those boxes.
            if (y >= 176)
                y += kEffectsRow;
            y += s_bannerHeight;
        }
        HWND h_ = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), s_parent,
                                  reinterpret_cast<HMENU>(intptr_t(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(h_, WM_SETFONT, WPARAM(s_font), TRUE);
        return h_;
    }

    HWND Item(int id) { return GetDlgItem(s_window, id); }
    HWND KeyItem(int id) { return GetDlgItem(s_keysWindow, id); }

    std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

    // ---- Keyboard bindings window ----
    // Each action gets a list of the key names; a binding the list doesn't have
    // (two keys, "Q, F") is shown as an extra item so it survives unchanged.
    void SelectKey(int action, const std::string& value)
    {
        HWND c = KeyItem(kKeyFirst + action);
        SendMessageW(c, CB_RESETCONTENT, 0, 0);
        SendMessageW(c, CB_ADDSTRING, 0, LPARAM(L"NONE"));
        int sel = 0;
        for (int k = 0; k < wet::keybinds::kKeyCount; k++)
        {
            SendMessageW(c, CB_ADDSTRING, 0, LPARAM(Wide(wet::keybinds::kKeys[k].name).c_str()));
            if (value == wet::keybinds::kKeys[k].name)
                sel = k + 1;
        }
        if (sel == 0 && value != "NONE" && !value.empty())
        {
            SendMessageW(c, CB_ADDSTRING, 0, LPARAM(Wide(value).c_str()));
            sel = wet::keybinds::kKeyCount + 1;
        }
        SendMessageW(c, CB_SETCURSEL, WPARAM(sel), 0);
    }

    std::string SelectedKey(int action)
    {
        HWND c = KeyItem(kKeyFirst + action);
        int sel = int(SendMessageW(c, CB_GETCURSEL, 0, 0));
        wchar_t text[64] = {};
        if (sel >= 0)
            SendMessageW(c, CB_GETLBTEXT, WPARAM(sel), LPARAM(text));
        std::string out;
        for (wchar_t* p = text; *p; p++)
            out += char(*p);
        return out.empty() ? "NONE" : out;
    }

    void CloseKeys()
    {
        if (!s_keysWindow)
            return;
        EnableWindow(s_window, TRUE);
        DestroyWindow(s_keysWindow);
        s_keysWindow = nullptr;
        SetForegroundWindow(s_window);
    }

    LRESULT CALLBACK KeysProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_COMMAND:
            switch (LOWORD(wp))
            {
            case kKeysOk:
                for (int i = 0; i < wet::keybinds::kActionCount; i++)
                    s_ini.SetString(std::string("key_") + wet::keybinds::kActions[i].id, SelectedKey(i));
                CloseKeys();
                return 0;
            case kKeysReset:
                for (int i = 0; i < wet::keybinds::kActionCount; i++)
                    SelectKey(i, wet::keybinds::kActions[i].fallback);
                return 0;
            case kKeysCancel:
            case IDCANCEL:
                CloseKeys();
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC:
            SetBkMode(HDC(wp), TRANSPARENT);
            return LRESULT(GetSysColorBrush(COLOR_WINDOW));
        case WM_CLOSE:
            CloseKeys();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void OpenKeys()
    {
        if (s_keysWindow)
            return;
        constexpr int kRows = (wet::keybinds::kActionCount + 1) / 2;
        RECT rc{ 0, 0, S(560), S(kRows * 30 + 64) };
        DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
        AdjustWindowRect(&rc, style, FALSE);
        RECT owner;
        GetWindowRect(s_window, &owner);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        s_keysWindow = CreateWindowExW(WS_EX_DLGMODALFRAME, L"WetLauncherKeys", L"Keyboard bindings", style,
                                       (owner.left + owner.right - w) / 2, (owner.top + owner.bottom - h) / 2, w, h,
                                       s_window, nullptr, GetModuleHandleW(nullptr), nullptr);
        s_parent = s_keysWindow;
        for (int i = 0; i < wet::keybinds::kActionCount; i++)
        {
            int x = (i % 2) * 280 + 12, y = (i / 2) * 30 + 12;
            Make(L"STATIC", Wide(wet::keybinds::kActions[i].label).c_str(), SS_LEFT, x, y + 3, 120, 18, 0);
            Make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, x + 124, y, 136, 300, kKeyFirst + i);
            std::string key = std::string("key_") + wet::keybinds::kActions[i].id;
            SelectKey(i, s_ini.GetString(key, wet::keybinds::kActions[i].fallback));
        }
        int by = kRows * 30 + 22;
        Make(L"BUTTON", L"Reset to defaults", BS_PUSHBUTTON | WS_TABSTOP, 12, by, 140, 28, kKeysReset);
        Make(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 338, by, 100, 28, kKeysOk);
        Make(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 448, by, 100, 28, kKeysCancel);
        s_parent = s_window;
        EnableWindow(s_window, FALSE);
        ShowWindow(s_keysWindow, SW_SHOW);
    }

    void Label(const wchar_t* text, int x, int y, int w = 110) { Make(L"STATIC", text, SS_LEFT, x, y + 3, w, 18, 0); }

    HWND Combo(int id, int x, int y, int w, std::initializer_list<const wchar_t*> items, int selected)
    {
        HWND c = Make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, x, y, w, 200, id);
        for (auto* item : items)
            SendMessageW(c, CB_ADDSTRING, 0, LPARAM(item));
        SendMessageW(c, CB_SETCURSEL, WPARAM(selected), 0);
        return c;
    }

    HWND Check(int id, const wchar_t* text, int x, int y, bool on, int w = 250)
    {
        HWND c = Make(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 20, id);
        SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        return c;
    }

    void UpdateValueLabel(int sliderId, int labelId, const wchar_t* suffix)
    {
        int v = int(SendMessageW(Item(sliderId), TBM_GETPOS, 0, 0));
        wchar_t text[32];
        std::swprintf(text, 32, L"%d%s", v, suffix);
        SetWindowTextW(Item(labelId), text);
    }

    void Slider(int id, int labelId, int x, int y, int w, int lo, int hi, int value, const wchar_t* suffix)
    {
        HWND t = Make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, x, y, w, 24, id);
        SendMessageW(t, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
        SendMessageW(t, TBM_SETPAGESIZE, 0, 10);
        SendMessageW(t, TBM_SETPOS, TRUE, value);
        Make(L"STATIC", L"", SS_LEFT, x + w + 4, y + 3, 44, 18, labelId);
        UpdateValueLabel(id, labelId, suffix);
    }

    void Group(const wchar_t* title, int x, int y, int w, int h) { Make(L"BUTTON", title, BS_GROUPBOX, x, y, w, h, 0); }

    // game/extracted above the launcher's folder (as WetRecomp.exe looks for it).
    fs::path FindGame()
    {
        std::error_code ec;
        for (fs::path dir = s_dir; !dir.empty(); dir = dir.parent_path())
        {
            if (fs::exists(dir / "game" / "extracted" / "default.xex", ec))
                return dir / "game" / "extracted";
            if (dir == dir.parent_path())
                break;
        }
        return {};
    }

    // ---- Banner ----
    // The picture across the top: launcher_banner.png / .jpg next to the
    // launcher if you put one there, otherwise the game's own dashboard
    // background, read from your disc (game\extracted\nxeart) when the
    // launcher starts. Nothing of the game is stored with the launcher.
    constexpr int kBannerWidth = 690, kBannerHeight = 150;
    HBITMAP s_banner = nullptr;

    std::vector<uint8_t> ReadAll(const fs::path& path)
    {
        std::ifstream f(path, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }

    // nxebg.jpg from the disc's NXE art package (an STFS package whose file
    // table and files are in consecutive 4 KB blocks).
    std::vector<uint8_t> DashboardBackground()
    {
        if (s_gameDir.empty())
            return {};
        std::vector<uint8_t> d = ReadAll(s_gameDir / "nxeart");
        if (d.size() < 0x10000 || (std::memcmp(d.data(), "PIRS", 4) && std::memcmp(d.data(), "LIVE", 4) &&
                                   std::memcmp(d.data(), "CON ", 4)))
            return {};
        static const char kName[] = "nxebg.jpg";
        for (size_t entry = 0x1000; entry + 0x40 <= d.size(); entry += 0x40)
        {
            if (std::memcmp(&d[entry], kName, sizeof(kName)))
                continue;
            size_t table = entry & ~size_t(0xFFF);   // the file table is block 0
            size_t block = d[entry + 0x2F] | (d[entry + 0x30] << 8) | (d[entry + 0x31] << 16);
            size_t size = (size_t(d[entry + 0x34]) << 24) | (d[entry + 0x35] << 16) | (d[entry + 0x36] << 8) |
                          d[entry + 0x37];
            size_t start = table + block * 0x1000;
            if (size < 4 || start + size > d.size() || d[start] != 0xFF || d[start + 1] != 0xD8 ||
                d[start + size - 2] != 0xFF || d[start + size - 1] != 0xD9)
                return {};   // not stored in one piece: no banner rather than a broken one
            return std::vector<uint8_t>(d.begin() + start, d.begin() + start + size);
        }
        return {};
    }

    // Decodes an image, crops it to the banner's shape (from the top, or the
    // middle for your own picture) and scales it to width x height pixels.
    HBITMAP DecodeBanner(const std::vector<uint8_t>& data, bool fromTop, int width, int height)
    {
        IWICImagingFactory* wic = nullptr;
        if (data.empty() || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                                    IID_PPV_ARGS(&wic))))
            return nullptr;
        HBITMAP result = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;
        IWICBitmapClipper* clipper = nullptr;
        IWICBitmapScaler* scaler = nullptr;
        UINT w = 0, h = 0;
        if (SUCCEEDED(wic->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(data.data()), DWORD(data.size()))) &&
            SUCCEEDED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w && h &&
            SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                            0.0, WICBitmapPaletteTypeCustom)))
        {
            // Largest part of the image with the banner's shape.
            WICRect crop{ 0, 0, INT(w), INT(h) };
            if (uint64_t(w) * height > uint64_t(h) * width)
            {
                crop.Width = INT(uint64_t(h) * width / height);
                crop.X = (INT(w) - crop.Width) / 2;
            }
            else
            {
                crop.Height = INT(uint64_t(w) * height / width);
                crop.Y = fromTop ? 0 : (INT(h) - crop.Height) / 2;
            }
            std::vector<uint8_t> pixels(size_t(width) * height * 4);
            if (SUCCEEDED(wic->CreateBitmapClipper(&clipper)) && SUCCEEDED(clipper->Initialize(converter, &crop)) &&
                SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
                // 4 = high-quality cubic (Windows 10; not in every SDK's headers).
                (SUCCEEDED(scaler->Initialize(clipper, width, height, WICBitmapInterpolationMode(4))) ||
                 SUCCEEDED(scaler->Initialize(clipper, width, height, WICBitmapInterpolationModeFant))) &&
                SUCCEEDED(scaler->CopyPixels(nullptr, width * 4, UINT(pixels.size()), pixels.data())))
            {
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth = width;
                bi.bmiHeader.biHeight = -height;   // top-down
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                void* bits = nullptr;
                result = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
                if (result && bits)
                    std::memcpy(bits, pixels.data(), pixels.size());
            }
        }
        for (IUnknown* u : std::initializer_list<IUnknown*>{ scaler, clipper, converter, frame, decoder, stream, wic })
            if (u)
                u->Release();
        return result;
    }

    void LoadBanner()
    {
        for (const char* name : { "launcher_banner.png", "launcher_banner.jpg", "launcher_banner.jpeg" })
            if (fs::exists(s_dir / name))
                if ((s_banner = DecodeBanner(ReadAll(s_dir / name), false, S(kBannerWidth), S(kBannerHeight))))
                    break;
        if (!s_banner)
            s_banner = DecodeBanner(DashboardBackground(), true, S(kBannerWidth), S(kBannerHeight));
        s_bannerHeight = s_banner ? kBannerHeight : 0;
    }

    void PaintBanner(HWND hwnd)
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (s_banner)
        {
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, s_banner);
            BitBlt(dc, 0, 0, S(kBannerWidth), S(kBannerHeight), mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
    }

    void UpdateStatus()
    {
        std::wstring text;
        if (!fs::exists(s_dir / "WetRecomp.exe"))
            text = L"WetRecomp.exe is missing - the launcher must be next to it.";
        else if (s_gameDir.empty())
            text = L"Game files not found: put your extracted disc in game\\extracted (with default.xex).";
        else
            text = L"Game files: " + s_gameDir.wstring();
        SetWindowTextW(Item(kStatus), text.c_str());
        EnableWindow(Item(kPlay), fs::exists(s_dir / "WetRecomp.exe") && !s_gameDir.empty());
    }

    bool Checked(int id) { return SendMessageW(Item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
    int Selected(int id) { return int(SendMessageW(Item(id), CB_GETCURSEL, 0, 0)); }
    int Pos(int id) { return int(SendMessageW(Item(id), TBM_GETPOS, 0, 0)); }

    std::string ChosenMonitor(int sel);

    void SaveSettings()
    {
        int fps = Selected(kFps);
        s_ini.Set("fps", s_fpsValues[fps < 0 ? 0 : size_t(fps)]);
        s_ini.Set("resolution_scale", Selected(kResolution) + 1);
        s_ini.Set("fullscreen", Checked(kFullscreen));
        s_ini.Set("show_fps", Checked(kShowFps));
        s_ini.Set("filter", Selected(kFilter));
        s_ini.Set("sharpness", Pos(kSharpness));
        s_ini.Set("antialiasing", Selected(kAntialiasing));
        s_ini.Set("film_grain", Pos(kGrain));
        s_ini.Set("fov", Pos(kFov));
        s_ini.Set("widescreen", Checked(kWidescreen));
        s_ini.Set("vsync", Checked(kVsync));
        s_ini.Set("button_prompts", Selected(kPrompts));
        s_ini.Set("hide_borders", Checked(kHideBorders));
        s_ini.Set("widescreen_hud", Checked(kWideHud));
        s_ini.Set("mouse_aim", Checked(kMouseAim));
        s_ini.Set("mouse_sensitivity", Pos(kSensitivity));
        s_ini.Set("mouse_invert_y", Checked(kInvertY));
        s_ini.Set("vibration", Checked(kVibration));
        s_ini.Set("shader_cache", Checked(kShaderCache));
        s_ini.Set("video_mode", Checked(kVideoMode));
        s_ini.Set("achievement_popups", Checked(kPopups));
        s_ini.Set("async_shaders", Checked(kAsyncShaders));
        s_ini.Set("shader_preload", Checked(kShaderPreload));
        s_ini.Set("motion_blur", Checked(kMotionBlur));
        {
            static const int kLanguageIds[] = { 0, 1, 3, 4, 5, 6 };
            int pick = Selected(kLanguage);
            s_ini.Set("language", kLanguageIds[pick >= 0 && pick < 6 ? pick : 0]);
        }
        s_ini.Set("depth_of_field", Checked(kDepthOfField));
        s_ini.Set("camera_shake", Checked(kCameraShake));
        s_ini.SetString("monitor", ChosenMonitor(Selected(kMonitor)));
        if (!s_ini.Save(s_iniPath))
            MessageBoxW(s_window, L"Could not save wet.ini.", L"WET Launcher", MB_ICONERROR);
    }

    void Play()
    {
        SaveSettings();
        std::wstring exe = (s_dir / "WetRecomp.exe").wstring();
        std::wstring cmd = L"\"" + exe + L"\"";
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, s_dir.c_str(), &si, &pi))
        {
            MessageBoxW(s_window, L"Could not start WetRecomp.exe.", L"WET Launcher", MB_ICONERROR);
            return;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        DestroyWindow(s_window);
    }

    // ---- Saves: back up and import ----
    // The saves are game\hdd\content\425307DB\00000001\WETSAVEGAME (the game's
    // own files: CHKPTS.DAT, COLLECT.DAT, ACHIEVE.DAT...), the achievements
    // game\hdd\achievements.ini.
    fs::path SaveFolder() { return s_hddDir / L"content" / L"425307DB" / L"00000001" / L"WETSAVEGAME"; }

    // PowerShell's Compress-Archive / Expand-Archive, hidden; true when it worked.
    bool PowerShell(const std::wstring& script)
    {
        std::wstring cmd = L"powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"" + script + L"\"";
        STARTUPINFOW si{ sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, s_dir.c_str(), &si,
                            &pi))
            return false;
        WaitForSingleObject(pi.hProcess, 120000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return code == 0;
    }

    std::wstring Stamp()
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t text[32];
        std::swprintf(text, 32, L"%04d-%02d-%02d_%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
        return text;
    }

    // Zips the saves (content folder) and achievements.ini to `zip`.
    bool BackupTo(const fs::path& zip)
    {
        std::error_code ec;
        std::vector<std::wstring> parts;
        if (fs::exists(s_hddDir / L"content", ec))
            parts.push_back(L"'" + (s_hddDir / L"content").wstring() + L"'");
        if (fs::exists(s_hddDir / L"achievements.ini", ec))
            parts.push_back(L"'" + (s_hddDir / L"achievements.ini").wstring() + L"'");
        if (parts.empty())
            return false;
        std::wstring list;
        for (auto& p : parts)
            list += (list.empty() ? L"" : L",") + p;
        fs::create_directories(zip.parent_path(), ec);
        return PowerShell(L"Compress-Archive -Force -Path " + list + L" -DestinationPath '" + zip.wstring() + L"'") &&
               fs::exists(zip, ec);
    }

    bool HaveSaves()
    {
        std::error_code ec;
        return fs::exists(SaveFolder(), ec) && !fs::is_empty(SaveFolder(), ec);
    }

    void BackupSaves()
    {
        if (!HaveSaves())
        {
            MessageBoxW(s_window, L"There are no saves yet.", L"WET Launcher", MB_ICONINFORMATION);
            return;
        }
        wchar_t file[MAX_PATH] = {};
        std::wstring name = L"WET-saves-" + Stamp() + L".zip";
        wcsncpy(file, name.c_str(), MAX_PATH - 1);
        wchar_t docs[MAX_PATH] = {};
        SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs);
        OPENFILENAMEW ofn{ sizeof(ofn) };
        ofn.hwndOwner = s_window;
        ofn.lpstrFilter = L"Zip files (*.zip)\0*.zip\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrInitialDir = docs;
        ofn.lpstrDefExt = L"zip";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&ofn))
            return;
        if (BackupTo(file))
        {
            std::wstring select = L"/select,\"" + std::wstring(file) + L"\"";
            ShellExecuteW(s_window, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
            MessageBoxW(s_window, (L"Saves and achievements backed up to\n" + std::wstring(file)).c_str(), L"WET Launcher",
                        MB_ICONINFORMATION);
        }
        else
            MessageBoxW(s_window, L"Couldn't make the backup.", L"WET Launcher", MB_ICONWARNING);
    }

    // The folder of save files in `dir`: a WETSAVEGAME folder anywhere below it
    // (our backups, Xenia's content folder), or `dir` itself if it holds the
    // game's save files.
    fs::path FindSaveFiles(const fs::path& dir)
    {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec))
        {
            if (it.depth() > 8)
            {
                it.disable_recursion_pending();
                continue;
            }
            std::wstring n = it->path().filename().wstring();
            for (auto& c : n)
                c = wchar_t(towupper(c));
            if (it->is_directory(ec) && n == L"WETSAVEGAME")
                return it->path();
        }
        for (const wchar_t* f : { L"CHKPTS.DAT", L"COLLECT.DAT", L"ACHIEVE.DAT" })
            if (fs::exists(dir / f, ec))
                return dir;
        return {};
    }

    void ImportSaves(bool fromZip)
    {
        fs::path source;
        if (fromZip)
        {
            wchar_t file[MAX_PATH] = {};
            OPENFILENAMEW ofn{ sizeof(ofn) };
            ofn.hwndOwner = s_window;
            ofn.lpstrFilter = L"Save backups (*.zip)\0*.zip\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (!GetOpenFileNameW(&ofn))
                return;
            source = file;
        }
        else
        {
            IFileOpenDialog* dlg = nullptr;
            if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                        reinterpret_cast<void**>(&dlg))))
                return;
            DWORD opts = 0;
            dlg->GetOptions(&opts);
            dlg->SetOptions(opts | FOS_PICKFOLDERS);
            dlg->SetTitle(L"The folder with your WET saves (a WETSAVEGAME folder, or one above it, e.g. Xenia's content folder)");
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->Show(s_window)) && SUCCEEDED(dlg->GetResult(&item)))
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                {
                    source = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
            dlg->Release();
            if (source.empty())
                return;
        }

        std::error_code ec;
        fs::path temp;
        fs::path from = source;
        if (fromZip)
        {
            wchar_t tmp[MAX_PATH];
            GetTempPathW(MAX_PATH, tmp);
            temp = fs::path(tmp) / (L"wet-save-import-" + Stamp());
            fs::remove_all(temp, ec);
            if (!PowerShell(L"Expand-Archive -Force -Path '" + source.wstring() + L"' -DestinationPath '" + temp.wstring() + L"'"))
            {
                MessageBoxW(s_window, L"Couldn't open that zip.", L"WET Launcher", MB_ICONWARNING);
                return;
            }
            from = temp;
        }
        fs::path saves = FindSaveFiles(from);
        if (saves.empty())
        {
            fs::remove_all(temp, ec);
            MessageBoxW(s_window,
                        L"No WET saves found there (a WETSAVEGAME folder, or CHKPTS.DAT / COLLECT.DAT files).",
                        L"WET Launcher", MB_ICONWARNING);
            return;
        }
        // Our own backup also has achievements.ini at its top.
        fs::path achievements = from / L"achievements.ini";
        bool withAchievements = fromZip && fs::exists(achievements, ec);
        if (MessageBoxW(s_window,
                        (L"Import the saves from\n" + saves.wstring() + L"?\n\nYour current saves" +
                         std::wstring(withAchievements ? L" and achievements" : L"") +
                         L" are replaced. They're backed up first to game\\hdd\\save-backups. Close the game before "
                         L"importing.")
                            .c_str(),
                        L"WET Launcher", MB_YESNO | MB_ICONQUESTION) != IDYES)
        {
            fs::remove_all(temp, ec);
            return;
        }
        if (HaveSaves() && !BackupTo(s_hddDir / L"save-backups" / (L"before-import-" + Stamp() + L".zip")))
        {
            fs::remove_all(temp, ec);
            MessageBoxW(s_window, L"Couldn't back up the current saves, so nothing was imported.", L"WET Launcher",
                        MB_ICONWARNING);
            return;
        }
        fs::path target = SaveFolder();
        fs::remove_all(target, ec);
        fs::create_directories(target, ec);
        fs::copy(saves, target, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        bool ok = !ec;
        if (ok && withAchievements)
            fs::copy_file(achievements, s_hddDir / L"achievements.ini", fs::copy_options::overwrite_existing, ec);
        fs::remove_all(temp, ec);
        MessageBoxW(s_window, ok ? L"Saves imported." : L"Some files couldn't be copied (is the game running?).",
                    L"WET Launcher", ok ? MB_ICONINFORMATION : MB_ICONWARNING);
    }

    // ---- Free up space: what only building needed (launcher/maintenance.h) ----
    void FreeSpace()
    {
        fs::path repo = s_dir.parent_path().parent_path();   // <repo>\build\runtime
        std::error_code ec;
        std::vector<wet::maintenance::Part> parts;
        if (fs::exists(repo / L".git", ec) || fs::exists(repo / L"CMakeLists.txt", ec))
        {
            parts.push_back(wet::maintenance::Leftovers(repo));
            parts.push_back(wet::maintenance::Tools(repo));
        }
        parts.push_back(wet::maintenance::Captures(s_dir));
        uint64_t total = 0;
        std::wstring list;
        for (auto& p : parts)
            if (p.bytes)
            {
                total += p.bytes;
                list += L"  \u2022 " + p.what + L": " + wet::maintenance::Size(p.bytes) + L"\n";
            }
        if (!total)
        {
            MessageBoxW(s_window, L"Nothing to clean up.", L"WET Launcher", MB_ICONINFORMATION);
            return;
        }
        if (MessageBoxW(s_window,
                        (L"Delete these?\n\n" + list + L"\nThat frees " + wet::maintenance::Size(total) +
                         L". The game keeps working, and your saves and settings stay. The next update downloads the "
                         L"tools again and rebuilds from scratch, so it takes as long as the first install.")
                            .c_str(),
                        L"WET Launcher - free up space", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        bool ok = true;
        for (auto& p : parts)
            ok &= wet::maintenance::Delete(p);
        MessageBoxW(s_window, ok ? (L"Freed " + wet::maintenance::Size(total) + L".").c_str()
                                 : L"Some files couldn't be deleted (is something using them?).",
                    L"WET Launcher", ok ? MB_ICONINFORMATION : MB_ICONWARNING);
    }

    // A small menu under a button.
    int ButtonMenu(int buttonId, std::initializer_list<std::pair<int, const wchar_t*>> items)
    {
        HMENU menu = CreatePopupMenu();
        for (auto& [id, text] : items)
            AppendMenuW(menu, id ? MF_STRING : MF_SEPARATOR, id, text);
        RECT r;
        GetWindowRect(Item(buttonId), &r);
        int pick = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, s_window, nullptr);
        DestroyMenu(menu);
        return pick;
    }

    void OpenSaves()
    {
        std::error_code ec;
        fs::create_directories(s_hddDir, ec);
        ShellExecuteW(s_window, L"open", s_hddDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void ClearShaders()
    {
        fs::path dir = s_hddDir / "shaders";
        std::error_code ec;
        if (!fs::exists(dir, ec))
        {
            MessageBoxW(s_window, L"The shader cache is already empty.", L"WET Launcher", MB_ICONINFORMATION);
            return;
        }
        if (MessageBoxW(s_window,
                        L"Delete the shader cache?\n\nIt is rebuilt as you play; effects stutter once more the "
                        L"first time they appear.",
                        L"WET Launcher", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        fs::remove_all(dir, ec);
        MessageBoxW(s_window, ec ? L"Some files could not be deleted (is the game running?)." : L"Shader cache cleared.",
                    L"WET Launcher", ec ? MB_ICONWARNING : MB_ICONINFORMATION);
    }

    // ---- Update check: the latest release on GitHub vs. this build ----
    constexpr UINT kUpdateResult = WM_APP + 1;   // lParam: new std::wstring*

    std::string HttpsGet(const wchar_t* host, const wchar_t* path)
    {
        std::string body;
        HINTERNET session = WinHttpOpen(L"WET-Launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
            return body;
        WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
        HINTERNET connect = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
        HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                    : nullptr;
        if (request && WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", DWORD(-1), nullptr, 0,
                                          0, 0) &&
            WinHttpReceiveResponse(request, nullptr))
        {
            DWORD size = 0;
            while (WinHttpQueryDataAvailable(request, &size) && size)
            {
                std::string chunk(size, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), size, &read) || !read)
                    break;
                body.append(chunk.data(), read);
            }
        }
        if (request)
            WinHttpCloseHandle(request);
        if (connect)
            WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return body;
    }

    // "0.1.0" -> {0, 1, 0}
    std::vector<int> VersionNumbers(const std::string& v)
    {
        std::vector<int> out;
        int n = -1;
        for (char c : v)
        {
            if (c >= '0' && c <= '9')
                n = (n < 0 ? 0 : n * 10) + (c - '0');
            else if (c == '.' && n >= 0)
            {
                out.push_back(n);
                n = -1;
            }
            else if (n >= 0 || !out.empty())
                break;
        }
        if (n >= 0)
            out.push_back(n);
        return out;
    }

    std::atomic<bool> s_updateAvailable{ false };

    // Update now: Setup (built with the launcher) updates the source to the
    // latest release and rebuilds. It runs from a copy, since the build replaces
    // the programs in this folder, and the launcher closes so its exe can be replaced.
    void UpdateNow()
    {
        fs::path repo = s_dir.parent_path().parent_path();   // <repo>\build\runtime
        fs::path setup = s_dir / L"WetSetup.exe";
        std::error_code ec;
        if (!fs::exists(repo / L".git", ec) || !fs::exists(setup, ec))
        {
            MessageBoxW(s_window,
                        L"This copy can't update itself (it isn't a built source folder with WetSetup.exe). Download "
                        L"WetSetup.exe from the GitHub releases page and run it.",
                        L"WET Launcher", MB_ICONINFORMATION);
            ShellExecuteW(s_window, L"open", L"https://github.com/zonehammer/wet-recomp/releases/latest", nullptr,
                          nullptr, SW_SHOWNORMAL);
            return;
        }
        if (MessageBoxW(s_window,
                        L"Update to the latest version?\n\nThe launcher closes, Setup downloads the new code and "
                        L"rebuilds (a few minutes, longer if the tools have to be downloaded again), then the launcher "
                        L"opens again. Your saves and settings stay. Close the game first.",
                        L"WET Launcher", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        SaveSettings();
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        fs::path copy = fs::path(tmp) / L"WetSetup-update.exe";
        fs::copy_file(setup, copy, fs::copy_options::overwrite_existing, ec);
        std::wstring cmd = L"\"" + copy.wstring() + L"\" --update \"" + repo.parent_path().wstring() + L"\"";
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        if (ec || !CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, tmp, &si, &pi))
        {
            MessageBoxW(s_window, L"Couldn't start Setup for the update.", L"WET Launcher", MB_ICONWARNING);
            return;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        DestroyWindow(s_window);
    }

    void CheckForUpdates()
    {
        if (s_updateAvailable)
        {
            UpdateNow();
            return;
        }
        SetWindowTextW(Item(kUpdateText), L"Checking...");
        EnableWindow(Item(kCheckUpdates), FALSE);
        std::thread([] {
            std::string json = HttpsGet(L"api.github.com", L"/repos/zonehammer/wet-recomp/releases/latest");
            std::wstring* text = new std::wstring;
            size_t at = json.find("\"tag_name\"");
            size_t q1 = at == std::string::npos ? at : json.find('"', json.find(':', at) + 1);
            size_t q2 = q1 == std::string::npos ? q1 : json.find('"', q1 + 1);
            if (q2 == std::string::npos)
                *text = L"Couldn't reach GitHub (offline?).";
            else
            {
                std::string tag = json.substr(q1 + 1, q2 - q1 - 1);
                std::vector<int> latest = VersionNumbers(tag), mine = VersionNumbers(WET_VERSION);
                std::wstring wtag(tag.begin(), tag.end());
                if (latest > mine)
                {
                    *text = L"Version " + wtag + L" is out: press Update now to get it (it rebuilds; 2-30 minutes).";
                    s_updateAvailable = true;
                }
                else
                    *text = L"Up to date (latest release " + wtag + L").";
            }
            PostMessageW(s_window, kUpdateResult, 0, LPARAM(text));
        }).detach();
    }

    // ---- Bug report: wet.log, wet.ini, xenia_gpu.log and system info in a zip ----
    std::wstring RegString(const wchar_t* key, const wchar_t* value)
    {
        wchar_t buf[256] = {};
        DWORD size = sizeof(buf);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            return buf;
        return L"?";
    }

    void ReportBug()
    {
        std::error_code ec;
        fs::path dir = s_dir / L"bug-report";
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        for (const wchar_t* name : { L"wet.log", L"wet.ini", L"xenia_gpu.log", L"profile.txt" })
            if (fs::exists(s_dir / name, ec))
                fs::copy_file(s_dir / name, dir / name, fs::copy_options::overwrite_existing, ec);
        {
            std::wofstream info(dir / L"system.txt");
            info << L"WET Recomp " << std::wstring(WET_VERSION, WET_VERSION + std::strlen(WET_VERSION)) << L"\n";
            const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
            info << L"Windows: " << RegString(nt, L"ProductName") << L" " << RegString(nt, L"DisplayVersion")
                 << L" build " << RegString(nt, L"CurrentBuild") << L"\n";
            info << L"CPU: " << RegString(L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString")
                 << L"\n";
            DISPLAY_DEVICEW dev{ sizeof(dev) };
            for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dev, 0); i++)
                if (dev.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)
                    info << L"GPU: " << dev.DeviceString << L"\n";
            MEMORYSTATUSEX mem{ sizeof(mem) };
            GlobalMemoryStatusEx(&mem);
            info << L"RAM: " << (mem.ullTotalPhys >> 20) << L" MB\n";
            info << L"Screen: " << GetSystemMetrics(SM_CXSCREEN) << L"x" << GetSystemMetrics(SM_CYSCREEN) << L"\n";
        }
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t name[64];
        std::swprintf(name, 64, L"WET-bug-report-%04d%02d%02d-%02d%02d.zip", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
        fs::path zip = s_dir / name;
        std::wstring cmd = L"powershell -NoProfile -NonInteractive -Command \"Compress-Archive -Force -Path '" +
                           (dir / L"*").wstring() + L"' -DestinationPath '" + zip.wstring() + L"'\"";
        STARTUPINFOW si{ sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        bool ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                 s_dir.c_str(), &si, &pi);
        if (ok)
        {
            WaitForSingleObject(pi.hProcess, 30000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        if (!ok || !fs::exists(zip, ec))
        {
            MessageBoxW(s_window, (L"Couldn't make the zip. The files are in:\n" + dir.wstring()).c_str(),
                        L"WET Launcher", MB_ICONWARNING);
            ShellExecuteW(s_window, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        fs::remove_all(dir, ec);
        std::wstring select = L"/select,\"" + zip.wstring() + L"\"";
        ShellExecuteW(s_window, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
        if (MessageBoxW(s_window,
                        (std::wstring(L"Saved ") + name +
                         L" (logs, settings and system info - nothing else).\n\nOpen the GitHub issue page to "
                         L"report the bug? Attach the zip and describe what happened.")
                            .c_str(),
                        L"WET Launcher", MB_YESNO | MB_ICONINFORMATION) == IDYES)
            ShellExecuteW(s_window, L"open", L"https://github.com/zonehammer/wet-recomp/issues/new", nullptr, nullptr,
                          SW_SHOWNORMAL);
    }

    // ---- Monitor choice (wet.ini monitor = \\.\DISPLAYn; empty = the main display) ----
    std::vector<std::wstring> s_monitors;   // device names, in the combo's order after "Main display"

    void FillMonitors(HWND combo, const std::string& chosen)
    {
        struct Ctx { HWND combo; const std::string* chosen; int sel; };
        Ctx c{ combo, &chosen, 0 };
        SendMessageW(combo, CB_ADDSTRING, 0, LPARAM(L"Main display"));
        s_monitors.clear();
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(p);
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (!GetMonitorInfoW(m, &info)) return TRUE;
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode);
            std::wstring name = info.szDevice;
            std::wstring shortName = name.substr(name.find_last_of(L'\\') + 1);   // DISPLAY2
            wchar_t text[160];
            std::swprintf(text, 160, L"%ls: %ldx%ld, %lu Hz%ls", shortName.c_str(), info.rcMonitor.right - info.rcMonitor.left,
                          info.rcMonitor.bottom - info.rcMonitor.top, mode.dmDisplayFrequency,
                          (info.dwFlags & MONITORINFOF_PRIMARY) ? L" (main)" : L"");
            SendMessageW(c->combo, CB_ADDSTRING, 0, LPARAM(text));
            s_monitors.push_back(name);
            std::wstring want(c->chosen->begin(), c->chosen->end());
            if (!want.empty() && _wcsicmp(want.c_str(), name.c_str()) == 0)
                c->sel = int(s_monitors.size());
            return TRUE;
        }, reinterpret_cast<LPARAM>(&c));
        SendMessageW(combo, CB_SETCURSEL, WPARAM(c.sel), 0);
    }

    std::string ChosenMonitor(int sel)
    {
        if (sel <= 0 || size_t(sel) > s_monitors.size()) return {};
        const std::wstring& w = s_monitors[size_t(sel) - 1];
        return std::string(w.begin(), w.end());   // \\.\DISPLAYn: ASCII
    }

    // ---- Small tool windows: achievements, controller test ----
    HWND s_toolWindow = nullptr;

    HWND OpenTool(const wchar_t* cls, const wchar_t* title, int w, int h)
    {
        if (s_toolWindow)
            DestroyWindow(s_toolWindow);
        RECT rc{ 0, 0, S(w), S(h) };
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        AdjustWindowRect(&rc, style, FALSE);
        RECT owner;
        GetWindowRect(s_window, &owner);
        int ww = rc.right - rc.left, hh = rc.bottom - rc.top;
        s_toolWindow = CreateWindowExW(0, cls, title, style, (owner.left + owner.right - ww) / 2,
                                       (owner.top + owner.bottom - hh) / 2, ww, hh, s_window, nullptr,
                                       GetModuleHandleW(nullptr), nullptr);
        return s_toolWindow;
    }

    std::wstring FromUtf8(const std::string& s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
        std::wstring w(size_t(std::max(n, 0)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
        return w;
    }

    // The game writes <hdd>/achievements_list.txt (tab-separated: id, gamerscore,
    // unlocked, unlock time, secret, name, description).
    void OpenAchievements()
    {
        std::ifstream in(s_hddDir / "achievements_list.txt");
        if (!in)
        {
            MessageBoxW(s_window, L"No achievements list yet: start the game once and it is written.", L"WET Launcher",
                        MB_ICONINFORMATION);
            return;
        }
        struct Row { std::wstring name, desc, when; int points; bool unlocked; };
        std::vector<Row> rows;
        int got = 0, total = 0, pts = 0, totalPts = 0;
        std::string line;
        while (std::getline(in, line))
        {
            if (line.empty() || line[0] == '#') continue;
            std::vector<std::string> f;
            size_t a = 0;
            for (size_t b; (b = line.find('\t', a)) != std::string::npos; a = b + 1)
                f.push_back(line.substr(a, b - a));
            f.push_back(line.substr(a));
            if (f.size() < 7) continue;
            Row r;
            r.points = std::atoi(f[1].c_str());
            r.unlocked = f[2] == "1";
            r.name = FromUtf8(f[5]);
            r.desc = FromUtf8(f[6]);
            if (!f[6].empty() && f[6].back() == '\r') r.desc.pop_back();
            unsigned long long t = std::strtoull(f[3].c_str(), nullptr, 10);
            if (r.unlocked && t)
            {
                // A FILETIME (100 ns since 1601) or a Unix time.
                FILETIME ft;
                ULARGE_INTEGER u;
                u.QuadPart = t > 100000000000ull ? t : t * 10000000ull + 116444736000000000ull;
                ft.dwLowDateTime = u.LowPart;
                ft.dwHighDateTime = u.HighPart;
                SYSTEMTIME st, local;
                FileTimeToSystemTime(&ft, &st);
                SystemTimeToTzSpecificLocalTime(nullptr, &st, &local);
                wchar_t d[64];
                std::swprintf(d, 64, L"%04d-%02d-%02d", local.wYear, local.wMonth, local.wDay);
                r.when = d;
            }
            else if (r.unlocked)
                r.when = L"yes";
            total++;
            totalPts += r.points;
            if (r.unlocked) { got++; pts += r.points; }
            rows.push_back(std::move(r));
        }
        std::stable_sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.unlocked > y.unlocked; });
        wchar_t title[128];
        std::swprintf(title, 128, L"Achievements: %d of %d, %d of %d G", got, total, pts, totalPts);
        HWND w = OpenTool(L"WetLauncherTool", title, 640, 420);
        HWND list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOSORTHEADER,
                                    S(8), S(8), S(624), S(404), w, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(list, WM_SETFONT, WPARAM(s_font), TRUE);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT);
        const struct { const wchar_t* name; int width; } cols[] = {
            { L"Achievement", 170 }, { L"Description", 300 }, { L"G", 40 }, { L"Unlocked", 90 },
        };
        for (int i = 0; i < 4; i++)
        {
            LVCOLUMNW c{};
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = const_cast<wchar_t*>(cols[i].name);
            c.cx = S(cols[i].width);
            ListView_InsertColumn(list, i, &c);
        }
        for (size_t i = 0; i < rows.size(); i++)
        {
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = int(i);
            item.pszText = rows[i].name.data();
            ListView_InsertItem(list, &item);
            ListView_SetItemText(list, int(i), 1, rows[i].desc.data());
            std::wstring p = std::to_wstring(rows[i].points);
            ListView_SetItemText(list, int(i), 2, p.data());
            ListView_SetItemText(list, int(i), 3, rows[i].when.data());
        }
        ShowWindow(w, SW_SHOW);
    }

    // Controller test: XInput pads (Xbox and compatible) and other game
    // controllers (DirectInput / WinMM: PlayStation pads and the like), live.
    typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, XINPUT_STATE*);
    XInputGetStateFn s_xinput = nullptr;
    constexpr UINT_PTR kPadTimer = 1;
    constexpr int kPadText = 900;

    std::wstring PadReport()
    {
        std::wstring out;
        bool any = false;
        if (s_xinput)
            for (DWORD i = 0; i < 4; i++)
            {
                XINPUT_STATE st{};
                if (s_xinput(i, &st) != ERROR_SUCCESS) continue;
                any = true;
                const XINPUT_GAMEPAD& g = st.Gamepad;
                static const struct { WORD bit; const wchar_t* name; } buttons[] = {
                    { XINPUT_GAMEPAD_A, L"A" }, { XINPUT_GAMEPAD_B, L"B" }, { XINPUT_GAMEPAD_X, L"X" },
                    { XINPUT_GAMEPAD_Y, L"Y" }, { XINPUT_GAMEPAD_LEFT_SHOULDER, L"LB" },
                    { XINPUT_GAMEPAD_RIGHT_SHOULDER, L"RB" }, { XINPUT_GAMEPAD_BACK, L"Back" },
                    { XINPUT_GAMEPAD_START, L"Start" }, { XINPUT_GAMEPAD_LEFT_THUMB, L"L-stick" },
                    { XINPUT_GAMEPAD_RIGHT_THUMB, L"R-stick" }, { XINPUT_GAMEPAD_DPAD_UP, L"Up" },
                    { XINPUT_GAMEPAD_DPAD_DOWN, L"Down" }, { XINPUT_GAMEPAD_DPAD_LEFT, L"Left" },
                    { XINPUT_GAMEPAD_DPAD_RIGHT, L"Right" },
                };
                std::wstring pressed;
                for (auto& b : buttons)
                    if (g.wButtons & b.bit) pressed += std::wstring(pressed.empty() ? L"" : L" ") + b.name;
                wchar_t line[400];
                std::swprintf(line, 400,
                              L"Xbox controller %lu\r\n   Left stick %+5.2f %+5.2f    Right stick %+5.2f %+5.2f\r\n"
                              L"   Triggers  LT %3d%%  RT %3d%%\r\n   Buttons: %ls\r\n\r\n",
                              i + 1, g.sThumbLX / 32767.0, g.sThumbLY / 32767.0, g.sThumbRX / 32767.0,
                              g.sThumbRY / 32767.0, g.bLeftTrigger * 100 / 255, g.bRightTrigger * 100 / 255,
                              pressed.empty() ? L"-" : pressed.c_str());
                out += line;
            }
        // Other controllers (WinMM also lists XInput pads: skip ones with the XInput name).
        UINT n = joyGetNumDevs();
        for (UINT i = 0; i < n && i < 16; i++)
        {
            JOYINFOEX ji{ sizeof(ji) };
            ji.dwFlags = JOY_RETURNALL;
            if (joyGetPosEx(i, &ji) != JOYERR_NOERROR) continue;
            JOYCAPSW caps{};
            joyGetDevCapsW(i, &caps, sizeof(caps));
            std::wstring name = caps.szPname;
            if (name.find(L"XINPUT") != std::wstring::npos || name.find(L"Xbox") != std::wstring::npos) continue;
            any = true;
            std::wstring pressed;
            for (int b = 0; b < 32; b++)
                if (ji.dwButtons & (1u << b)) pressed += (pressed.empty() ? L"" : L" ") + std::to_wstring(b + 1);
            auto axis = [](DWORD v, UINT lo, UINT hi) { return hi > lo ? (int(v) - int(lo)) * 2.0 / (hi - lo) - 1.0 : 0.0; };
            wchar_t line[400];
            std::swprintf(line, 400,
                          L"%ls (game controller %u)\r\n   Axes X %+5.2f  Y %+5.2f  Z %+5.2f  R %+5.2f\r\n"
                          L"   Buttons: %ls\r\n\r\n",
                          name.empty() ? L"Controller" : name.c_str(), i + 1, axis(ji.dwXpos, caps.wXmin, caps.wXmax),
                          axis(ji.dwYpos, caps.wYmin, caps.wYmax), axis(ji.dwZpos, caps.wZmin, caps.wZmax),
                          axis(ji.dwRpos, caps.wRmin, caps.wRmax), pressed.empty() ? L"-" : pressed.c_str());
            out += line;
        }
        if (!any)
            out = L"No controller found. Connect one (USB or Bluetooth) - it shows up here within a second.";
        else
            out += L"The game reads PlayStation and other controllers through SDL, with the Xbox layout.";
        return out;
    }

    LRESULT CALLBACK ToolProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_TIMER:
            if (wp == kPadTimer)
            {
                std::wstring text = PadReport();
                wchar_t old[2048] = {};
                GetDlgItemTextW(hwnd, kPadText, old, 2048);
                if (text != old)
                    SetDlgItemTextW(hwnd, kPadText, text.c_str());
            }
            return 0;
        case WM_CTLCOLORSTATIC:
            SetBkMode(HDC(wp), TRANSPARENT);
            return LRESULT(GetSysColorBrush(COLOR_WINDOW));
        case WM_DESTROY:
            KillTimer(hwnd, kPadTimer);
            if (hwnd == s_toolWindow) s_toolWindow = nullptr;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void OpenPadTest()
    {
        if (!s_xinput)
            if (HMODULE x = LoadLibraryW(L"xinput1_4.dll"); x || (x = LoadLibraryW(L"xinput9_1_0.dll")))
                s_xinput = reinterpret_cast<XInputGetStateFn>(reinterpret_cast<void*>(GetProcAddress(x, "XInputGetState")));
        HWND w = OpenTool(L"WetLauncherTool", L"Controller test", 460, 300);
        HWND t = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, S(12), S(10), S(436), S(280), w,
                                 reinterpret_cast<HMENU>(intptr_t(kPadText)), GetModuleHandleW(nullptr), nullptr);
        HFONT mono = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                                 FIXED_PITCH, L"Consolas");
        SendMessageW(t, WM_SETFONT, WPARAM(mono), TRUE);
        SetWindowTextW(t, PadReport().c_str());
        SetTimer(w, kPadTimer, 50, nullptr);
        ShowWindow(w, SW_SHOW);
    }

    void BuildControls()
    {
        const Ini& ini = s_ini;
        // Display
        Group(L"Display", 10, 8, 330, 164 + kEffectsRow);
        Label(L"Frame rate", 22, 30);
        int fps = ini.Get("fps", 30), fpsIndex = -1;
        for (size_t i = 0; i < s_fpsValues.size(); i++)
            if (s_fpsValues[i] == fps)
                fpsIndex = int(i);
        HWND fpsCombo = Combo(kFps, 140, 28, 186,
                              { L"30 (original)", L"60", L"120", L"144", L"165", L"240", L"360", L"Match monitor" }, 0);
        if (fpsIndex < 0)
        {
            // A custom rate from wet.ini: keep it as its own choice.
            std::wstring text = std::to_wstring(fps) + L" (wet.ini)";
            SendMessageW(fpsCombo, CB_ADDSTRING, 0, LPARAM(text.c_str()));
            s_fpsValues.push_back(fps);
            fpsIndex = int(s_fpsValues.size()) - 1;
        }
        SendMessageW(fpsCombo, CB_SETCURSEL, WPARAM(fpsIndex), 0);
        Label(L"Resolution", 22, 60);
        Combo(kResolution, 140, 58, 186, { L"720p (original)", L"1440p", L"2160p (4K)" },
              std::min(std::max(ini.Get("resolution_scale", 1), 1), 3) - 1);
        Check(kFullscreen, L"Fullscreen", 22, 90, ini.Get("fullscreen", 0) != 0, 140);
        Check(kShowFps, L"Show FPS counter", 170, 90, ini.Get("show_fps", 0) != 0, 160);
        Check(kVideoMode, L"Videos at 30 FPS (avoids video flicker)", 22, 114, ini.Get("video_mode", 1) != 0, 300);
        Check(kWidescreen, L"Widescreen / ultrawide (fill 21:9 and 32:9 screens)", 22, 140, ini.Get("widescreen", 0) != 0, 310);

        // Graphics
        Group(L"Graphics", 350, 8, 330, 164 + kEffectsRow);
        Label(L"Scaling", 362, 30, 90);
        Combo(kFilter, 460, 28, 206, { L"Bilinear", L"AMD CAS (sharpen)", L"AMD FSR (upscale)", L"NVIDIA NIS (upscale)" },
              std::min(std::max(ini.Get("filter", 0), 0), 3));
        Label(L"NIS sharpness", 362, 60, 90);
        Slider(kSharpness, kSharpnessValue, 456, 58, 160, 0, 100, ini.Get("sharpness", 50), L"%");
        Label(L"Anti-aliasing", 362, 88, 90);
        Combo(kAntialiasing, 460, 86, 206, { L"Off", L"FXAA", L"FXAA (extreme)", L"SMAA (sharper)" },
              std::min(std::max(ini.Get("antialiasing", 0), 0), 3));
        Label(L"Film grain", 362, 116, 90);
        Slider(kGrain, kGrainValue, 456, 114, 160, 0, 100, ini.Get("film_grain", 100), L"%");
        Label(L"Field of view", 362, 144, 90);
        Slider(kFov, kFovValue, 456, 142, 160, 60, 130, std::min(std::max(ini.Get("fov", 90), 60), 130), L"\u00B0");
        {
            static const int kLanguageIds[] = { 0, 1, 3, 4, 5, 6 };
            int lang = ini.Get("language", 0), pick = 0;
            for (int i = 0; i < 6; i++)
                if (kLanguageIds[i] == lang)
                    pick = i;
            Label(L"Language", 22, 170);
            Combo(kLanguage, 140, 168, 186,
                  { L"Automatic (Windows)", L"English", L"Deutsch", L"Fran\u00E7ais", L"Espa\u00F1ol", L"Italiano" }, pick);
        }
        Check(kMotionBlur, L"Motion blur", 362, 170, ini.Get("motion_blur", 1) != 0, 96);
        Check(kDepthOfField, L"Depth of field", 462, 170, ini.Get("depth_of_field", 1) != 0, 104);
        Check(kCameraShake, L"Camera shake", 570, 170, ini.Get("camera_shake", 1) != 0, 104);

        // Controls
        Group(L"Controls", 10, 180, 330, 134);
        Check(kMouseAim, L"Mouse aim (WASD + mouse)", 22, 200, ini.Get("mouse_aim", 1) != 0, 200);
        Check(kInvertY, L"Invert Y", 230, 200, ini.Get("mouse_invert_y", 0) != 0, 100);
        Label(L"Mouse sensitivity", 22, 230);
        Slider(kSensitivity, kSensitivityValue, 136, 228, 150, 1, 200, ini.Get("mouse_sensitivity", 50), L"");
        Check(kVibration, L"Controller vibration", 22, 256, ini.Get("vibration", 1) != 0, 180);
        Label(L"Prompts", 22, 284, 56);
        Combo(kPrompts, 80, 282, 128, { L"Automatic", L"Xbox", L"PlayStation", L"Keyboard / mouse" },
              std::min(std::max(ini.Get("button_prompts", 0), 0), 3));
        Make(L"BUTTON", L"Keyboard...", BS_PUSHBUTTON | WS_TABSTOP, 216, 278, 110, 26, kKeyboard);

        // Other
        Group(L"Other", 350, 180, 330, 134);
        Check(kShaderCache, L"Shader cache (less stutter after the first time)", 362, 196, ini.Get("shader_cache", 1) != 0, 310);
        Check(kAsyncShaders, L"Don't wait for new shaders (objects may pop in)", 362, 216, ini.Get("async_shaders", 1) != 0, 310);
        Check(kShaderPreload, L"Build all cached shaders before starting (slower start)", 362, 236, ini.Get("shader_preload", 0) != 0, 310);
        Check(kPopups, L"Achievement pop-ups", 362, 256, ini.Get("achievement_popups", 1) != 0, 200);
        Make(L"BUTTON", L"Saves \u25BE", BS_PUSHBUTTON | WS_TABSTOP, 362, 278, 145, 26, kSaves);
        Make(L"BUTTON", L"Clean up \u25BE", BS_PUSHBUTTON | WS_TABSTOP, 517, 278, 150, 26, kClearShaders);

        // Screen
        Group(L"Screen", 10, 322, 330, 122);
        Check(kVsync, L"V-Sync: no tearing (off for G-Sync / FreeSync)", 22, 342, ini.Get("vsync", 0) != 0, 310);
        Check(kHideBorders, L"Hide the dark frame at the screen edges", 22, 364, ini.Get("hide_borders", 0) != 0, 310);
        Check(kWideHud, L"Widescreen: keep the HUD 16:9 in the middle", 22, 386, ini.Get("widescreen_hud", 1) != 0, 310);
        Label(L"Monitor", 22, 414, 56);
        FillMonitors(Make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 80, 412, 246, 200, kMonitor),
                     ini.GetString("monitor", ""));

        // Help
        Group(L"Help", 350, 322, 330, 122);
        Make(L"BUTTON", L"Check for updates", BS_PUSHBUTTON | WS_TABSTOP, 362, 342, 145, 26, kCheckUpdates);
        Make(L"BUTTON", L"Report a bug...", BS_PUSHBUTTON | WS_TABSTOP, 517, 342, 150, 26, kReportBug);
        Make(L"BUTTON", L"Controller test...", BS_PUSHBUTTON | WS_TABSTOP, 362, 372, 145, 26, kPadTest);
        Make(L"BUTTON", L"Achievements...", BS_PUSHBUTTON | WS_TABSTOP, 517, 372, 150, 26, kAchievements);
        Make(L"STATIC", L"", SS_LEFT, 362, 406, 305, 32, kUpdateText);

        Make(L"STATIC", L"", SS_LEFT | SS_PATHELLIPSIS, 12, 458, 540, 18, kStatus);
        Make(L"BUTTON", L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, 570, 452, 110, 32, kPlay);
        UpdateStatus();
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_HSCROLL:
            if (HWND(lp) == Item(kSharpness)) UpdateValueLabel(kSharpness, kSharpnessValue, L"%");
            else if (HWND(lp) == Item(kGrain)) UpdateValueLabel(kGrain, kGrainValue, L"%");
            else if (HWND(lp) == Item(kFov)) UpdateValueLabel(kFov, kFovValue, L"\u00B0");
            else if (HWND(lp) == Item(kSensitivity)) UpdateValueLabel(kSensitivity, kSensitivityValue, L"");
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp))
            {
            case kPlay: Play(); return 0;
            case kSaves:
                switch (ButtonMenu(kSaves, { { kMenuOpenSaves, L"Open the saves folder" },
                                             { 0, nullptr },
                                             { kMenuBackup, L"Back up saves and achievements..." },
                                             { kMenuImportZip, L"Import saves from a backup (.zip)..." },
                                             { kMenuImportFolder, L"Import saves from a folder (Xenia, another PC)..." } }))
                {
                case kMenuOpenSaves: OpenSaves(); break;
                case kMenuBackup: BackupSaves(); break;
                case kMenuImportZip: ImportSaves(true); break;
                case kMenuImportFolder: ImportSaves(false); break;
                }
                return 0;
            case kClearShaders:
                switch (ButtonMenu(kClearShaders, { { kMenuFreeSpace, L"Free up disk space..." },
                                                    { kMenuClearShaders, L"Clear the shader cache..." } }))
                {
                case kMenuFreeSpace: FreeSpace(); break;
                case kMenuClearShaders: ClearShaders(); break;
                }
                return 0;
            case kKeyboard: OpenKeys(); return 0;
            case kCheckUpdates: CheckForUpdates(); return 0;
            case kReportBug: ReportBug(); return 0;
            case kPadTest: OpenPadTest(); return 0;
            case kAchievements: OpenAchievements(); return 0;
            case IDCANCEL: DestroyWindow(hwnd); return 0;
            }
            break;
        case WM_CTLCOLORSTATIC:
            SetBkMode(HDC(wp), TRANSPARENT);
            return LRESULT(GetSysColorBrush(COLOR_WINDOW));
        case WM_PAINT:
            PaintBanner(hwnd);
            return 0;
        case kUpdateResult:
        {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
            SetWindowTextW(Item(kUpdateText), text->c_str());
            EnableWindow(Item(kCheckUpdates), TRUE);
            if (s_updateAvailable)
                SetWindowTextW(Item(kCheckUpdates), L"Update now");
            return 0;
        }
        case WM_CLOSE:
            SaveSettings();   // keep changes even without playing
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    s_dir = fs::path(self).parent_path();
    s_iniPath = s_dir / "wet.ini";
    s_gameDir = FindGame();
    s_hddDir = s_gameDir.empty() ? s_dir / "game" / "hdd" : s_gameDir.parent_path() / "hdd";
    // First start (no wet.ini yet): resolution and anti-aliasing to suit the
    // graphics card, rather than the same for every PC.
    std::wstring firstStartNote;
    if (!fs::exists(s_iniPath))
    {
        wet::gpu_info::Info gpu = wet::gpu_info::Query();
        wet::gpu_info::Recommended r = wet::gpu_info::Recommend(gpu);
        s_ini.Set("resolution_scale", r.scale);
        s_ini.Set("antialiasing", r.aa);
        s_ini.Set("filter", r.filter);
        if (gpu.found)
            firstStartNote = L"Settings picked for " + Wide(gpu.name) + L" (" + Wide(r.tier) +
                             L"); change them here any time.";
    }
    s_ini.Load(s_iniPath);

    HDC screen = GetDC(nullptr);
    s_scale = GetDeviceCaps(screen, LOGPIXELSY) / 96.0;
    ReleaseDC(nullptr, screen);
    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    s_font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"WetLauncher";
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    WNDCLASSEXW kc = wc;
    kc.lpfnWndProc = KeysProc;
    kc.lpszClassName = L"WetLauncherKeys";
    RegisterClassExW(&kc);
    WNDCLASSEXW tc = wc;
    tc.lpfnWndProc = ToolProc;
    tc.lpszClassName = L"WetLauncherTool";
    RegisterClassExW(&tc);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LoadBanner();
    RECT rc{ 0, 0, S(690), S(494 + kEffectsRow + s_bannerHeight) };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rc, style, FALSE);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    std::wstring title = L"WET Recomp " + std::wstring(WET_VERSION, WET_VERSION + std::strlen(WET_VERSION));
    s_window = CreateWindowExW(0, wc.lpszClassName, title.c_str(), style,
                               (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
                               nullptr, nullptr, instance, nullptr);
    s_parent = s_window;
    BuildControls();
    if (!firstStartNote.empty())
        SetWindowTextW(Item(kUpdateText), firstStartNote.c_str());
    ShowWindow(s_window, show);
    SetFocus(Item(kPlay));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        if (!(s_keysWindow && IsDialogMessageW(s_keysWindow, &msg)) && !IsDialogMessageW(s_window, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
