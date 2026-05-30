#pragma once
#include <cstdint>
#include <vector>

// Decodes images to 8-bit RGBA. Used for the in-app logo texture and the GLFW
// window icon. The logo is embedded in the executable as a Windows RCDATA
// resource so the app stays a single self-contained file.
namespace ImageLoader {

struct Rgba {
    int width{0};
    int height{0};
    std::vector<uint8_t> pixels; // width * height * 4, RGBA8
    bool valid() const { return width > 0 && height > 0 && !pixels.empty(); }
};

// Load the embedded logo (RCDATA resource IDR_LOGO_PNG) and decode to RGBA.
Rgba load_embedded_logo();

// Decode an arbitrary image file from disk to RGBA (fallback / general use).
Rgba load_from_file(const char* path);

} // namespace ImageLoader
