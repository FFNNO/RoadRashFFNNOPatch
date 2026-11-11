#pragma once
#include <cstddef>
#include <cstdint>

// HUD key icons (\x01-\x05 in race messages, e.g. the ESC key in "Hit [ESC] again to quit") are
// 32-bit premultiplied images that the game blends into its 32bpp DIB with FUN_00464140:
//   alpha 0: pixel untouched; alpha 255: colour copied;
//   otherwise each channel = min(255, dst * (255 - a) / 255 + src).
// With GpuWorld the DIB is not composed, so the icons are recovered as their own layer by running
// FUN_00464140 over a buffer prefilled with black and another prefilled with white:
//   over black the result is the premultiplied colour, and white - black = 255 - a per channel.
//
// RecoverHudIconLayer turns such a pair into premultiplied 0xAARRGGBB pixels (B in the low byte,
// matching DXGI_FORMAT_B8G8R8A8_UNORM), to be drawn with blend (ONE, INV_SRC_ALPHA), which
// reproduces the game's blend over any background. The top byte of the inputs is ignored. The
// alpha comes from the channel with the largest white - black difference, because a channel the
// game saturated at 255 in the white run understates the difference. Transparent pixels are 0.
// Returns the number of pixels with non-zero alpha.
size_t RecoverHudIconLayer(const uint32_t* onBlack, const uint32_t* onWhite, uint32_t* out, size_t pixelCount);
