#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

static const wchar_t* APP_NAME = L"AudioDeviceBeeper";
static const wchar_t* MUTEX_NAME = L"AudioHolder_v6_1_SingleInstance";
static const wchar_t* GAME_PROCESS = L"cs2.exe";
static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const UINT WM_TRAY = WM_APP + 1;
static const UINT IDT_MAIN = 1;
static const UINT IDT_TRIM = 2;
static const UINT ID_TRAY_BUSY = 1001;
static const UINT ID_TRAY_SHOW = 1002;
static const UINT ID_TRAY_EXIT = 1003;
static const UINT ID_BUSY = 2001;
static const UINT ID_AUTOSTART = 2002;
static const UINT ID_DEVICE = 2003;
static const UINT RELEASE_DELAY = 20;
static const DWORD SAMPLE_RATE = 22050;
static const DWORD BUFFER_BYTES = SAMPLE_RATE / 4 * 2; // 250 ms, mono 16-bit
static const DWORD LOOP_COUNT = 0x7fffffff;

struct Device { UINT id; std::wstring name; };
static HWND g_hwnd = nullptr, g_combo = nullptr, g_busyButton = nullptr, g_status = nullptr, g_autoBox = nullptr;
static HINSTANCE g_instance = nullptr;
static NOTIFYICONDATAW g_nid = {};
static HMENU g_trayMenu = nullptr;
static std::vector<Device> g_devices;
static std::wstring g_configPath, g_savedDevice;
static bool g_busy = false, g_holding = false, g_cs2Active = false, g_autoOff = false, g_exiting = false, g_filling = false;
static ULONGLONG g_cs2Start = 0;
static HWAVEOUT g_wave = nullptr;
static WAVEHDR g_header = {};
static BYTE* g_audio = nullptr;
static bool g_headerPrepared = false;
static MMRESULT g_lastWaveError = MMSYSERR_NOERROR;
static HICON g_icon = nullptr;

static std::wstring GetConfigPath() {
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", path, MAX_PATH);
    std::wstring p = n && n < MAX_PATH ? path : L".";
    p += L"\\AudioDeviceBeeper";
    CreateDirectoryW(p.c_str(), nullptr);
    p += L"\\config.ini";
    return p;
}
static std::wstring ReadIni(const wchar_t* key, const wchar_t* fallback = L"") {
    wchar_t value[512] = {};
    GetPrivateProfileStringW(L"Settings", key, fallback, value, 512, g_configPath.c_str());
    return value;
}
static void SaveConfig() {
    WritePrivateProfileStringW(L"Settings", L"Device", g_savedDevice.c_str(), g_configPath.c_str());
    WritePrivateProfileStringW(L"Settings", L"Busy", g_busy ? L"1" : L"0", g_configPath.c_str());
    BOOL checked = g_autoBox ? (SendMessageW(g_autoBox, BM_GETCHECK, 0, 0) == BST_CHECKED) : FALSE;
    WritePrivateProfileStringW(L"Settings", L"Autostart", checked ? L"1" : L"0", g_configPath.c_str());
}
static bool IsAutostart() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    DWORD type = 0, bytes = 0;
    LONG r = RegQueryValueExW(key, APP_NAME, nullptr, &type, nullptr, &bytes);
    RegCloseKey(key);
    return r == ERROR_SUCCESS && bytes > 0;
}
static void SetAutostart(bool enable) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (enable) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring value = L"\"" + std::wstring(exe) + L"\" --minimized";
        RegSetValueExW(key, APP_NAME, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    } else RegDeleteValueW(key, APP_NAME);
    RegCloseKey(key);
}
static std::vector<Device> EnumerateDevices() {
    std::vector<Device> out;
    UINT count = waveOutGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        WAVEOUTCAPSW caps = {};
        if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            out.push_back({i, caps.szPname});
    }
    return out;
}
static int FindSelectedDevice() {
    for (size_t i = 0; i < g_devices.size(); ++i)
        if (g_devices[i].name == g_savedDevice) return (int)i;
    return -1;
}
static void FillDevices() {
    if (!g_combo) return;
    g_filling = true;
    g_devices = EnumerateDevices();
    SendMessageW(g_combo, CB_RESETCONTENT, 0, 0);
    for (const auto& d : g_devices) SendMessageW(g_combo, CB_ADDSTRING, 0, (LPARAM)d.name.c_str());
    int selected = FindSelectedDevice();
    if (selected < 0 && g_savedDevice.empty() && !g_devices.empty()) {
        selected = 0;
        g_savedDevice = g_devices[0].name;
    }
    if (selected >= 0) SendMessageW(g_combo, CB_SETCURSEL, selected, 0);
    g_filling = false;
}
static void UpdateStatus() {
    if (!g_status) return;
    wchar_t text[512] = {};
    if (g_lastWaveError != MMSYSERR_NOERROR) {
        wsprintfW(text, L"Status: audio cleanup error %u (retrying)", (UINT)g_lastWaveError);
    }
    else if (!g_busy) wsprintfW(text, L"Status: %s", g_autoOff ? L"off - released because CS2 started" : L"off");
    else if (g_cs2Active) {
        ULONGLONG elapsed = (GetTickCount64() - g_cs2Start) / 1000;
        int left = elapsed >= RELEASE_DELAY ? 0 : (int)(RELEASE_DELAY - elapsed);
        wsprintfW(text, left ? L"Status: CS2 detected - releasing in %d s" : L"Status: CS2 running - device released", left);
    } else if (g_holding) wsprintfW(text, L"Status: holding \"%s\"", g_savedDevice.c_str());
    else lstrcpyW(text, L"Status: waiting for the device...");
    SetWindowTextW(g_status, text);
}
static void UpdateUI() {
    if (g_busyButton) SetWindowTextW(g_busyButton, g_busy ? L"Busy: ON  (click to stop)" : L"Busy: OFF  (click to start)");
    if (g_nid.hWnd) {
        g_nid.uFlags = NIF_TIP;
        wcscpy_s(g_nid.szTip, g_busy ? L"Audio Holder: busy" : L"Audio Holder: off");
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
    if (g_trayMenu) CheckMenuItem(g_trayMenu, ID_TRAY_BUSY, MF_BYCOMMAND | (g_busy ? MF_CHECKED : MF_UNCHECKED));
    UpdateStatus();
}
// Release the WinMM stream without losing track of a handle if the driver
// reports that a header is still queued or the device is not ready to close.
// Keeping the handle and buffer alive is safer than freeing memory still owned
// by the driver. HoldTick() retries cleanup on the next timer tick.
static bool StopHold() {
    if (!g_wave) {
        if (g_audio) { HeapFree(GetProcessHeap(), 0, g_audio); g_audio = nullptr; }
        ZeroMemory(&g_header, sizeof(g_header));
        g_headerPrepared = false;
        g_holding = false;
        g_lastWaveError = MMSYSERR_NOERROR;
        UpdateStatus();
        return true;
    }

    MMRESULT r = waveOutReset(g_wave);
    if (r != MMSYSERR_NOERROR) {
        g_lastWaveError = r;
        g_holding = true;
        UpdateStatus();
        return false;
    }

    if (g_headerPrepared) {
        // Some drivers need a short moment after reset before the header can
        // be unprepared. Retry WAVERR_STILLPLAYING instead of discarding it.
        r = WAVERR_STILLPLAYING;
        for (int attempt = 0; attempt < 50; ++attempt) {
            r = waveOutUnprepareHeader(g_wave, &g_header, sizeof(g_header));
            if (r == MMSYSERR_NOERROR) {
                g_headerPrepared = false;
                break;
            }
            if (r != WAVERR_STILLPLAYING) break;
            Sleep(10);
        }
        if (g_headerPrepared) {
            g_lastWaveError = r;
            g_holding = true;
            UpdateStatus();
            return false;
        }
    }

    r = waveOutClose(g_wave);
    if (r != MMSYSERR_NOERROR) {
        g_lastWaveError = r;
        g_holding = true;
        UpdateStatus();
        return false;
    }

    g_wave = nullptr;
    if (g_audio) { HeapFree(GetProcessHeap(), 0, g_audio); g_audio = nullptr; }
    ZeroMemory(&g_header, sizeof(g_header));
    g_holding = false;
    g_lastWaveError = MMSYSERR_NOERROR;
    UpdateStatus();
    return true;
}
static bool StartHold() {
    // Never overwrite a handle left open by a driver cleanup failure.
    if (g_wave) return false;
    if (g_savedDevice.empty()) return false;
    int index = FindSelectedDevice();
    if (index < 0) {
        g_devices = EnumerateDevices();
        index = FindSelectedDevice();
    }
    if (index < 0) return false;
    WAVEFORMATEX fmt = {};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = SAMPLE_RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = SAMPLE_RATE * 2;
    MMRESULT r = waveOutOpen(&g_wave, g_devices[index].id, &fmt, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR) { g_wave = nullptr; return false; }
    g_audio = (BYTE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, BUFFER_BYTES);
    if (!g_audio) { StopHold(); return false; }
    // A zero-filled PCM buffer is digital silence.
    ZeroMemory(&g_header, sizeof(g_header));
    g_header.lpData = (LPSTR)g_audio;
    g_header.dwBufferLength = BUFFER_BYTES;
    g_header.dwLoops = LOOP_COUNT;
    r = waveOutPrepareHeader(g_wave, &g_header, sizeof(g_header));
    if (r != MMSYSERR_NOERROR) { StopHold(); return false; }
    g_headerPrepared = true;
    // Set loop flags after preparation, as required by the WinMM header lifecycle.
    g_header.dwFlags |= WHDR_BEGINLOOP | WHDR_ENDLOOP;
    r = waveOutWrite(g_wave, &g_header, sizeof(g_header));
    if (r != MMSYSERR_NOERROR) { StopHold(); return false; }
    g_holding = true;
    g_lastWaveError = MMSYSERR_NOERROR;
    return true;
}
static bool IsGameRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) do {
        if (_wcsicmp(pe.szExeFile, GAME_PROCESS) == 0) { found = true; break; }
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return found;
}
static void HoldTick() {
    if (g_exiting) return;
    if (!g_busy) {
        g_cs2Active = false;
        if (g_holding) StopHold();
        UpdateStatus();
        return;
    }
    if (IsGameRunning()) {
        if (!g_cs2Active) { g_cs2Active = true; g_cs2Start = GetTickCount64(); }
        if (GetTickCount64() - g_cs2Start >= RELEASE_DELAY * 1000ULL) {
            StopHold();
            g_cs2Active = false;
            g_busy = false;
            g_autoOff = true;
            UpdateUI();
            SaveConfig();
            return;
        }
    } else {
        g_cs2Active = false;
    }
    if (g_holding) {
        if (g_header.dwFlags & WHDR_DONE) StopHold();
        else {
            // Device index can change after hot-plug; verify the selected device still exists.
            auto current = EnumerateDevices();
            bool exists = false;
            for (const auto& d : current) if (d.name == g_savedDevice) { exists = true; break; }
            if (!exists) StopHold();
        }
    }
    if (!g_cs2Active && !g_holding) {
        if (!StartHold()) {
            auto current = EnumerateDevices();
            if (current.size() != g_devices.size()) FillDevices();
        }
    }
    UpdateStatus();
}
static void SetBusy(bool on) {
    g_busy = on; g_autoOff = false; g_cs2Active = false;
    if (!on) StopHold();
    UpdateUI(); SaveConfig(); HoldTick();
}
static void HideToTray() { ShowWindow(g_hwnd, SW_HIDE); }
static void ShowFromTray() { ShowWindow(g_hwnd, SW_SHOW); ShowWindow(g_hwnd, SW_RESTORE); SetForegroundWindow(g_hwnd); }
static void ExitApp() {
    g_exiting = true;
    KillTimer(g_hwnd, IDT_MAIN);
    KillTimer(g_hwnd, IDT_TRIM);
    StopHold(); SaveConfig();
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    DestroyWindow(g_hwnd);
}
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND label = CreateWindowW(L"STATIC", L"Audio output device:", WS_CHILD | WS_VISIBLE, 16, 14, 180, 20, hwnd, 0, g_instance, 0);
        g_combo = CreateWindowExW(WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 16, 36, 408, 220, hwnd, (HMENU)ID_DEVICE, g_instance, 0);
        g_busyButton = CreateWindowW(L"BUTTON", L"Busy: OFF  (click to start)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 16, 74, 408, 38, hwnd, (HMENU)ID_BUSY, g_instance, 0);
        g_autoBox = CreateWindowW(L"BUTTON", L"Start with Windows", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 16, 124, 200, 22, hwnd, (HMENU)ID_AUTOSTART, g_instance, 0);
        g_status = CreateWindowW(L"STATIC", L"Status: off", WS_CHILD | WS_VISIBLE, 16, 150, 408, 20, hwnd, 0, g_instance, 0);
        HWND controls[] = { label, g_combo, g_busyButton, g_autoBox, g_status };
        for (HWND c : controls) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        bool autoStart = ReadIni(L"Autostart", L"1") == L"1";
        SendMessageW(g_autoBox, BM_SETCHECK, autoStart ? BST_CHECKED : BST_UNCHECKED, 0);
        SetAutostart(autoStart);
        FillDevices();
        g_trayMenu = CreatePopupMenu();
        AppendMenuW(g_trayMenu, MF_STRING, ID_TRAY_BUSY, L"Busy");
        AppendMenuW(g_trayMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(g_trayMenu, MF_STRING, ID_TRAY_SHOW, L"Show");
        AppendMenuW(g_trayMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");
        g_nid.cbSize = sizeof(g_nid); g_nid.hWnd = hwnd; g_nid.uID = 1; g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; g_nid.uCallbackMessage = WM_TRAY;
        g_nid.hIcon = (HICON)LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON, 0, 0, LR_SHARED);
        wcscpy_s(g_nid.szTip, L"Audio Holder: off");
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        SetTimer(hwnd, IDT_MAIN, 1000, nullptr);
        SetTimer(hwnd, IDT_TRIM, 3000, nullptr);
        UpdateUI();
        HoldTick();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_TRAY_BUSY: SetBusy(!g_busy); return 0;
        case ID_TRAY_SHOW: ShowFromTray(); return 0;
        case ID_TRAY_EXIT: ExitApp(); return 0;
        case ID_BUSY: SetBusy(!g_busy); return 0;
        case ID_AUTOSTART: {
            bool enabled = SendMessageW(g_autoBox, BM_GETCHECK, 0, 0) == BST_CHECKED;
            SetAutostart(enabled); SaveConfig(); return 0;
        }
        case ID_DEVICE:
            if (HIWORD(wp) == CBN_SELCHANGE && !g_filling) {
                int i = (int)SendMessageW(g_combo, CB_GETCURSEL, 0, 0);
                if (i >= 0 && i < (int)g_devices.size()) {
                    g_savedDevice = g_devices[i].name; SaveConfig(); StopHold(); HoldTick();
                }
            }
            return 0;
        }
        break;
    case WM_TIMER:
        if (wp == IDT_MAIN) HoldTick();
        else if (wp == IDT_TRIM) { KillTimer(hwnd, IDT_TRIM); }
        return 0;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) HideToTray();
        return 0;
    case WM_CLOSE: SaveConfig(); HideToTray(); return 0;
    case WM_TRAY:
        if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) ShowFromTray();
        else if (lp == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt); SetForegroundWindow(hwnd);
            TrackPopupMenu(g_trayMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            PostMessageW(hwnd, WM_NULL, 0, 0);
        }
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC) { StopHold(); }
        return TRUE;
    case WM_DESTROY:
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int show) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, MUTEX_NAME);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) { if (mutex) CloseHandle(mutex); return 0; }
    g_instance = instance;
    g_configPath = GetConfigPath();
    g_savedDevice = ReadIni(L"Device");
    g_busy = ReadIni(L"Busy", L"1") == L"1";
    g_autoOff = false;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc; wc.hInstance = instance; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION); wc.hIconSm = wc.hIcon; wc.lpszClassName = L"AudioHolderV61Window";
    if (!RegisterClassExW(&wc)) { CloseHandle(mutex); return 1; }
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Audio Holder v6.1", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 456, 220, nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) { CloseHandle(mutex); return 1; }
    bool minimized = wcsstr(cmdLine ? cmdLine : L"", L"--minimized") != nullptr;
    if (!minimized) { ShowWindow(g_hwnd, SW_SHOW); UpdateWindow(g_hwnd); }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_trayMenu) DestroyMenu(g_trayMenu);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return (int)msg.wParam;
}
