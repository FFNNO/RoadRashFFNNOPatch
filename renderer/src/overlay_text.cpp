#include "overlay_text.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

const wchar_t* ToastText(ToastKind kind) {
    switch (kind) {
        case ToastKind::Saved:  return L"Partida guardada";
        case ToastKind::Loaded: return L"Partida cargada";
        case ToastKind::None:   break;
    }
    return L"";
}

float ToastAlpha(uint64_t startMs, uint64_t nowMs) {
    if (nowMs <= startMs) {
        return 1.0f;
    }
    const uint64_t elapsed = nowMs - startMs;
    if (elapsed >= kToastVisibleMs) {
        return 0.0f;
    }
    const uint64_t fadeStart = kToastVisibleMs - kToastFadeMs;
    if (elapsed < fadeStart) {
        return 1.0f;
    }
    return static_cast<float>(kToastVisibleMs - elapsed) / static_cast<float>(kToastFadeMs);
}

bool ToastExpired(uint64_t startMs, uint64_t nowMs) {
    return nowMs > startMs && (nowMs - startMs) >= kToastVisibleMs;
}

bool ToastNeedsRepresent(uint64_t nowMs, uint64_t lastPresentMs, bool expired, bool lastFrameHadToast) {
    if (expired) {
        return lastFrameHadToast;
    }
    return nowMs >= lastPresentMs && (nowMs - lastPresentMs) >= kToastTimerMs;
}

int ToastFontHeight(int clientH) {
    return std::clamp(clientH / 30, 12, 96);
}

int ToastMinFontHeight(int clientH) {
    return std::clamp(clientH / 54, 10, 64);
}

int ToastPadding(int clientH) {
    return std::max(4, clientH / 108);
}

int ToastRightMargin(int clientW, const ToastRect& image) {
    return std::max(0, clientW - (image.x + image.width));
}

int ToastMarginFontHeight(int baseFontH, int textW, int marginW, int padding, int minFontH) {
    const int available = marginW - 2 * padding;
    if (available <= 0 || textW <= 0 || textW <= available) {
        return 0;
    }
    const int shrunk = static_cast<int>(static_cast<int64_t>(baseFontH) * available / textW);
    return (shrunk >= minFontH) ? shrunk : 0;
}

ToastPlacement ComputeToastPlacement(int clientW, int clientH, const ToastRect& image, int textW, int textH,
                                     int padding) {
    ToastPlacement p;
    const int imageRight = image.x + image.width;
    const int imageBottom = image.y + image.height;
    const int marginW = ToastRightMargin(clientW, image);
    if (marginW >= textW + 2 * padding) {
        p.inMargin = true;
        p.drawBackground = false;
        p.text = {imageRight + (marginW - textW) / 2, clientH - padding - textH, textW, textH};
        return p;
    }

    const int inner = std::max(2, padding / 2);
    ToastRect bg;
    bg.width = textW + 2 * inner;
    bg.height = textH + 2 * inner;
    bg.x = std::max(image.x, imageRight - padding - bg.width);
    bg.y = std::max(image.y, imageBottom - padding - bg.height);
    p.inMargin = false;
    p.drawBackground = true;
    p.background = bg;
    p.text = {bg.x + inner, bg.y + inner, textW, textH};
    return p;
}

bool RasterizeToastText(const wchar_t* text, int fontHeight, ToastBitmap& out) {
    out = ToastBitmap{};
    if (text == nullptr || text[0] == L'\0' || fontHeight <= 0) {
        return false;
    }
    const int len = static_cast<int>(std::wcslen(text));

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        return false;
    }
    HFONT font = CreateFontW(-fontHeight, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             VARIABLE_PITCH | FF_SWISS, L"Arial");
    if (!font) {
        DeleteDC(dc);
        return false;
    }
    HGDIOBJ oldFont = SelectObject(dc, font);

    SIZE extent = {};
    bool ok = GetTextExtentPoint32W(dc, text, len, &extent) != 0 && extent.cx > 0 && extent.cy > 0;
    // One pixel of slack on each side for antialiasing that spills past the advance width.
    const int width = ok ? extent.cx + 2 : 0;
    const int height = ok ? extent.cy : 0;

    void* bits = nullptr;
    HBITMAP dib = nullptr;
    if (ok) {
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;  // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        dib = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ok = dib != nullptr && bits != nullptr;
    }
    if (ok) {
        HGDIOBJ oldBitmap = SelectObject(dc, dib);
        const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
        std::memset(bits, 0, count * sizeof(uint32_t));  // black
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutW(dc, 1, 0, text, len);
        GdiFlush();

        // White text on black: any channel is the glyph coverage (grayscale antialiasing); take the
        // maximum in case the driver still produced colour fringes.
        const auto* src = static_cast<const uint32_t*>(bits);
        out.pixels.resize(count);
        for (size_t i = 0; i < count; ++i) {
            const uint32_t px = src[i];
            const uint32_t cov = std::max({px & 0xFFu, (px >> 8) & 0xFFu, (px >> 16) & 0xFFu});
            out.pixels[i] = (cov << 24) | 0x00FFFFFFu;
        }
        out.width = width;
        out.height = height;
        SelectObject(dc, oldBitmap);
    }

    if (dib) {
        DeleteObject(dib);
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteDC(dc);
    return ok;
}
