// WET Launcher: a small settings window that edits wet.ini (the same file the
// in-game F1 overlay uses) and starts WetRecomp.exe. Plain Win32 controls, no
// dependencies. Keys it doesn't know are kept as they are.
#ifndef UNICODE
#define UNICODE
#endif
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
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

        int Get(const std::string& key, int fallback) const
        {
            auto it = values.find(key);
            return it == values.end() || it->second.empty() ? fallback : std::atoi(it->second.c_str());
        }

        void Set(const std::string& key, int value)
        {
            std::string v = std::to_string(value);
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
        kFilter, kSharpness, kSharpnessValue, kAntialiasing, kGrain, kGrainValue,
        kMouseAim, kSensitivity, kSensitivityValue, kInvertY, kVibration,
        kShaderCache, kVideoMode, kPopups,
        kStatus, kPlay, kSaves, kClearShaders,
    };

    HWND s_window = nullptr;
    HFONT s_font = nullptr;
    double s_scale = 1.0;
    fs::path s_dir, s_iniPath, s_gameDir, s_hddDir;
    Ini s_ini;

    const int kFpsValues[] = { 30, 60, 120, 144 };

    int S(int v) { return int(v * s_scale + 0.5); }

    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id)
    {
        HWND h_ = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), s_window,
                                  reinterpret_cast<HMENU>(intptr_t(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(h_, WM_SETFONT, WPARAM(s_font), TRUE);
        return h_;
    }

    HWND Item(int id) { return GetDlgItem(s_window, id); }

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

    void SaveSettings()
    {
        int fps = Selected(kFps);
        s_ini.Set("fps", kFpsValues[fps < 0 ? 0 : fps]);
        s_ini.Set("resolution_scale", Selected(kResolution) + 1);
        s_ini.Set("fullscreen", Checked(kFullscreen));
        s_ini.Set("show_fps", Checked(kShowFps));
        s_ini.Set("filter", Selected(kFilter));
        s_ini.Set("sharpness", Pos(kSharpness));
        s_ini.Set("antialiasing", Selected(kAntialiasing));
        s_ini.Set("film_grain", Pos(kGrain));
        s_ini.Set("mouse_aim", Checked(kMouseAim));
        s_ini.Set("mouse_sensitivity", Pos(kSensitivity));
        s_ini.Set("mouse_invert_y", Checked(kInvertY));
        s_ini.Set("vibration", Checked(kVibration));
        s_ini.Set("shader_cache", Checked(kShaderCache));
        s_ini.Set("video_mode", Checked(kVideoMode));
        s_ini.Set("achievement_popups", Checked(kPopups));
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

    void BuildControls()
    {
        const Ini& ini = s_ini;
        // Display
        Group(L"Display", 10, 8, 330, 136);
        Label(L"Frame rate", 22, 30);
        int fps = ini.Get("fps", 30), fpsIndex = 0;
        for (int i = 0; i < 4; i++)
            if (kFpsValues[i] == fps)
                fpsIndex = i;
        Combo(kFps, 140, 28, 186, { L"30 (original)", L"60", L"120", L"144" }, fpsIndex);
        Label(L"Resolution", 22, 60);
        Combo(kResolution, 140, 58, 186, { L"720p (original)", L"1440p", L"2160p (4K)" },
              std::min(std::max(ini.Get("resolution_scale", 1), 1), 3) - 1);
        Check(kFullscreen, L"Fullscreen", 22, 90, ini.Get("fullscreen", 0) != 0, 140);
        Check(kShowFps, L"Show FPS counter", 170, 90, ini.Get("show_fps", 0) != 0, 160);
        Check(kVideoMode, L"Videos at 30 FPS (avoids video flicker)", 22, 114, ini.Get("video_mode", 1) != 0, 300);

        // Graphics
        Group(L"Graphics", 350, 8, 330, 136);
        Label(L"Scaling", 362, 30, 90);
        Combo(kFilter, 460, 28, 206, { L"Bilinear", L"AMD CAS (sharpen)", L"AMD FSR (upscale)", L"NVIDIA NIS (upscale)" },
              std::min(std::max(ini.Get("filter", 0), 0), 3));
        Label(L"NIS sharpness", 362, 60, 90);
        Slider(kSharpness, kSharpnessValue, 456, 58, 160, 0, 100, ini.Get("sharpness", 50), L"%");
        Label(L"Anti-aliasing", 362, 88, 90);
        Combo(kAntialiasing, 460, 86, 206, { L"Off", L"FXAA", L"FXAA (extreme)" },
              std::min(std::max(ini.Get("antialiasing", 0), 0), 2));
        Label(L"Film grain", 362, 116, 90);
        Slider(kGrain, kGrainValue, 456, 114, 160, 0, 100, ini.Get("film_grain", 100), L"%");

        // Controls
        Group(L"Controls", 10, 152, 330, 112);
        Check(kMouseAim, L"Mouse aim (WASD + mouse)", 22, 172, ini.Get("mouse_aim", 1) != 0, 200);
        Check(kInvertY, L"Invert Y", 230, 172, ini.Get("mouse_invert_y", 0) != 0, 100);
        Label(L"Mouse sensitivity", 22, 202);
        Slider(kSensitivity, kSensitivityValue, 136, 200, 150, 1, 100, ini.Get("mouse_sensitivity", 50), L"");
        Check(kVibration, L"Controller vibration", 22, 234, ini.Get("vibration", 1) != 0, 200);

        // Other
        Group(L"Other", 350, 152, 330, 112);
        Check(kShaderCache, L"Shader cache (less stutter after the first time)", 362, 172, ini.Get("shader_cache", 1) != 0, 310);
        Check(kPopups, L"Achievement pop-ups", 362, 198, ini.Get("achievement_popups", 1) != 0, 200);
        Make(L"BUTTON", L"Open saves folder", BS_PUSHBUTTON | WS_TABSTOP, 362, 228, 145, 26, kSaves);
        Make(L"BUTTON", L"Clear shader cache", BS_PUSHBUTTON | WS_TABSTOP, 517, 228, 150, 26, kClearShaders);

        Make(L"STATIC", L"", SS_LEFT | SS_PATHELLIPSIS, 12, 280, 540, 18, kStatus);
        Make(L"BUTTON", L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, 570, 274, 110, 32, kPlay);
        UpdateStatus();
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_HSCROLL:
            if (HWND(lp) == Item(kSharpness)) UpdateValueLabel(kSharpness, kSharpnessValue, L"%");
            else if (HWND(lp) == Item(kGrain)) UpdateValueLabel(kGrain, kGrainValue, L"%");
            else if (HWND(lp) == Item(kSensitivity)) UpdateValueLabel(kSensitivity, kSensitivityValue, L"");
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp))
            {
            case kPlay: Play(); return 0;
            case kSaves: OpenSaves(); return 0;
            case kClearShaders: ClearShaders(); return 0;
            case IDCANCEL: DestroyWindow(hwnd); return 0;
            }
            break;
        case WM_CTLCOLORSTATIC:
            SetBkMode(HDC(wp), TRANSPARENT);
            return LRESULT(GetSysColorBrush(COLOR_WINDOW));
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
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    s_dir = fs::path(self).parent_path();
    s_iniPath = s_dir / "wet.ini";
    s_gameDir = FindGame();
    s_hddDir = s_gameDir.empty() ? s_dir / "game" / "hdd" : s_gameDir.parent_path() / "hdd";
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

    RECT rc{ 0, 0, S(690), S(316) };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rc, style, FALSE);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    s_window = CreateWindowExW(0, wc.lpszClassName, L"WET Recomp", style,
                               (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
                               nullptr, nullptr, instance, nullptr);
    BuildControls();
    ShowWindow(s_window, show);
    SetFocus(Item(kPlay));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        if (!IsDialogMessageW(s_window, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
