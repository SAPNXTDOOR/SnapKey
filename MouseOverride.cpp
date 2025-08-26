#include "MouseOverride.h"
#include <set>
#include <vector>
#include <Windows.h>

// Forward declaration from SnapKey.cpp
extern void SendKey(int targetKey, bool keyDown);
extern std::vector<int> lmbOverrideKeys;

// Global toggle
bool lmbOverrideEnabled = true;

// Track suppressed keys
bool lmbHeld = false;
std::set<int> suppressedKeys;

// Mouse hook procedure
LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && lmbOverrideEnabled) {
        switch (wParam) {
            case WM_LBUTTONDOWN:
                lmbHeld = true;
                for (int key : lmbOverrideKeys) {
                    if (GetAsyncKeyState(key) & 0x8000) {
                        SendKey(key, false);   // fake release
                        suppressedKeys.insert(key);
                    }
                }
                break;

            case WM_LBUTTONUP:
                lmbHeld = false;
                for (int key : suppressedKeys) {
                    SendKey(key, true);    // restore
                }
                suppressedKeys.clear();
                break;
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// Install mouse hook
void installMouseHook(HINSTANCE hInst) {
    SetWindowsHookEx(WH_MOUSE_LL, MouseProc, hInst, 0);
}
