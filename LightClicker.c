#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <mmsystem.h>

#define WM_TRAYICON      (WM_USER + 1)
#define IDM_SPEED_MAX    1001
#define IDM_SPEED_HIGH   1002
#define IDM_SPEED_NORMAL 1003
#define IDM_TOGGLE_MODE  1005
#define IDM_EXIT         1004

#define SLEEP_MAX    1
#define SLEEP_HIGH   50
#define SLEEP_NORMAL 100

#define SYNTH_MAGIC 0xC1C1C1C1UL

// ---- Hotkey modifier -- change to VK_LMENU for Alt, VK_LSHIFT for Shift ----
#define HOTKEY_VK VK_LCONTROL

static HHOOK         hMouseHook;
static volatile int  clicking     = 0;  /* which button is active: 0=none, 1=M1, 2=M2, 3=M3 */
static HWND          hwnd;
static int           sleep_ms     = SLEEP_HIGH;
static BOOL          is_light_theme = FALSE;
static BOOL          toggle_mode  = FALSE;

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
    AppendMenuA(menu, MF_STRING | (toggle_mode ? MF_CHECKED : 0), IDM_TOGGLE_MODE, "Toggle");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, IDM_EXIT, "Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
}

// ---- Click thread ----
typedef struct { int button; } ThreadParam;

static DWORD WINAPI click_thread(LPVOID param) {
    int btn = ((ThreadParam *)param)->button;
    HeapFree(GetProcessHeap(), 0, param);

    INPUT inp    = {0};
    INPUT inp_up = {0};

    inp.type           = INPUT_MOUSE;
    inp.mi.dwExtraInfo = SYNTH_MAGIC;

    inp_up.type           = INPUT_MOUSE;
    inp_up.mi.dwExtraInfo = SYNTH_MAGIC;

    switch (btn) {
    case 1:
        inp.mi.dwFlags    = MOUSEEVENTF_LEFTDOWN;
        inp_up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        break;
    case 2:
        inp.mi.dwFlags    = MOUSEEVENTF_RIGHTDOWN;
        inp_up.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        break;
    case 3:
        inp.mi.dwFlags    = MOUSEEVENTF_MIDDLEDOWN;
        inp_up.mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
        break;
    default:
        return 0;
    }

    while (clicking == btn) {
        SendInput(1, &inp,    sizeof(INPUT));
        SendInput(1, &inp_up, sizeof(INPUT));
        Sleep(sleep_ms);
    }
    return 0;
}

static void start_clicking(int btn) {
    ThreadParam *p = (ThreadParam *)HeapAlloc(GetProcessHeap(), 0, sizeof(ThreadParam));
    if (!p) return;
    p->button = btn;
    clicking = btn;
    HANDLE h = CreateThread(NULL, 0, click_thread, p, 0, NULL);
    if (h) CloseHandle(h);
    else { clicking = 0; HeapFree(GetProcessHeap(), 0, p); }
}

static void stop_clicking(void) {
    clicking = 0;
}

// ---- Mouse hook ----
static LRESULT CALLBACK MouseProc(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode >= 0) {
        MSLLHOOKSTRUCT *ms = (MSLLHOOKSTRUCT *)lp;

        if (ms->dwExtraInfo == SYNTH_MAGIC)
            return CallNextHookEx(hMouseHook, nCode, wp, lp);

        int hotkey = GetAsyncKeyState(HOTKEY_VK) & 0x8000;

        /* Determine which button and whether it's a down or up event */
        int  btn_down = 0, btn_up = 0;
        if      (wp == WM_LBUTTONDOWN) btn_down = 1;
        else if (wp == WM_RBUTTONDOWN) btn_down = 2;
        else if (wp == WM_MBUTTONDOWN) btn_down = 3;
        else if (wp == WM_LBUTTONUP)   btn_up   = 1;
        else if (wp == WM_RBUTTONUP)   btn_up   = 2;
        else if (wp == WM_MBUTTONUP)   btn_up   = 3;

        if (toggle_mode) {
            /* Toggle mode: hotkey+down either starts or stops */
            if (hotkey && btn_down) {
                if (!clicking) {
                    start_clicking(btn_down);
                    PlaySoundA("SystemAsterisk", NULL,
                               SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
                } else {
                    stop_clicking();
                    PlaySoundA("SystemHand", NULL,
                               SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
                }
                return 1;
            }
            /* Suppress the matching up event while active */
            if (btn_up && clicking == btn_up)
                return 1;
        } else {
            /* Hold mode: hotkey+down starts, any up of the active button stops */
            if (hotkey && btn_down && !clicking) {
                start_clicking(btn_down);
                return 1;
            }
            if (btn_up && clicking == btn_up) {
                stop_clicking();
                return 1;
            }
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
        case IDM_TOGGLE_MODE:
            if (toggle_mode && clicking)
                stop_clicking();
            toggle_mode = !toggle_mode;
            break;
        case IDM_EXIT:
            stop_clicking();
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
        stop_clicking();
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
