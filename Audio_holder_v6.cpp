// Audio Holder v6 - native C++ / Win32 version (no dependencies, no runtime needed)
//
// Logic:
//   Runs in the tray and waits for cs2.exe. When it appears, the chosen audio device is
//   kept busy with an inaudible looping stream for 20 seconds, then released. After that
//   the app rests until cs2.exe has closed, and then goes back to waiting for the next start.
//
// Optional settings in %APPDATA%\AudioDeviceBeeper\config.txt (plain text, key=value):
//   process=cs2            comma separated list of process names that trigger the hold
//   hold=20                seconds to hold the device after the trigger process appears
//
// Build with MSVC (see build.yml for GitHub Actions):
//   cl /nologo /O1 /Os /GS- /MT /EHsc Audio_holder_v6.cpp /link /SUBSYSTEM:WINDOWS

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdlib.h>
#include <wchar.h>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
// modern look for buttons / combo box
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

// config folder and autostart entry keep the name used by the v5.x builds,
// so the saved device carries over and the old autostart entry is replaced
static const wchar_t* kAppName   = L"AudioDeviceBeeper";
static const wchar_t* kRunKey    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kClassName = L"AudioHolderV6Window";
static const wchar_t* kMutexName = L"AudioHolder_v6_SingleInstance";

enum
{
    IDC_COMBO = 101,
    IDC_WATCH = 102,
    IDC_AUTO = 103,
    ID_TRAY_WATCH = 201,
    ID_TRAY_SHOW = 202,
    ID_TRAY_EXIT = 203,
    IDT_TICK = 1
};

static const UINT WM_TRAY = WM_APP + 1;
static const int kSampleRate = 22050;

enum State { ST_WAIT, ST_HOLD, ST_REST };

// ---------- globals ----------
static HINSTANCE g_inst = NULL;
static HWND g_wnd = NULL;
static HWND g_combo = NULL;
static HWND g_watchCheck = NULL;
static HWND g_autoCheck = NULL;
static HWND g_status = NULL;
static HFONT g_font = NULL;
static HICON g_icon = NULL;
static NOTIFYICONDATAW g_nid;
static UINT g_msgTaskbar = 0;
static bool g_exiting = false;

static bool g_watch = true;
static bool g_autostart = true;
static std::wstring g_device;
static std::wstring g_processText = L"cs2";
static int g_holdSeconds = 20;
static std::vector<std::wstring> g_targets;

static State g_state = ST_WAIT;
static ULONGLONG g_since = 0;
static std::wstring g_lastStatus;
static std::wstring g_lastTip;

static HWAVEOUT g_hwo = NULL;
static WAVEHDR g_hdr;
static short g_quiet[kSampleRate];

// ---------- small helpers ----------
static std::wstring Utf8ToW(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string WToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

static bool IsSpaceChar(wchar_t c)
{
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

static std::wstring Trim(const std::wstring& s)
{
    size_t a = 0;
    size_t b = s.size();
    while (a < b && IsSpaceChar(s[a])) a++;
    while (b > a && IsSpaceChar(s[b - 1])) b--;
    return s.substr(a, b - a);
}

// "cs2, other.exe" -> { "cs2.exe", "other.exe" }
static void ParseTargets(const std::wstring& list)
{
    g_targets.clear();
    size_t start = 0;
    while (start <= list.size())
    {
        size_t comma = list.find(L',', start);
        if (comma == std::wstring::npos) comma = list.size();
        std::wstring name = Trim(list.substr(start, comma - start));
        if (!name.empty())
        {
            CharLowerBuffW(&name[0], (DWORD)name.size());
            if (name.size() < 4 || name.compare(name.size() - 4, 4, L".exe") != 0) name += L".exe";
            g_targets.push_back(name);
        }
        start = comma + 1;
    }
    if (g_targets.empty()) g_targets.push_back(L"cs2.exe");
}

static std::wstring TargetsLabel()
{
    std::wstring s;
    for (size_t i = 0; i < g_targets.size(); ++i)
    {
        if (i > 0) s += L", ";
        s += g_targets[i];
    }
    return s;
}

// ---------- config ----------
static std::wstring ConfigDir()
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L".");
    return dir + L"\\" + kAppName;
}

static void LoadConfig()
{
    std::wstring path = ConfigDir() + L"\\config.txt";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;

    DWORD size = GetFileSize(f, NULL);
    std::string data;
    if (size != INVALID_FILE_SIZE && size < 65536)
    {
        data.resize(size);
        DWORD got = 0;
        if (size > 0 && !ReadFile(f, &data[0], size, &got, NULL)) got = 0;
        data.resize(got);
    }
    CloseHandle(f);

    std::wstring text = Utf8ToW(data);
    if (!text.empty() && text[0] == 0xFEFF) text.erase(0, 1);

    size_t pos = 0;
    while (pos < text.size())
    {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line[line.size() - 1] == L'\r') line.erase(line.size() - 1);

        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || eq == 0) continue;
        std::wstring key = line.substr(0, eq);
        std::wstring val = line.substr(eq + 1);

        if (key == L"device") g_device = val;              // not trimmed: names can end in a space
        else if (key == L"autostart") g_autostart = (Trim(val) == L"1");
        else if (key == L"watch") g_watch = (Trim(val) == L"1");
        else if (key == L"process") g_processText = Trim(val);
        else if (key == L"hold")
        {
            int h = _wtoi(Trim(val).c_str());
            if (h >= 1 && h <= 600) g_holdSeconds = h;
        }
    }
}

static void SaveConfig()
{
    std::wstring dir = ConfigDir();
    CreateDirectoryW(dir.c_str(), NULL);

    std::wstring text = L"device=" + g_device + L"\n";
    text += std::wstring(L"autostart=") + (g_autostart ? L"1" : L"0") + L"\n";
    text += std::wstring(L"watch=") + (g_watch ? L"1" : L"0") + L"\n";
    text += L"process=" + g_processText + L"\n";
    text += L"hold=" + std::to_wstring(g_holdSeconds) + L"\n";
    std::string utf8 = WToUtf8(text);

    std::wstring path = dir + L"\\config.txt";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(f, utf8.data(), (DWORD)utf8.size(), &wrote, NULL);
    CloseHandle(f);
}

// ---------- autostart ----------
static void SetAutostart(bool enable)
{
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (enable)
    {
        wchar_t exe[MAX_PATH];
        DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe, n) + L"\" --minimized";
        RegSetValueExW(key, kAppName, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    }
    else
    {
        RegDeleteValueW(key, kAppName);
    }
    RegCloseKey(key);
}

// ---------- audio devices ----------
static std::vector<std::wstring> GetDevices()
{
    std::vector<std::wstring> names;
    UINT n = waveOutGetNumDevs();
    for (UINT i = 0; i < n; ++i)
    {
        WAVEOUTCAPSW caps;
        ZeroMemory(&caps, sizeof(caps));
        if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            names.push_back(std::wstring(caps.szPname));
        else
            names.push_back(L"Device " + std::to_wstring((int)i));
    }
    return names;
}

static int FindDevice(const std::wstring& name)
{
    std::vector<std::wstring> names = GetDevices();
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (names[i] == name) return (int)i;
    }
    return -1;
}

static void FillCombo()
{
    std::vector<std::wstring> names = GetDevices();
    SendMessageW(g_combo, CB_RESETCONTENT, 0, 0);
    int sel = -1;
    for (size_t i = 0; i < names.size(); ++i)
    {
        SendMessageW(g_combo, CB_ADDSTRING, 0, (LPARAM)names[i].c_str());
        if (names[i] == g_device) sel = (int)i;
    }
    if (sel < 0 && g_device.empty() && !names.empty())
    {
        sel = 0; // first run: pick the first device
        g_device = names[0];
    }
    if (sel >= 0) SendMessageW(g_combo, CB_SETCURSEL, (WPARAM)sel, 0);
}

// ---------- holding the device ----------
// 1 second of inaudible near-silence (values -1, 0, +1), looped
static void FillQuiet()
{
    unsigned int seed = 12345u;
    for (int i = 0; i < kSampleRate; ++i)
    {
        seed = seed * 1103515245u + 12345u;
        g_quiet[i] = (short)((int)((seed >> 16) % 3u) - 1);
    }
}

static void StopHold()
{
    if (g_hwo)
    {
        waveOutReset(g_hwo);
        waveOutUnprepareHeader(g_hwo, &g_hdr, sizeof(g_hdr));
        waveOutClose(g_hwo);
        g_hwo = NULL;
    }
}

static bool StartHold()
{
    if (g_device.empty()) return false;

    // look the device up by name, the index can change
    int dev = FindDevice(g_device);
    if (dev < 0) return false;

    WAVEFORMATEX fmt;
    ZeroMemory(&fmt, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = kSampleRate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = kSampleRate * 2;

    ZeroMemory(&g_hdr, sizeof(g_hdr));
    HWAVEOUT h = NULL;
    if (waveOutOpen(&h, (UINT)dev, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return false;
    g_hwo = h;

    FillQuiet();
    g_hdr.lpData = (LPSTR)g_quiet;
    g_hdr.dwBufferLength = (DWORD)sizeof(g_quiet);

    if (waveOutPrepareHeader(g_hwo, &g_hdr, sizeof(g_hdr)) != MMSYSERR_NOERROR)
    {
        StopHold();
        return false;
    }
    // set the loop flags after preparing the header
    g_hdr.dwFlags |= WHDR_BEGINLOOP | WHDR_ENDLOOP;
    g_hdr.dwLoops = 0x7FFFFFFF;
    if (waveOutWrite(g_hwo, &g_hdr, sizeof(g_hdr)) != MMSYSERR_NOERROR)
    {
        StopHold();
        return false;
    }
    return true;
}

// make sure the device is held; reopen it if it vanished or the stream ended
static void EnsureHold()
{
    if (g_hwo)
    {
        bool done = (*(volatile DWORD*)&g_hdr.dwFlags & WHDR_DONE) != 0;
        if (!done && FindDevice(g_device) >= 0) return;
        StopHold();
    }
    if (!StartHold())
    {
        // device not there (yet): refresh the list if it changed
        if ((int)GetDevices().size() != (int)SendMessageW(g_combo, CB_GETCOUNT, 0, 0)) FillCombo();
    }
}

// ---------- game detection ----------
static bool IsGameRunning()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);

    bool found = false;
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            for (size_t i = 0; i < g_targets.size(); ++i)
            {
                if (lstrcmpiW(pe.szExeFile, g_targets[i].c_str()) == 0)
                {
                    found = true;
                    break;
                }
            }
        } while (!found && Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// ---------- state machine ----------
static void UpdateStatus()
{
    std::wstring text;
    const wchar_t* shortState = L"waiting";

    if (!g_watch)
    {
        text = L"Status: watching is off";
        shortState = L"off";
    }
    else if (g_state == ST_WAIT)
    {
        text = L"Status: waiting for " + TargetsLabel();
        shortState = L"waiting";
    }
    else if (g_state == ST_HOLD)
    {
        long left = g_holdSeconds - (long)((GetTickCount64() - g_since) / 1000);
        if (left < 0) left = 0;
        if (g_hwo)
            text = L"Status: game detected - holding \"" + g_device + L"\" (" + std::to_wstring(left) + L" s left)";
        else
            text = L"Status: game detected - waiting for the device...";
        shortState = L"holding";
    }
    else
    {
        text = L"Status: game running - device released";
        shortState = L"resting";
    }

    if (g_status && text != g_lastStatus)
    {
        g_lastStatus = text;
        SetWindowTextW(g_status, text.c_str());
    }

    std::wstring tip = std::wstring(L"Audio Holder v6 - ") + shortState;
    if (g_nid.hWnd && tip != g_lastTip)
    {
        g_lastTip = tip;
        lstrcpynW(g_nid.szTip, tip.c_str(), 128);
        g_nid.uFlags = NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    }
}

// at startup / when watching is switched on: if the game is already running, just rest
static void ResetState()
{
    StopHold();
    g_state = (g_watch && IsGameRunning()) ? ST_REST : ST_WAIT;
}

static void OnTick()
{
    if (g_exiting) return;

    if (!g_watch)
    {
        StopHold();
        g_state = ST_WAIT;
        UpdateStatus();
        return;
    }

    bool running = IsGameRunning();

    if (g_state == ST_WAIT)
    {
        if (running)
        {
            g_state = ST_HOLD;
            g_since = GetTickCount64();
            EnsureHold();
        }
    }
    else if (g_state == ST_HOLD)
    {
        if (!running)
        {
            StopHold();            // game closed again during the hold window
            g_state = ST_WAIT;
        }
        else if ((GetTickCount64() - g_since) / 1000 >= (ULONGLONG)g_holdSeconds)
        {
            StopHold();            // the game has its own audio stream now
            g_state = ST_REST;
        }
        else
        {
            EnsureHold();
        }
    }
    else // ST_REST
    {
        if (!running) g_state = ST_WAIT;
    }

    UpdateStatus();
}

static void SetWatch(bool on)
{
    g_watch = on;
    SendMessageW(g_watchCheck, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    SaveConfig();
    ResetState();
    OnTick();
}

// ---------- UI ----------
static HICON MakeIcon()
{
    const int S = 32;
    HDC screen = GetDC(NULL);
    HDC dcColor = CreateCompatibleDC(screen);
    HDC dcMask = CreateCompatibleDC(screen);
    HBITMAP bmColor = CreateCompatibleBitmap(screen, S, S);
    HBITMAP bmMask = CreateBitmap(S, S, 1, 1, NULL);
    HGDIOBJ oldColor = SelectObject(dcColor, bmColor);
    HGDIOBJ oldMask = SelectObject(dcMask, bmMask);

    RECT r = { 0, 0, S, S };

    // mask: white = transparent, black = opaque
    FillRect(dcMask, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
    HGDIOBJ oldMB = SelectObject(dcMask, GetStockObject(BLACK_BRUSH));
    HGDIOBJ oldMP = SelectObject(dcMask, GetStockObject(BLACK_PEN));
    Ellipse(dcMask, 1, 1, 31, 31);
    SelectObject(dcMask, oldMB);
    SelectObject(dcMask, oldMP);

    // colour image: blue disc with a white triangle
    FillRect(dcColor, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    HBRUSH blue = CreateSolidBrush(RGB(40, 110, 220));
    HPEN bluePen = CreatePen(PS_SOLID, 1, RGB(40, 110, 220));
    HGDIOBJ oldB = SelectObject(dcColor, blue);
    HGDIOBJ oldP = SelectObject(dcColor, bluePen);
    Ellipse(dcColor, 1, 1, 31, 31);
    SelectObject(dcColor, GetStockObject(WHITE_BRUSH));
    SelectObject(dcColor, GetStockObject(WHITE_PEN));
    POINT tri[3] = { { 12, 9 }, { 12, 23 }, { 23, 16 } };
    Polygon(dcColor, tri, 3);
    SelectObject(dcColor, oldB);
    SelectObject(dcColor, oldP);
    DeleteObject(blue);
    DeleteObject(bluePen);

    SelectObject(dcColor, oldColor);
    SelectObject(dcMask, oldMask);

    ICONINFO ii;
    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmMask = bmMask;
    ii.hbmColor = bmColor;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(bmColor);
    DeleteObject(bmMask);
    DeleteDC(dcColor);
    DeleteDC(dcMask);
    ReleaseDC(NULL, screen);
    return icon;
}

static HWND MakeChild(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
                      int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                             parent, (HMENU)(INT_PTR)id, g_inst, NULL);
    if (c && g_font) SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static void ShowMain()
{
    ShowWindow(g_wnd, SW_SHOW);
    ShowWindow(g_wnd, SW_RESTORE);
    SetForegroundWindow(g_wnd);
}

static void ShowTrayMenu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g_watch ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_WATCH, L"Watch for game");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, L"Show");
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static void OnDeviceSelected()
{
    int sel = (int)SendMessageW(g_combo, CB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    int len = (int)SendMessageW(g_combo, CB_GETLBTEXTLEN, (WPARAM)sel, 0);
    if (len <= 0) return;
    std::wstring name((size_t)len + 1, L'\0');
    SendMessageW(g_combo, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)&name[0]);
    name.resize((size_t)len);

    g_device = name;
    SaveConfig();
    StopHold();   // reopened on the next tick if a hold is due
    OnTick();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (g_msgTaskbar != 0 && msg == g_msgTaskbar)
    {
        // Explorer restarted: put the tray icon back
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        return 0;
    }

    switch (msg)
    {
    case WM_CREATE:
    {
        NONCLIENTMETRICSW ncm;
        ZeroMemory(&ncm, sizeof(ncm));
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
            g_font = CreateFontIndirectW(&ncm.lfMessageFont);

        std::wstring watchText = L"Watch for " + TargetsLabel() + L" and hold the device for "
                                 + std::to_wstring(g_holdSeconds) + L" s";

        MakeChild(hwnd, L"STATIC", L"Audio output device:", 0, 16, 14, 408, 18, 0);
        g_combo = MakeChild(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                            16, 36, 408, 200, IDC_COMBO);
        g_watchCheck = MakeChild(hwnd, L"BUTTON", watchText.c_str(), BS_AUTOCHECKBOX | WS_TABSTOP,
                                 16, 76, 408, 20, IDC_WATCH);
        g_autoCheck = MakeChild(hwnd, L"BUTTON", L"Start with Windows", BS_AUTOCHECKBOX | WS_TABSTOP,
                                16, 102, 408, 20, IDC_AUTO);
        g_status = MakeChild(hwnd, L"STATIC", L"", SS_ENDELLIPSIS, 16, 132, 408, 20, 0);

        SendMessageW(g_watchCheck, BM_SETCHECK, g_watch ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g_autoCheck, BM_SETCHECK, g_autostart ? BST_CHECKED : BST_UNCHECKED, 0);

        FillCombo();

        // tray icon
        ZeroMemory(&g_nid, sizeof(g_nid));
        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = hwnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAY;
        g_nid.hIcon = g_icon;
        lstrcpynW(g_nid.szTip, L"Audio Holder v6", 128);
        Shell_NotifyIconW(NIM_ADD, &g_nid);

        // keep registry in sync (creates the entry on first run)
        SetAutostart(g_autostart);
        SaveConfig();

        ResetState();
        OnTick();
        SetTimer(hwnd, IDT_TICK, 1000, NULL);
        return 0;
    }

    case WM_TIMER:
        if (wParam == IDT_TICK) OnTick();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_COMBO:
            if (HIWORD(wParam) == CBN_SELCHANGE) OnDeviceSelected();
            break;
        case IDC_WATCH:
            if (HIWORD(wParam) == BN_CLICKED)
                SetWatch(SendMessageW(g_watchCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
            break;
        case IDC_AUTO:
            if (HIWORD(wParam) == BN_CLICKED)
            {
                g_autostart = (SendMessageW(g_autoCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
                SetAutostart(g_autostart);
                SaveConfig();
            }
            break;
        case ID_TRAY_WATCH:
            SetWatch(!g_watch);
            break;
        case ID_TRAY_SHOW:
            ShowMain();
            break;
        case ID_TRAY_EXIT:
            g_exiting = true;
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_TRAY:
        if (lParam == WM_LBUTTONUP) ShowMain();
        else if (lParam == WM_RBUTTONUP) ShowTrayMenu(hwnd);
        return 0;

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) ShowWindow(hwnd, SW_HIDE);   // minimize -> tray
        return 0;

    case WM_CLOSE:
        if (!g_exiting)
        {
            ShowWindow(hwnd, SW_HIDE);                              // X button -> tray
            return 0;
        }
        break;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_TICK);
        StopHold();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    HANDLE mutex = CreateMutexW(NULL, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) return 0;   // already running (in the tray)

    g_inst = hInst;
    LoadConfig();
    ParseTargets(g_processText);

    bool startHidden = (wcsstr(GetCommandLineW(), L"--minimized") != NULL);

    g_icon = MakeIcon();
    g_msgTaskbar = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    wc.hIcon = g_icon;
    wc.hIconSm = g_icon;
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = { 0, 0, 440, 165 };
    AdjustWindowRect(&rc, style, FALSE);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    g_wnd = CreateWindowExW(0, kClassName, L"Audio Holder v6", style, x, y, w, h,
                            NULL, NULL, hInst, NULL);
    if (!g_wnd) return 1;

    if (!startHidden)
    {
        ShowWindow(g_wnd, SW_SHOWNORMAL);
        UpdateWindow(g_wnd);
    }

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0)
    {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (g_font) DeleteObject(g_font);
    if (g_icon) DestroyIcon(g_icon);
    if (mutex) CloseHandle(mutex);
    return (int)m.wParam;
}
