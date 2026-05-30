#include "media_decoder.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <string>
#include <vector>

namespace MediaDecoder {

namespace {

constexpr unsigned kTargetSampleRate = 16000;
constexpr unsigned kTargetChannels   = 1;
constexpr unsigned kTargetBits       = 16;

// Minimal RAII helpers so every COM/MF resource is released on all paths.
template <typename T>
struct ComPtr {
    T* p{nullptr};
    ComPtr() = default;
    ~ComPtr() { if (p) p->Release(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T** put() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
    void reset() { if (p) { p->Release(); p = nullptr; } }
};

struct MfRuntime {
    bool com_ok{false};
    bool mf_ok{false};
    ~MfRuntime() {
        if (mf_ok) MFShutdown();
        if (com_ok) CoUninitialize();
    }
};

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

} // namespace

std::expected<void, std::string> decode(const std::string& path,
                                        const SampleSink& sink,
                                        const std::function<bool()>& keep_going) {
    MfRuntime rt;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return std::unexpected("COM init failed");
    }
    rt.com_ok = SUCCEEDED(hr);

    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        return std::unexpected("Media Foundation startup failed");
    }
    rt.mf_ok = true;

    std::wstring wpath = widen(path);

    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromURL(wpath.c_str(), nullptr, reader.put());
    if (FAILED(hr)) {
        return std::unexpected("Could not open the media file (unsupported or missing codec)");
    }

    // Select only the first audio stream.
    reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    hr = reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    if (FAILED(hr)) {
        return std::unexpected("The file has no audio track");
    }

    // Ask the reader to deliver 16 kHz mono 16-bit PCM; MF inserts the decoder
    // and resampler transforms needed to satisfy this.
    ComPtr<IMFMediaType> target;
    hr = MFCreateMediaType(target.put());
    if (FAILED(hr)) return std::unexpected("Failed to allocate media type");
    target->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    target->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    target->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, kTargetBits);
    target->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kTargetSampleRate);
    target->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kTargetChannels);

    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, target.p);
    if (FAILED(hr)) {
        return std::unexpected("Could not configure PCM output for this audio track");
    }
    reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

    std::vector<float> scratch;
    bool produced_any = false;

    while (true) {
        if (keep_going && !keep_going()) break;

        DWORD stream_flags = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr,
                                &stream_flags, nullptr, sample.put());
        if (FAILED(hr)) {
            return std::unexpected("Error while decoding the audio track");
        }
        if (stream_flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) continue; // gap or stream tick with no data

        ComPtr<IMFMediaBuffer> buffer;
        hr = sample->ConvertToContiguousBuffer(buffer.put());
        if (FAILED(hr)) continue;

        BYTE* data = nullptr;
        DWORD max_len = 0, cur_len = 0;
        hr = buffer->Lock(&data, &max_len, &cur_len);
        if (FAILED(hr)) continue;

        const size_t sample_count = cur_len / sizeof(int16_t);
        const int16_t* pcm = reinterpret_cast<const int16_t*>(data);
        scratch.clear();
        scratch.reserve(sample_count);
        for (size_t i = 0; i < sample_count; ++i) {
            scratch.push_back(static_cast<float>(pcm[i]) / 32768.0f);
        }
        buffer->Unlock();

        if (!scratch.empty()) {
            sink(scratch.data(), scratch.size());
            produced_any = true;
        }
    }

    if (!produced_any) {
        return std::unexpected("No audio could be decoded from the file");
    }
    return {};
}

} // namespace MediaDecoder
