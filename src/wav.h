#pragma once
#include <cstdint>
#include <vector>

namespace Wav {

// Encode a block of 32-bit float PCM samples (mono, 16000 Hz) into a
// standard RIFF WAV file in memory, using 16-bit signed PCM.
// Returns the raw bytes ready to upload.
std::vector<uint8_t> encode_f32_to_wav(const float* samples, size_t count,
                                        uint32_t sample_rate = 16000,
                                        uint16_t channels    = 1);

} // namespace Wav
