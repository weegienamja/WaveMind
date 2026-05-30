#pragma once
#include <cstddef>
#include <functional>
#include <vector>
#include "app_state.h"

// Pulls samples from the ring buffer, applies energy-based VAD, and emits
// complete audio chunks (the full f32 PCM, not just the analysis window) via a
// callback. Runs entirely on the worker thread.
class VadChunker {
public:
    explicit VadChunker(AppState& state,
                        std::function<void(std::vector<float>)> chunk_callback);

    // Drains the ring buffer and runs VAD, emitting chunks as boundaries are hit.
    // Used by live capture (the audio callback fills the ring).
    void process_available();

    // Feed samples directly (used by file decoding, which does not go through the
    // ring buffer). Runs the same VAD and emits chunks as boundaries are hit.
    void feed(const float* samples, size_t count);

    // Emit whatever speech is buffered as a final chunk (called on a clean Stop).
    void flush();

    // Tunable parameters (RMS is computed over [-1, 1] normalized samples).
    float silence_threshold{0.01f}; // frames below this RMS count as silence
    int   frame_samples{480};       // 30 ms at 16 kHz
    int   silence_frames_to_cut{20}; // 20 * 30 ms = 600 ms of trailing silence
    int   max_chunk_samples{480000}; // 30 s at 16 kHz

private:
    void analyze_and_maybe_emit();
    void emit_chunk();

    AppState& state_;
    std::function<void(std::vector<float>)> cb_;

    std::vector<float> chunk_;    // ALL audio for the current chunk (this is what gets sent)
    std::vector<float> read_buf_; // scratch buffer for ring reads
    size_t analyzed_{0};          // how many samples of chunk_ have been VAD-analyzed
    int    silence_frame_count_{0};
    bool   in_speech_{false};
    bool   had_speech_{false};    // chunk contains at least one speech frame
};
