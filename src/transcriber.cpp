#include "transcriber.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

namespace Transcriber {

namespace {

// RAII wrappers so every curl resource is freed on all paths, including errors.
struct CurlEasyDeleter {
    void operator()(CURL* c) const { if (c) curl_easy_cleanup(c); }
};
struct CurlMimeDeleter {
    void operator()(curl_mime* m) const { if (m) curl_mime_free(m); }
};
struct CurlSlistDeleter {
    void operator()(curl_slist* s) const { if (s) curl_slist_free_all(s); }
};

using CurlEasy  = std::unique_ptr<CURL,        CurlEasyDeleter>;
using CurlMime  = std::unique_ptr<curl_mime,   CurlMimeDeleter>;
using CurlSlist = std::unique_ptr<curl_slist,  CurlSlistDeleter>;

struct Failure {
    std::string message;
    bool retryable;
};

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* resp = static_cast<std::string*>(userdata);
    resp->append(ptr, size * nmemb);
    return size * nmemb;
}

// Returning non-zero aborts the transfer (curl reports CURLE_ABORTED_BY_CALLBACK).
int xfer_callback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto* flag = static_cast<const std::atomic<bool>*>(clientp);
    return (flag && flag->load(std::memory_order_acquire)) ? 1 : 0;
}

std::expected<std::string, Failure> do_request(const Request& req) {
    CurlEasy curl(curl_easy_init());
    if (!curl) {
        return std::unexpected(Failure{"curl_easy_init failed", false});
    }

    std::string auth_header = "Authorization: Bearer " + req.api_key;
    CurlSlist headers(curl_slist_append(nullptr, auth_header.c_str()));
    if (!headers) {
        return std::unexpected(Failure{"failed to build request headers", false});
    }

    CurlMime mime(curl_mime_init(curl.get()));
    if (!mime) {
        return std::unexpected(Failure{"curl_mime_init failed", false});
    }

    // Part: file (curl copies the data, so wav_bytes can be freed afterwards).
    curl_mimepart* part = curl_mime_addpart(mime.get());
    curl_mime_name(part, "file");
    curl_mime_data(part, reinterpret_cast<const char*>(req.wav_bytes.data()),
                   req.wav_bytes.size());
    curl_mime_filename(part, "audio.wav");
    curl_mime_type(part, "audio/wav");

    // Part: model
    part = curl_mime_addpart(mime.get());
    curl_mime_name(part, "model");
    curl_mime_data(part, req.model.c_str(), CURL_ZERO_TERMINATED);

    // Part: response_format (default json)
    part = curl_mime_addpart(mime.get());
    curl_mime_name(part, "response_format");
    curl_mime_data(part, "json", CURL_ZERO_TERMINATED);

    // Part: prompt (omitted when empty, e.g. on the first chunk)
    if (!req.prompt.empty()) {
        part = curl_mime_addpart(mime.get());
        curl_mime_name(part, "prompt");
        curl_mime_data(part, req.prompt.c_str(), CURL_ZERO_TERMINATED);
    }

    std::string body;
    curl_easy_setopt(curl.get(), CURLOPT_URL,
                     "https://api.openai.com/v1/audio/transcriptions");
    curl_easy_setopt(curl.get(), CURLOPT_MIMEPOST, mime.get());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);

    if (req.abort_flag) {
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, xfer_callback);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA,
                         const_cast<std::atomic<bool>*>(req.abort_flag));
    }

    CURLcode res = curl_easy_perform(curl.get());
    if (res == CURLE_ABORTED_BY_CALLBACK) {
        return std::unexpected(Failure{"aborted", false});
    }
    if (res != CURLE_OK) {
        return std::unexpected(Failure{std::string("curl error: ") +
                                       curl_easy_strerror(res), true});
    }

    long http_code = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        bool retryable = (http_code == 429 || http_code >= 500);
        std::string msg = "HTTP " + std::to_string(http_code);
        if (!body.empty()) msg += ": " + body.substr(0, 300);
        return std::unexpected(Failure{msg, retryable});
    }

    try {
        auto j = nlohmann::json::parse(body);
        if (!j.contains("text") || !j["text"].is_string()) {
            return std::unexpected(Failure{"response missing 'text' field", false});
        }
        return j["text"].get<std::string>();
    } catch (const nlohmann::json::exception& e) {
        return std::unexpected(Failure{std::string("JSON parse error: ") + e.what(), false});
    }
}

} // anonymous namespace

std::expected<std::string, std::string> transcribe(const Request& req) {
    constexpr int kMaxAttempts = 3;
    std::string last_error = "unknown error";

    auto should_stop = [&]() -> bool {
        return req.should_continue && !(*req.should_continue)();
    };
    auto interruptible_sleep = [&](int ms) {
        constexpr int step = 50;
        while (ms > 0) {
            if (should_stop()) return;
            int slice = std::min(step, ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            ms -= slice;
        }
    };

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (attempt > 0) {
            if (should_stop()) break;
            interruptible_sleep(1000 * (1 << (attempt - 1))); // 1s, then 2s
            if (should_stop()) break;
        }

        auto result = do_request(req);
        if (result) return *result;

        last_error = result.error().message;
        if (!result.error().retryable) break;
    }

    return std::unexpected(last_error);
}

} // namespace Transcriber
