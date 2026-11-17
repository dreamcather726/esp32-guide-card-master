#pragma once

#include <cstddef>
#include <cstdint>

namespace startup_qr {

constexpr uint16_t kMaxJpegDimension = 1024;

struct JpegDimensions {
    uint16_t width = 0;
    uint16_t height = 0;
};

// Inspect bounded JPEG header segments before allocating a decoded pixel buffer.
// This does not decode the image or verify entropy-coded data: the caller must
// still run the JPEG decoder before accepting an image. On failure, dimensions
// are cleared. Progressive SOF headers are recognized; decoder support remains
// the decoder's responsibility.
inline bool ReadJpegDimensions(const uint8_t* data, size_t size,
                               JpegDimensions& dimensions) {
    dimensions = {};
    if (data == nullptr || size < 4 || data[0] != 0xff || data[1] != 0xd8) {
        return false;
    }

    size_t offset = 2;
    while (offset < size) {
        if (data[offset++] != 0xff) {
            return false;
        }
        // JPEG permits FF fill bytes between markers.
        while (offset < size && data[offset] == 0xff) {
            ++offset;
        }
        if (offset == size) {
            return false;
        }
        const uint8_t marker = data[offset++];
        if (marker == 0x01) {  // TEM has no segment length.
            continue;
        }
        // SOF must precede SOS. Never interpret compressed scan bytes as
        // headers; stuffed bytes and restart markers belong inside a scan.
        if (marker == 0x00 || marker == 0xd8 || marker == 0xd9 ||
            marker == 0xda || (marker >= 0xd0 && marker <= 0xd7)) {
            return false;
        }
        if (size - offset < 2) {
            return false;
        }
        const size_t length = (static_cast<size_t>(data[offset]) << 8) |
                              data[offset + 1];
        if (length < 2 || length > size - offset) {
            return false;
        }

        // C4 is DHT, C8 is JPG and CC is DAC; all other C0..CF markers
        // are start-of-frame variants with the same dimensions layout.
        const bool is_sof = marker >= 0xc0 && marker <= 0xcf &&
                            marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
        if (is_sof) {
            if (length < 8) {
                return false;
            }
            const uint8_t component_count = data[offset + 7];
            if (component_count == 0 ||
                length != 8U + 3U * component_count) {
                return false;
            }
            const uint16_t height = static_cast<uint16_t>(
                (static_cast<uint16_t>(data[offset + 3]) << 8) | data[offset + 4]);
            const uint16_t width = static_cast<uint16_t>(
                (static_cast<uint16_t>(data[offset + 5]) << 8) | data[offset + 6]);
            if (width == 0 || height == 0 || width > kMaxJpegDimension ||
                height > kMaxJpegDimension) {
                return false;
            }
            dimensions = {width, height};
            return true;
        }
        offset += length;
    }
    return false;
}

}  // namespace startup_qr
