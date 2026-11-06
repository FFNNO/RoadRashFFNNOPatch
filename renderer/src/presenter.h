#pragma once
#include <windows.h>
#include <cstdint>

struct LetterboxViewport {
    int x;
    int y;
    int width;
    int height;
};

LetterboxViewport ComputeLetterboxViewport(int srcW, int srcH, int clientW, int clientH);

bool PresenterPresent8bpp(HWND hwnd, const uint8_t* surface, int pitch, const uint8_t* palette, int width, int height);
bool PresenterPresentBgra(HWND hwnd, const uint32_t* dib, int width, int height);
bool PresenterIsLatchedOff();
void PresenterNotifyWindowChange();
void PresenterShutdown();
