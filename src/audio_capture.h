#pragma once
#include <expected>
#include <memory>
#include <string>
#include "app_state.h"

// Owns a miniaudio loopback device. Samples are pushed into AppState::ring
// directly from the audio callback. The miniaudio device lives in an opaque
// Impl so this header stays free of miniaudio's large set of macros and types.
class AudioCapture {
public:
    explicit AudioCapture(AppState& state);
    ~AudioCapture();

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    std::expected<void, std::string> start();
    void stop();
    bool is_running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
