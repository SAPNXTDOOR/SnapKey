#define UNICODE
#define _UNICODE
#define IDI_APP_ICON 101

#include <windows.h>
#include <shellapi.h>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <regex>
#include <vector>
#include <filesystem>

#include "MouseOverride.h"   // installMouseHook / lmbOverrideEnabled

using namespace std;
namespace fs = std::filesystem;

// ---------- IDs ----------
#define ID_TRAY_APP_ICON                 1001
#define WM_TRAYICON                      (WM_USER + 1)

#define ID_TRAY_TOGGLE_SNAPKEY           2000
#define ID_TRAY_TOGGLE_LMB               2001
#define ID_TRAY_REBIND_KEYS              2100
#define ID_TRAY_RESTART_SNAPKEY          2101
#define ID_TRAY_ABOUT                    2200
#define ID_TRAY_EXIT                     2201

#define IDC_ABOUT_OK_BUTTON              3001

// ---------- Types ----------
struct KeyState {
    bool registered = false;
    bool keyDown = false;
    int  group = 0;
    bool simulated = false;
};

struct GroupState {
    int previousKey = 0;
    int activeKey   = 0;
};

// ---------- Globals ----------
unordered_map<int, GroupState> GroupInfo;
unordered_map<int, KeyState>   KeyInfo;

HHOOK  hHook   = NULL;
HANDLE hMutex  = NULL;
NOTIFYICONDATA nid{};
bool isLocked = false;   // false = SnapKey enabled

extern bool lmbOverrideEnabled;
std::vector<int> lmbOverrideKeys;

// ---------- Forward Declarations ----------
LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

bool   LoadConfig(const std::string& filename);
void   CreateDefaultConfig(const std::string& filename);
void   RestoreConfigFromBackup(const std::string& backupFilename, const std::string& destinationFilename);

void   InitNotifyIconData(HWND hwnd, HICON hIcon);
void   RestartSnapKey();
void   SendKey(int target, bool keyDown);

LRESULT CALLBACK AboutWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void   ShowAboutDialog(HWND parent);

HICON  LoadAppIconLarge();
HICON  LoadAppIconSmall();

// ---------- Main ----------
int main() {
    if (!LoadConfig("config.cfg")) return 1;

    hMutex = CreateMutex(NULL, TRUE, TEXT("SnapKeyMutex"));
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBox(NULL, TEXT("SnapKey is already running!"), TEXT("SnapKey"), MB_ICONINFORMATION | MB_OK);
        return 1;
    }

    HINSTANCE hInst = GetModuleHandle(NULL);

    WNDCLASSEX wc{};
    wc.cbSize        = sizeof(WNDCLASSEX);
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = TEXT("SnapKeyClass");
    wc.hIcon         = LoadAppIconLarge();
    wc.hIconSm       = LoadAppIconSmall();
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    if (!RegisterClassEx(&wc)) {
        MessageBox(NULL, TEXT("Window Registration Failed!"), TEXT("Error"), MB_ICONEXCLAMATION | MB_OK);
        return 1;
    }

    HWND hwnd = CreateWindowEx(0, wc.lpszClassName, TEXT("SnapKey"), WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 240, 120,
                               NULL, NULL, hInst, NULL);

    InitNotifyIconData(hwnd, LoadAppIconSmall());

    hHook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, NULL, 0);
    if (!hHook) {
        MessageBox(NULL, TEXT("Failed to install keyboard hook!"), TEXT("Error"), MB_ICONEXCLAMATION | MB_OK);
        return 1;
    }

    installMouseHook(hInst);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(hHook);
    Shell_NotifyIcon(NIM_DELETE, &nid);
    if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
    return 0;
}

// ---------- Keyboard logic ----------
bool isSimulatedKeyEvent(DWORD flags) { return (flags & 0x10) != 0; }

void handleKeyDown(int keyCode) {
    KeyState& ks = KeyInfo[keyCode];
    GroupState& gs = GroupInfo[ks.group];
    if (!ks.keyDown) {
        ks.keyDown = true;
        SendKey(keyCode, true);
        if (gs.activeKey == 0 || gs.activeKey == keyCode) {
            gs.activeKey = keyCode;
        } else {
            gs.previousKey = gs.activeKey;
            gs.activeKey = keyCode;
            SendKey(gs.previousKey, false);
        }
    }
}

void handleKeyUp(int keyCode) {
    KeyState& ks = KeyInfo[keyCode];
    GroupState& gs = GroupInfo[ks.group];

    if (gs.previousKey == keyCode && !ks.keyDown) gs.previousKey = 0;

    if (ks.keyDown) {
        ks.keyDown = false;
        if (gs.activeKey == keyCode && gs.previousKey != 0) {
            SendKey(keyCode, false);
            gs.activeKey = gs.previousKey;
            gs.previousKey = 0;
            SendKey(gs.activeKey, true);
        } else {
            gs.previousKey = 0;
            if (gs.activeKey == keyCode) gs.activeKey = 0;
            SendKey(keyCode, false);
        }
    }
}

void SendKey(int targetKey, bool keyDown) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = targetKey;
    input.ki.wScan = MapVirtualKey(targetKey, 0);
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (keyDown ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &input, sizeof(INPUT));
}

LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (!isLocked && nCode >= 0) {
        KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;
        if (!isSimulatedKeyEvent(kb->flags) && KeyInfo[kb->vkCode].registered) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) handleKeyDown(kb->vkCode);
            if (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP)   handleKeyUp(kb->vkCode);
            return 1;
        }
    }
    return CallNextHookEx(hHook, nCode, wParam, lParam);
}

// ---------- Config ----------
void RestoreConfigFromBackup(const std::string& backupFilename, const std::string& destinationFilename) {
    std::wstring src = std::wstring(L"meta\\") + std::wstring(backupFilename.begin(), backupFilename.end());
    std::wstring dst = std::wstring(destinationFilename.begin(), destinationFilename.end());
    CopyFileW(src.c_str(), dst.c_str(), FALSE);
}
void CreateDefaultConfig(const std::string& filename) {
    RestoreConfigFromBackup("backup.snapkey", filename);
}
bool LoadConfig(const std::string& filename) {
    std::ifstream configFile(filename);
    if (!configFile.is_open()) { CreateDefaultConfig(filename); return false; }

    string line; int id = 0;
    while (getline(configFile, line)) {
        istringstream iss(line);
        string key; int value;
        regex secPat(R"(\s*\[Group\]\s*)");
        if (regex_match(line, secPat)) {
            id++;
        } else if (getline(iss, key, '=')) {
            if (key == "lmb_override") {
                iss >> value; lmbOverrideEnabled = (value != 0);
            } else if (key == "lmb_override_keys") {
                string values;
                if (getline(iss, values)) {
                    lmbOverrideKeys.clear();
                    stringstream ss(values); string token;
                    while (getline(ss, token, ',')) {
                        try { int vk = stoi(token); lmbOverrideKeys.push_back(vk); }
                        catch (...) {}
                    }
                }
            } else if (iss >> value) {
                if (key.find("key") != string::npos) {
                    if (!KeyInfo[value].registered) {
                        KeyInfo[value].registered = true;
                        KeyInfo[value].group = id;
                    }
                }
            }
        }
    }
    return true;
}

// ---------- Tray / UI ----------
void InitNotifyIconData(HWND hwnd, HICON) {
    memset(&nid, 0, sizeof(NOTIFYICONDATA));
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd   = hwnd;
    nid.uID    = ID_TRAY_APP_ICON;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;

    // Always use our custom app icon
    nid.hIcon = LoadAppIconSmall();

    lstrcpy(nid.szTip, TEXT("SnapKey"));
    Shell_NotifyIcon(NIM_ADD, &nid);
}

void RestartSnapKey() {
    TCHAR exe[MAX_PATH]; GetModuleFileName(NULL, exe, MAX_PATH);
    ShellExecute(NULL, TEXT("open"), exe, NULL, NULL, SW_SHOWNORMAL);
    PostQuitMessage(0);
}

HMENU BuildTrayMenu() {
    HMENU hMenu = CreatePopupMenu();

    AppendMenu(hMenu, MF_STRING, ID_TRAY_TOGGLE_SNAPKEY,
        isLocked ? TEXT("Enable SnapKey") : TEXT("Disable SnapKey"));

    AppendMenu(hMenu, MF_STRING, ID_TRAY_TOGGLE_LMB,
        lmbOverrideEnabled ? TEXT("Disable LMB Override") : TEXT("Enable LMB Override"));

    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);

    AppendMenu(hMenu, MF_STRING, ID_TRAY_REBIND_KEYS,     TEXT("Rebind Keys"));
    AppendMenu(hMenu, MF_STRING, ID_TRAY_RESTART_SNAPKEY, TEXT("Restart SnapKey"));

    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);

    AppendMenu(hMenu, MF_STRING, ID_TRAY_ABOUT, TEXT("About"));
    AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT,  TEXT("Exit"));

    return hMenu;
}

// ---------- About ----------
LRESULT CALLBACK AboutWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
    CreateWindowEx(0, L"STATIC",
        L"This app is developed by Cafali (github.com/cafali) and further modified by Saptarshi (github.com/SAPNXTDOOR) to add more functions.",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_EDITCONTROL,
        20, 20, 400, 60, hWnd, NULL,
        ((LPCREATESTRUCT)lParam)->hInstance, NULL);

    CreateWindowEx(0, L"BUTTON", L"OK",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        180, 95, 80, 25,
        hWnd, (HMENU)IDC_ABOUT_OK_BUTTON,
        ((LPCREATESTRUCT)lParam)->hInstance, NULL);
    break;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_ABOUT_OK_BUTTON) {
            DestroyWindow(hWnd);
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hWnd);
        break;
    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

void ShowAboutDialog(HWND parent) {
    HINSTANCE hInst = GetModuleHandle(NULL);

    static bool registered = false;
    if (!registered) {
        WNDCLASS wc{};
        wc.lpfnWndProc = AboutWndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = L"AboutDialog";
        wc.hIcon = LoadAppIconSmall();
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
        RegisterClass(&wc);
        registered = true;
    }

    HWND hAbout = CreateWindowEx(WS_EX_DLGMODALFRAME, L"AboutDialog", L"About SnapKey",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 460, 160,
        parent, NULL, hInst, NULL);

    if (hAbout) {
        ShowWindow(hAbout, SW_SHOW);
        UpdateWindow(hAbout);
    }
}

// ---------- Main Window Proc ----------
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONDOWN) {
            POINT cur; GetCursorPos(&cur);
            SetForegroundWindow(hwnd);
            HMENU hMenu = BuildTrayMenu();
            TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, cur.x, cur.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_TRAY_TOGGLE_SNAPKEY:
            isLocked = !isLocked;
            break;
        case ID_TRAY_TOGGLE_LMB:
            lmbOverrideEnabled = !lmbOverrideEnabled;
            break;
        case ID_TRAY_REBIND_KEYS:
            ShellExecute(NULL, TEXT("open"), TEXT("config.cfg"), NULL, NULL, SW_SHOWNORMAL);
            break;
        case ID_TRAY_RESTART_SNAPKEY:
            RestartSnapKey();
            break;
        case ID_TRAY_ABOUT:
            ShowAboutDialog(hwnd);
            break;
        case ID_TRAY_EXIT:
            PostQuitMessage(0);
            break;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ---------- Icons ----------
HICON LoadAppIconLarge() {
    return (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP_ICON),
                            IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
}
HICON LoadAppIconSmall() {
    return (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP_ICON),
                            IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
}
