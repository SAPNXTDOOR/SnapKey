#pragma once
#include <windows.h>

LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam);
void installMouseHook(HINSTANCE hInst);

// Global toggle (default = enabled)
extern bool lmbOverrideEnabled;
