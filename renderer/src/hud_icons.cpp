#include "hud_icons.h"

namespace {

// white - black of one 8-bit channel, clamped at 0 for a malformed pair.
uint32_t ChannelDiff(uint32_t black, uint32_t white, int shift) {
    const uint32_t b = (black >> shift) & 0xFFu;
    const uint32_t w = (white >> shift) & 0xFFu;
    return w > b ? w - b : 0u;
}

}  // namespace

size_t RecoverHudIconLayer(const uint32_t* onBlack, const uint32_t* onWhite, uint32_t* out, size_t pixelCount) {
    size_t visible = 0;
    for (size_t i = 0; i < pixelCount; ++i) {
        const uint32_t black = onBlack[i];
        const uint32_t white = onWhite[i];
        uint32_t diff = ChannelDiff(black, white, 0);
        const uint32_t diffG = ChannelDiff(black, white, 8);
        const uint32_t diffR = ChannelDiff(black, white, 16);
        if (diffG > diff) {
            diff = diffG;
        }
        if (diffR > diff) {
            diff = diffR;
        }
        const uint32_t alpha = 255u - diff;
        if (alpha == 0) {
            out[i] = 0;
            continue;
        }
        out[i] = (alpha << 24) | (black & 0x00FFFFFFu);
        ++visible;
    }
    return visible;
}
