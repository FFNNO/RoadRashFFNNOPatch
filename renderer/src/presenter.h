#pragma once
#include <windows.h>
#include <cstdint>

bool PresenterPresent8bpp(HWND hwnd, const uint8_t* surface, int pitch, const uint8_t* palette, int width, int height);
bool PresenterPresentBgra(HWND hwnd, const uint32_t* dib, int width, int height);
void PresenterNotifyWindowChange();
void PresenterShutdown();
