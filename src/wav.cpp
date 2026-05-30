#include "wav.h"
#include <algorithm>

namespace Wav {

static void write_le16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

static void write_le32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

static void write_tag(std::vector<uint8_t>& buf, const char tag[4]) {
    buf.push_back(static_cast<uint8_t>(tag[0]));
    buf.push_back(static_cast<uint8_t>(tag[1]));
    buf.push_back(static_cast<uint8_t>(tag[2]));
    buf.push_back(static_cast<uint8_t>(tag[3]));
}

std::vector<uint8_t> encode_f32_to_wav(const float* samples, size_t count,
                                        uint32_t sample_rate, uint16_t channels) {
    const uint16_t bits_per_sample = 16;
    const uint32_t byte_rate       = sample_rate * channels * (bits_per_sample / 8);
    const uint16_t block_align     = static_cast<uint16_t>(channels * (bits_per_sample / 8));
    const uint32_t data_size       = static_cast<uint32_t>(count) * block_align;
    const uint32_t riff_size       = 36 + data_size;

    std::vector<uint8_t> buf;
    buf.reserve(44 + data_size);

    // RIFF header
    write_tag(buf, "RIFF");
    write_le32(buf, riff_size);
    write_tag(buf, "WAVE");

    // fmt chunk
    write_tag(buf, "fmt ");
    write_le32(buf, 16);           // chunk size
    write_le16(buf, 1);            // PCM format
    write_le16(buf, channels);
    write_le32(buf, sample_rate);
    write_le32(buf, byte_rate);
    write_le16(buf, block_align);
    write_le16(buf, bits_per_sample);

    // data chunk
    write_tag(buf, "data");
    write_le32(buf, data_size);

    // Convert f32 -> int16
    for (size_t i = 0; i < count; ++i) {
        float clamped = std::clamp(samples[i], -1.0f, 1.0f);
        auto s = static_cast<int16_t>(clamped * 32767.0f);
        buf.push_back(static_cast<uint8_t>(s & 0xFF));
        buf.push_back(static_cast<uint8_t>((s >> 8) & 0xFF));
    }

    return buf;
}

} // namespace Wav
