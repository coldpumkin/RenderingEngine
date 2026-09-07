#include "Bmp.h"

#include <cstdio>
#include <vector>

// Not Vulkan/Core.h's LOG. This file follows a file format and knows nothing about the
// API, so it prints the way that macro does rather than including the layer for it.
#define LOG(...)  std::fprintf(stderr, __VA_ARGS__)

static void Put32(uint8_t* at, uint32_t value) noexcept {
    at[0] = static_cast<uint8_t>(value);
    at[1] = static_cast<uint8_t>(value >> 8);
    at[2] = static_cast<uint8_t>(value >> 16);
    at[3] = static_cast<uint8_t>(value >> 24);
}

// A 24-bit BMP: a 54-byte header, then rows bottom-up with each padded to 4 bytes.
// Written by hand rather than vendoring an encoder for one debug path, and BMP rather
// than PPM because Windows opens it without asking what it is.
//
// Input: rgba is width * height * 4, top row first, red first
bool WriteBmp(const char* path, uint32_t width, uint32_t height,
                     const uint8_t* rgba) noexcept {
    const uint32_t rowBytes = width * 3;
    const uint32_t pad = (4 - (rowBytes % 4)) % 4;
    const uint32_t imageBytes = (rowBytes + pad) * height;

    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) {
        LOG("[capture] cannot write %s\n", path);
        return false;
    }

    uint8_t header[54]{};
    header[0] = 'B';
    header[1] = 'M';
    Put32(header + 2, 54 + imageBytes);   // file size
    Put32(header + 10, 54);               // where the pixels start
    Put32(header + 14, 40);               // DIB header size
    Put32(header + 18, width);
    Put32(header + 22, height);
    header[26] = 1;                       // planes
    header[28] = 24;                      // bits per pixel
    Put32(header + 34, imageBytes);
    std::fwrite(header, 1, sizeof(header), file);

    std::vector<uint8_t> row(rowBytes + pad, 0);
    for (uint32_t y = 0; y < height; ++y) {
        // BMP counts rows from the bottom, and stores them as B, G, R.
        const uint8_t* src = rgba + static_cast<size_t>(height - 1 - y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            row[x * 3 + 0] = src[x * 4 + 2];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 0];
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
    return true;
}
