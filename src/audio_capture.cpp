#include "audio_capture.h"

// The miniaudio single-header implementation is compiled exactly once, here.
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

struct AudioCapture::Impl {
    ma_device device{};
    AppState* state{nullptr};
    bool initialized{false};
};

namespace {
// pUserData is an AppState* (a public type) so this free function does not need
// to name AudioCapture's private Impl.
void audio_data_callback(ma_device* device, void* /*output*/,
                         const void* input, ma_uint32 frame_count) {
    auto* state = static_cast<AppState*>(device->pUserData);
    if (!state || !input) return;
    // Mono f32, so frame_count == sample_count. Push only; never block or allocate.
    state->ring.push(static_cast<const float*>(input),
                     static_cast<size_t>(frame_count));
}
} // namespace

AudioCapture::AudioCapture(AppState& state) : impl_(std::make_unique<Impl>()) {
    impl_->state = &state;
}

// Defined here (not =default in the header) so unique_ptr<Impl> sees a complete Impl.
AudioCapture::~AudioCapture() {
    stop();
}

std::expected<void, std::string> AudioCapture::start() {
    if (impl_->initialized) return {};

    ma_device_config cfg = ma_device_config_init(ma_device_type_loopback);
    cfg.capture.format   = ma_format_f32;
    cfg.capture.channels = 1;
    cfg.sampleRate       = 16000;
    cfg.dataCallback     = audio_data_callback;
    cfg.pUserData        = impl_->state;

    ma_result res = ma_device_init(nullptr, &cfg, &impl_->device);
    if (res != MA_SUCCESS) {
        return std::unexpected(std::string("ma_device_init failed: ") +
                               ma_result_description(res));
    }

    res = ma_device_start(&impl_->device);
    if (res != MA_SUCCESS) {
        ma_device_uninit(&impl_->device);
        return std::unexpected(std::string("ma_device_start failed: ") +
                               ma_result_description(res));
    }

    impl_->initialized = true;
    return {};
}

void AudioCapture::stop() {
    if (!impl_->initialized) return;
    ma_device_uninit(&impl_->device); // stops the device, then frees it
    impl_->initialized = false;
}

bool AudioCapture::is_running() const {
    if (!impl_->initialized) return false;
    return ma_device_is_started(&impl_->device) == MA_TRUE;
}
