#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include "overlay_text.h"

struct LetterboxViewport {
    int x;
    int y;
    int width;
    int height;
};

LetterboxViewport ComputeLetterboxViewport(int srcW, int srcH, int clientW, int clientH);

// Mod API overlay layer (rr95-music-standalone S1): a client layer is expressed in the game's 640x480
// coordinates and mapped into the letterboxed image, with the same integer truncation the engine's
// own coordinate scaling uses (x * viewport.width / 640).
constexpr int kOverlayGameWidth = 640;
constexpr int kOverlayGameHeight = 480;

struct OverlayRect {
    int x;
    int y;
    int width;
    int height;
};

OverlayRect MapOverlayRectToClient(const LetterboxViewport& viewport, int x, int y, int width, int height);

struct PresenterFailureState {
    int failureCount = 0;
    uint64_t firstFailureMs = 0;
    uint64_t lastFailureMs = 0;
    bool latchedOff = false;
};

constexpr int kPresenterMaxFailuresInWindow = 3;
constexpr uint64_t kPresenterFailureWindowMs = 5000;
constexpr uint64_t kPresenterRetryBackoffMs = 1000;

bool PresenterCanAttempt(const PresenterFailureState& state, uint64_t nowMs);
bool PresenterRecordFailure(PresenterFailureState& state, uint64_t nowMs);
void PresenterRecordSuccess(PresenterFailureState& state);
// True only when the presenter is healthy: not latched off, no failed present since the last
// success, and therefore not inside the retry backoff. The software rasterizer may be skipped
// (GpuWorld) only then, so a frame that falls back to GDI always contains the software world.
bool PresenterStateAllowsGpuWorld(const PresenterFailureState& state);

// Sub-pixel vertex counters (T6) accumulated by GPU world frames since the last call.
struct PresenterSubPixelStats {
    uint64_t frames = 0;
    uint64_t floatQuads = 0;      // quads drawn at their 16.16 corners
    uint64_t fallbackQuads = 0;   // eligible quads drawn at their snapped vertices
    uint32_t maxFallbackPerFrame = 0;
};
PresenterSubPixelStats PresenterTakeSubPixelStats();

// Full HLSL source of the world pixel shader (shared quad mapping + main), exposed for tests.
std::string PresenterWorldPixelShaderSource();

// hudIcons: optional width x height premultiplied 0xAARRGGBB layer (see hud_icons.h) drawn over
// the HUD layer of a GPU world frame; ignored on the software path. A failure to draw it is
// logged once and the frame is presented without it.
bool PresenterPresent8bpp(HWND hwnd, const uint8_t* surface, int pitch, const uint8_t* palette, int width, int height,
                          const uint32_t* hudIcons = nullptr);
bool PresenterPresentBgra(HWND hwnd, const uint32_t* dib, int width, int height);
bool PresenterIsLatchedOff();
bool PresenterGpuWorldAllowed();
void PresenterNotifyWindowChange();
void PresenterShutdown();

// Starts the save/load toast. Callable from any thread: the toast is drawn by the next present.
// When the caller is the thread that presents (the game thread), the last frame is also
// re-presented at once with the toast, and a window timer keeps re-presenting it while the game
// does not present (static menus) until the toast has faded out and been removed.
void PresenterShowToast(ToastKind kind);
