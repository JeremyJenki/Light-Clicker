#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#define WM_TRAYICON      (WM_USER + 1)
#define IDM_SPEED_MAX    1001
#define IDM_SPEED_HIGH   1002
#define IDM_SPEED_NORMAL 1003
#define IDM_EXIT         1004

#define SLEEP_MAX    1
#define SLEEP_HIGH   50
#define SLEEP_NORMAL 100

#define SYNTH_MAGIC 0xC1C1C1C1UL

static HHOOK         hMouseHook;
static volatile int  clicking   = 0;
static HWND          hwnd;
static int           sleep_ms   = SLEEP_HIGH;
static BOOL          is_light_theme = FALSE;

// ---- Theme detection ----
static BOOL detect_light_theme(void) {
    HKEY hKey;
    DWORD value = 0, size = sizeof(DWORD);
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExA(hKey, "SystemUsesLightTheme", NULL, NULL,
                         (LPBYTE)&value, &size);
        RegCloseKey(hKey);
    }
    return value == 1;
}

#define IDI_LIGHT 2
#define IDI_DARK  3

static HICON get_tray_icon(void) {
    HINSTANCE hInst = GetModuleHandleA(NULL);
    HICON icon = (HICON)LoadImageA(hInst,
        MAKEINTRESOURCEA(is_light_theme ? IDI_LIGHT : IDI_DARK),
        IMAGE_ICON, 64, 64, LR_DEFAULTCOLOR);
    if (!icon) icon = LoadIcon(NULL, IDI_APPLICATION);
    return icon;
}

// ---- Tray ----
static void add_tray_icon(void) {
    NOTIFYICONDATAA nid = {0};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = hwnd;
    nid.uID              = 1;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon            = get_tray_icon();
    strncpy(nid.szTip, "Light Clicker", sizeof(nid.szTip) - 1);
    Shell_NotifyIconA(NIM_ADD, &nid);
}

static void update_tray_icon(void) {
    NOTIFYICONDATAA nid = {0};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd;
    nid.uID    = 1;
    nid.uFlags = NIF_ICON;
    nid.hIcon  = get_tray_icon();
    Shell_NotifyIconA(NIM_MODIFY, &nid);
}

static void remove_tray_icon(void) {
    NOTIFYICONDATAA nid = {0};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd;
    nid.uID    = 1;
    Shell_NotifyIconA(NIM_DELETE, &nid);
}

static void show_tray_menu(void) {
    HMENU menu = CreatePopupMenu();
    AppendMenuA(menu, MF_STRING | (sleep_ms == SLEEP_MAX    ? MF_CHECKED : 0), IDM_SPEED_MAX,    "Max");
    AppendMenuA(menu, MF_STRING | (sleep_ms == SLEEP_HIGH   ? MF_CHECKED : 0), IDM_SPEED_HIGH,   "High");
    AppendMenuA(menu, MF_STRING | (sleep_ms == SLEEP_NORMAL ? MF_CHECKED : 0), IDM_SPEED_NORMAL, "Normal");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, IDM_EXIT, "Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
}

// ---- Clicker thread ----
static DWORD WINAPI click_thread(LPVOID param) {
    (void)param;
    INPUT inp    = {0};
    INPUT inp_up = {0};

    inp.type              = INPUT_MOUSE;
    inp.mi.dwFlags        = MOUSEEVENTF_LEFTDOWN;
    inp.mi.dwExtraInfo    = SYNTH_MAGIC;

    inp_up.type           = INPUT_MOUSE;
    inp_up.mi.dwFlags     = MOUSEEVENTF_LEFTUP;
    inp_up.mi.dwExtraInfo = SYNTH_MAGIC;

    while (clicking) {
        SendInput(1, &inp,    sizeof(INPUT));
        SendInput(1, &inp_up, sizeof(INPUT));
        Sleep(sleep_ms);
    }
    return 0;
}

// ---- Mouse hook ----
static LRESULT CALLBACK MouseProc(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode >= 0) {
        MSLLHOOKSTRUCT *ms = (MSLLHOOKSTRUCT *)lp;

        if (ms->dwExtraInfo == SYNTH_MAGIC)
            return CallNextHookEx(hMouseHook, nCode, wp, lp);

        int alt = GetAsyncKeyState(VK_LMENU) & 0x8000;

        if (alt && wp == WM_LBUTTONDOWN && !clicking) {
            clicking = 1;
            HANDLE h = CreateThread(NULL, 0, click_thread, NULL, 0, NULL);
            if (h) CloseHandle(h);
            return 1;
        } else if (wp == WM_LBUTTONUP && clicking) {
            clicking = 0;
            return 1;
        }
    }
    return CallNextHookEx(hMouseHook, nCode, wp, lp);
}

// ---- Window proc ----
static LRESULT CALLBACK WndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAYICON:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP)
            show_tray_menu();
        break;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_SPEED_MAX:    sleep_ms = SLEEP_MAX;    break;
        case IDM_SPEED_HIGH:   sleep_ms = SLEEP_HIGH;   break;
        case IDM_SPEED_NORMAL: sleep_ms = SLEEP_NORMAL; break;
        case IDM_EXIT:
            clicking = 0;
            remove_tray_icon();
            PostQuitMessage(0);
            break;
        }
        break;
    case WM_SETTINGCHANGE:
        {
            BOOL new_theme = detect_light_theme();
            if (new_theme != is_light_theme) {
                is_light_theme = new_theme;
                update_tray_icon();
            }
        }
        break;
    case WM_DESTROY:
        clicking = 0;
        remove_tray_icon();
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

// ---- Entry point ----
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev; (void)cmd; (void)show;

    CreateMutexA(NULL, TRUE, "LightClicker_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    is_light_theme = detect_light_theme();

    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = "LightClickerWnd";
    RegisterClassA(&wc);

    hwnd = CreateWindowA("LightClickerWnd", NULL, WS_POPUP,
                         0, 0, 0, 0, NULL, NULL, hInst, NULL);

    if (!hwnd) return 1;

    add_tray_icon();
    hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseProc, NULL, 0);

    MSG m;
    while (GetMessageA(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }

    UnhookWindowsHookEx(hMouseHook);
    return 0;
}