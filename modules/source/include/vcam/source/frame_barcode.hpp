// =============================================================================
// frame_barcode.hpp — stamp a frame number into the picture and read it back
//
// The test-pattern source draws a 32-bit barcode across the top of every
// frame: 24 bits of frame number + an 8-bit checksum. A consumer (vcam_probe,
// the end-to-end tests) reads the barcode from what it received and can then
// prove, frame by frame, that nothing was dropped, duplicated or reordered on
// the way through the virtual camera.
//
// Layout: 32 equal-width blocks across the top band, most significant bit
// first, white = 1, black = 0. Only luma is sampled, at block centres, so it
// survives any pixel-format conversion.
// =============================================================================
#pragma once

#include <cstdint>
#include <optional>

#include "vcam/core/frame.hpp"

namespace vcam {

constexpr int kBarcodeBits = 32;
constexpr int kBarcodeMinWidth = 64;  // 2 pixels per block at least

struct BarcodeLayout {
    int block_width = 0;
    int band_height = 0;
};

BarcodeLayout barcode_layout(int width, int height);

// Draws `frame_number` (lower 24 bits) into an RGB24 image.
void draw_barcode_rgb(uint8_t* rgb, size_t stride, int width, int height, uint32_t frame_number);

// Approximate luma (0..255) of pixel (x, y) in any supported pixel format.
uint8_t luma_at(const FrameView& frame, int x, int y);

// Reads the barcode. nullopt if the frame has no valid barcode (checksum fails).
std::optional<uint32_t> read_barcode(const FrameView& frame);

}  // namespace vcam
