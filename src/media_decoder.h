#pragma once
#include <expected>
#include <functional>
#include <string>

// Decodes the audio track of any media file Windows can open (mp4, mov, mkv,
// avi, wmv, m4a, mp3, wav, flac, webm, ...) using Media Foundation, resampling
// to 16 kHz mono float PCM. No external binaries or codecs are bundled; this
// relies on the codecs already present on Windows 10/11.
namespace MediaDecoder {

// Called repeatedly with decoded, normalized f32 samples [-1, 1].
using SampleSink = std::function<void(const float* samples, size_t count)>;

// Decode `path` and stream samples to `sink`. `keep_going` is polled between
// reads; returning false stops decoding early (used for Cancel / shutdown).
// Returns an error string on failure.
std::expected<void, std::string> decode(const std::string& path,
                                        const SampleSink& sink,
                                        const std::function<bool()>& keep_going);

} // namespace MediaDecoder
