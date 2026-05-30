#include "image_loader.h"
#include "resource.h"

#include <windows.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb_image.h>

namespace ImageLoader {

namespace {
Rgba decode_rgba(const unsigned char* data, int len) {
    Rgba out;
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(data, len, &w, &h, &channels, 4);
    if (!pixels) return out;
    out.width = w;
    out.height = h;
    out.pixels.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
    stbi_image_free(pixels);
    return out;
}
} // namespace

Rgba load_embedded_logo() {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(IDR_LOGO_PNG), RT_RCDATA);
    if (!res) return {};
    HGLOBAL handle = LoadResource(module, res);
    if (!handle) return {};
    const void* data = LockResource(handle);
    DWORD size = SizeofResource(module, res);
    if (!data || size == 0) return {};
    return decode_rgba(static_cast<const unsigned char*>(data), static_cast<int>(size));
}

Rgba load_from_file(const char* path) {
    Rgba out;
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path, &w, &h, &channels, 4);
    if (!pixels) return out;
    out.width = w;
    out.height = h;
    out.pixels.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
    stbi_image_free(pixels);
    return out;
}

} // namespace ImageLoader
