#include "vcam/source/frame_barcode.hpp"

#include <algorithm>
#include <cstring>

namespace vcam {

namespace {

uint8_t checksum(uint32_t value24) {
    const auto b0 = static_cast<uint8_t>(value24 & 0xFF);
    const auto b1 = static_cast<uint8_t>((value24 >> 8) & 0xFF);
    const auto b2 = static_cast<uint8_t>((value24 >> 16) & 0xFF);
    return static_cast<uint8_t>(b0 ^ b1 ^ b2 ^ 0xA5);
}

uint32_t encode(uint32_t frame_number) {
    const uint32_t value24 = frame_number & 0xFFFFFF;
    return (value24 << 8) | checksum(value24);
}

}  // namespace

BarcodeLayout barcode_layout(int width, int height) {
    BarcodeLayout layout;
    layout.block_width = width / kBarcodeBits;
    layout.band_height = std::max(4, height / 16);
    return layout;
}

void draw_barcode_rgb(uint8_t* rgb, size_t stride, int width, int height, uint32_t frame_number) {
    const BarcodeLayout layout = barcode_layout(width, height);
    if (layout.block_width < 2) {
        return;  // too narrow for a readable code
    }
    const uint32_t code = encode(frame_number);
    for (int y = 0; y < std::min(layout.band_height, height); ++y) {
        uint8_t* row = rgb + static_cast<size_t>(y) * stride;
        for (int bit = 0; bit < kBarcodeBits; ++bit) {
            const bool one = ((code >> (kBarcodeBits - 1 - bit)) & 1u) != 0;
            const uint8_t value = one ? 255 : 0;
            const size_t start = static_cast<size_t>(bit * layout.block_width) * 3;
            std::memset(row + start, value, static_cast<size_t>(layout.block_width) * 3);
        }
    }
}

uint8_t luma_at(const FrameView& frame, int x, int y) {
    const FrameFormat& format = frame.format;
    const size_t stride = format.planes[0].stride_bytes;
    const uint8_t* row = frame.plane(0) + static_cast<size_t>(y) * stride;
    const auto ux = static_cast<size_t>(x);
    switch (format.pixel_format) {
        case PixelFormat::YUYV:  return row[ux * 2];      // Y0 U Y1 V
        case PixelFormat::UYVY:  return row[ux * 2 + 1];  // U Y0 V Y1
        case PixelFormat::I420:
        case PixelFormat::NV12:
        case PixelFormat::GRAY8: return row[ux];          // luma plane
        case PixelFormat::RGB24:
        case PixelFormat::BGR24: {
            // Rough luma: (R + 2G + B) / 4 is enough to tell black from white.
            const uint8_t* pixel = row + ux * 3;
            return static_cast<uint8_t>((pixel[0] + 2 * pixel[1] + pixel[2]) / 4);
        }
    }
    return 0;
}

std::optional<uint32_t> read_barcode(const FrameView& frame) {
    const BarcodeLayout layout = barcode_layout(frame.format.width, frame.format.height);
    if (layout.block_width < 2) {
        return std::nullopt;
    }
    const int y = layout.band_height / 2;
    uint32_t code = 0;
    for (int bit = 0; bit < kBarcodeBits; ++bit) {
        const int x = bit * layout.block_width + layout.block_width / 2;  // block centre
        code = (code << 1) | (luma_at(frame, x, y) >= 128 ? 1u : 0u);
    }
    const uint32_t value24 = code >> 8;
    if ((code & 0xFF) != checksum(value24)) {
        return std::nullopt;
    }
    return value24;
}

}  // namespace vcam
