#pragma once
// On-screen toast ("Partida guardada" / "Partida cargada"): timing, placement and GDI text
// rasterization. Placement and timing are pure functions; the presenter owns the GPU side.
#include <cstdint>
#include <vector>

enum class ToastKind : uint32_t {
    None = 0,
    Saved = 1,
    Loaded = 2
};

// Spanish, to match the game's language.
const wchar_t* ToastText(ToastKind kind);

constexpr uint64_t kToastVisibleMs = 3000;  // total time on screen, fade included
constexpr uint64_t kToastFadeMs = 500;      // linear fade-out at the end
constexpr uint64_t kToastTimerMs = 50;      // re-present cadence while the game is idle

// 1.0 until the fade starts, then a linear ramp to 0.0 at kToastVisibleMs. A start time in the
// future (clock skew) counts as just started.
float ToastAlpha(uint64_t startMs, uint64_t nowMs);
bool ToastExpired(uint64_t startMs, uint64_t nowMs);

// Whether the toast timer must re-present the last frame itself: the game has not presented for a
// whole timer period (static menu), or the toast expired while the last presented frame still
// shows it.
bool ToastNeedsRepresent(uint64_t nowMs, uint64_t lastPresentMs, bool expired, bool lastFrameHadToast);

struct ToastRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct ToastPlacement {
    ToastRect text;
    ToastRect background;  // valid only when drawBackground
    bool inMargin = false;
    bool drawBackground = false;
};

// Font height in pixels for a client area of clientH pixels.
int ToastFontHeight(int clientH);
// Smallest acceptable font height when shrinking the text to fit the margin.
int ToastMinFontHeight(int clientH);
// Gap between the text and the client/image edges.
int ToastPadding(int clientH);
// Right pillarbox margin: client area right of the image.
int ToastRightMargin(int clientW, const ToastRect& image);
// Font height that makes text measured as textW pixels at baseFontH fit the right margin, or 0 when
// it already fits at baseFontH or would need a font smaller than minFontH.
int ToastMarginFontHeight(int baseFontH, int textW, int marginW, int padding, int minFontH);
// Bottom of the right margin when it holds the text plus padding on both sides; otherwise the
// bottom-right corner of the image over a translucent background rectangle.
ToastPlacement ComputeToastPlacement(int clientW, int clientH, const ToastRect& image, int textW, int textH,
                                     int padding);

struct ToastBitmap {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;  // BGRA, top-down: white with alpha = glyph coverage
};

// Renders text with GDI (bold sans, antialiased grayscale) into a 32-bit DIB section, white on
// black, and converts the coverage to alpha.
bool RasterizeToastText(const wchar_t* text, int fontHeight, ToastBitmap& out);
