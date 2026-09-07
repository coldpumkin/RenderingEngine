#pragma once

// Writing a frame to a file
// ============================================================================
//
// Here rather than in main because what it follows is the BMP format, which changes for
// reasons that have nothing to do with a frame. It is the other half of
// ReadTexturePixels: that one hands back rows of RGBA, this one puts them on disk.

#include <cstdint>

// Input:  rgba is width * height * 4 bytes, top row first
// Output: false if the file cannot be written
//
// Contract: the caller has checked that the source is red-first. A BMP stores blue
//           first, and the swap happens here, but nothing here can tell which order the
//           bytes arrived in.
bool WriteBmp(const char* path, uint32_t width, uint32_t height,
              const uint8_t* rgba) noexcept;
