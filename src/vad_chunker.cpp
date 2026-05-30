#include "vad_chunker.h"
#include <algorithm>
#include <cmath>

VadChunker::VadChunker(AppState& state,
                       std::function<void(std::vector<float>)> chunk_callback)
    : state_(state), cb_(std::move(chunk_callback)) {
    read_buf_.resize(8192);
    chunk_.reserve(static_cast<size_t>(max_chunk_samples));
}

static float frame_rms(const float* samples, int count) {
    if (count <= 0) return 0.0f;
    double sum = 0.0;
    for (int i = 0; i < count; ++i) {
        sum += static_cast<double>(samples[i]) * samples[i];
    }
    return static_cast<float>(std::sqrt(sum / static_cast<double>(count)));
}

void VadChunker::process_available() {
    while (true) {
        size_t got = state_.ring.pop(read_buf_.data(), read_buf_.size());
        if (got == 0) break;
        feed(read_buf_.data(), got);
    }
}

void VadChunker::feed(const float* samples, size_t count) {
    // Keep every sample: the analysis below never removes audio from chunk_.
    chunk_.insert(chunk_.end(), samples, samples + count);
    analyze_and_maybe_emit();
}

void VadChunker::analyze_and_maybe_emit() {
    const size_t frame = static_cast<size_t>(frame_samples);

    // Analyze each complete frame we have not looked at yet.
    while (chunk_.size() - analyzed_ >= frame) {
        float rms = frame_rms(chunk_.data() + analyzed_, frame_samples);
        analyzed_ += frame;

        if (rms >= silence_threshold) {
            in_speech_ = true;
            had_speech_ = true;
            silence_frame_count_ = 0;
        } else if (in_speech_) {
            ++silence_frame_count_;
        }

        bool cut_on_silence = in_speech_ && silence_frame_count_ >= silence_frames_to_cut;
        bool cut_on_max     = chunk_.size() >= static_cast<size_t>(max_chunk_samples);
        if (cut_on_silence || cut_on_max) {
            emit_chunk();
        }
    }

    // Enforce the hard cap even if the tail does not land on a frame boundary.
    if (chunk_.size() >= static_cast<size_t>(max_chunk_samples)) {
        emit_chunk();
    }
}

void VadChunker::emit_chunk() {
    // Only pay to transcribe chunks that actually contain speech.
    if (had_speech_ && !chunk_.empty()) {
        cb_(std::move(chunk_));
        chunk_ = std::vector<float>();
    } else {
        chunk_.clear();
    }
    chunk_.reserve(static_cast<size_t>(max_chunk_samples));
    analyzed_ = 0;
    silence_frame_count_ = 0;
    in_speech_ = false;
    had_speech_ = false;
}

void VadChunker::flush() {
    analyze_and_maybe_emit(); // settle any complete frames first
    emit_chunk();             // emit the trailing partial chunk if it has speech
}
