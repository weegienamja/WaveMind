#pragma once
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// Ring buffer for audio samples shared between the miniaudio callback (single
// producer) and the worker thread (single consumer). Lock-free via atomic
// indices so the audio callback never blocks.
class AudioRingBuffer {
public:
    static constexpr size_t kCapacity = 16 * 1024 * 1024 / sizeof(float); // ~262s at 16 kHz

    AudioRingBuffer() : buf_(kCapacity), write_pos_(0), read_pos_(0) {}

    // Called from the audio callback thread (producer). Drops samples on overflow.
    void push(const float* samples, size_t count) {
        size_t wp = write_pos_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < count; ++i) {
            size_t next = (wp + 1) % kCapacity;
            if (next == read_pos_.load(std::memory_order_acquire))
                break; // buffer full: drop the rest
            buf_[wp] = samples[i];
            wp = next;
        }
        write_pos_.store(wp, std::memory_order_release);
    }

    // Called from the worker thread (consumer). Returns number actually read.
    size_t pop(float* out, size_t max_count) {
        size_t rp = read_pos_.load(std::memory_order_relaxed);
        size_t wp = write_pos_.load(std::memory_order_acquire);
        size_t count = 0;
        while (count < max_count && rp != wp) {
            out[count++] = buf_[rp];
            rp = (rp + 1) % kCapacity;
        }
        read_pos_.store(rp, std::memory_order_release);
        return count;
    }

private:
    std::vector<float> buf_;
    std::atomic<size_t> write_pos_;
    std::atomic<size_t> read_pos_;
};

// Shared application state accessed by the main (UI) thread and the worker thread.
// Every cross-thread field is guarded by a mutex or is atomic.
struct AppState {
    // ---- Settings (UI writes, worker reads) ----
    std::mutex settings_mutex;
    std::string api_key;
    std::string selected_model = "gpt-4o-mini-transcribe";

    // ---- Lifecycle flags ----
    std::atomic<bool> capturing{false};      // true while a capture session is active
    std::atomic<bool> worker_running{false}; // true while the worker thread is alive
    std::atomic<bool> app_closing{false};    // true once the window is closing (abort in-flight work)
    std::atomic<bool> file_job_active{false}; // true while a file transcription job runs (cancellable)

    // ---- Pending dropped/opened file (handed from UI/drop callback to the worker) ----
    std::mutex drop_mutex;
    std::string pending_file;

    // ---- Audio ring buffer (lock-free) ----
    AudioRingBuffer ring;

    // ---- Transcript (mutex-protected) ----
    std::mutex transcript_mutex;
    std::deque<std::string> transcript_lines;
    bool transcript_dirty{false}; // set when a new line arrives so the UI re-scrolls

    // ---- Status / error (mutex-protected) ----
    std::mutex status_mutex;
    std::string status_text = "Idle";
    std::string error_text;

    // ---- Context tail carried across chunk boundaries (mutex-protected) ----
    std::mutex context_mutex;
    std::string context_tail; // last ~200 chars of finalized transcript

    void push_line(const std::string& line) {
        {
            std::lock_guard lk(transcript_mutex);
            transcript_lines.push_back(line);
            transcript_dirty = true;
        }
        std::lock_guard ck(context_mutex);
        context_tail += " " + line;
        if (context_tail.size() > 200)
            context_tail = context_tail.substr(context_tail.size() - 200);
    }

    std::string get_context_tail() {
        std::lock_guard lk(context_mutex);
        return context_tail;
    }

    // Returns the full transcript as a single string (lines joined with '\n').
    std::string get_transcript_text() {
        std::string out;
        std::lock_guard lk(transcript_mutex);
        for (const auto& line : transcript_lines) {
            out += line;
            out += '\n';
        }
        return out;
    }

    void clear_transcript() {
        {
            std::lock_guard lk(transcript_mutex);
            transcript_lines.clear();
            transcript_dirty = true;
        }
        std::lock_guard ck(context_mutex);
        context_tail.clear();
    }

    size_t transcript_line_count() {
        std::lock_guard lk(transcript_mutex);
        return transcript_lines.size();
    }

    void set_status(const std::string& s) {
        std::lock_guard lk(status_mutex);
        status_text = s;
    }

    void set_error(const std::string& e) {
        std::lock_guard lk(status_mutex);
        error_text = e;
    }

    void clear_error() {
        std::lock_guard lk(status_mutex);
        error_text.clear();
    }

    std::pair<std::string, std::string> get_status() {
        std::lock_guard lk(status_mutex);
        return {status_text, error_text};
    }

    std::string get_api_key() {
        std::lock_guard lk(settings_mutex);
        return api_key;
    }

    std::string get_model() {
        std::lock_guard lk(settings_mutex);
        return selected_model;
    }

    // Queue a file path (from the Upload dialog or a window drop) for the worker.
    void set_pending_file(const std::string& path) {
        std::lock_guard lk(drop_mutex);
        pending_file = path;
    }

    // Atomically take and clear the queued file path; empty if none.
    std::string take_pending_file() {
        std::lock_guard lk(drop_mutex);
        std::string p = std::move(pending_file);
        pending_file.clear();
        return p;
    }
};
