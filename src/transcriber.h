#pragma once
#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <vector>

// POST audio bytes as multipart/form-data to the OpenAI transcription endpoint.
// Each call is synchronous; run it from the worker thread.
namespace Transcriber {

struct Request {
    std::vector<uint8_t> wav_bytes; // complete WAV file in memory
    std::string api_key;
    std::string model;
    std::string prompt;             // optional: tail of the previous transcript

    // Optional: return false to stop issuing further retry attempts.
    const std::function<bool()>* should_continue = nullptr;
    // Optional: when this becomes true, abort an in-flight transfer (used on shutdown).
    const std::atomic<bool>* abort_flag = nullptr;
};

// Returns the transcribed text, or an error string. Retries transient failures
// (curl errors, HTTP 429, HTTP 5xx) with exponential backoff, up to 3 attempts.
std::expected<std::string, std::string> transcribe(const Request& req);

} // namespace Transcriber
