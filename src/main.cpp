#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <GL/gl.h>

// Windows ships an OpenGL 1.1 gl.h; this token is GL 1.2+ but supported by the
// 3.3 context we create at runtime.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// Dear ImGui with the GLFW + OpenGL3 backends.
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <dwmapi.h> // dark title bar to match the theme

#include <curl/curl.h> // curl_global_init / curl_global_cleanup

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "app_state.h"
#include "audio_capture.h"
#include "image_loader.h"
#include "key_store.h"
#include "media_decoder.h"
#include "transcriber.h"
#include "vad_chunker.h"
#include "wav.h"

// ---------------------------------------------------------------------------
// Worker thread: live VAD chunking + HTTP transcription pipeline.
// ---------------------------------------------------------------------------

static void worker_thread_func(AppState& state) {
    state.set_status("Capturing");
    state.clear_error();

    // Retries are skipped once a session stops; in-flight transfers are aborted
    // only when the whole app is closing (so a manual Stop still finishes cleanly).
    std::function<bool()> keep_retrying = [&state]() {
        return state.capturing.load(std::memory_order_acquire);
    };

    VadChunker chunker(state, [&state, &keep_retrying](std::vector<float> chunk) {
        Transcriber::Request req;
        req.wav_bytes      = Wav::encode_f32_to_wav(chunk.data(), chunk.size());
        req.api_key        = state.get_api_key();
        req.model          = state.get_model();
        req.prompt         = state.get_context_tail();
        req.should_continue = &keep_retrying;
        req.abort_flag      = &state.app_closing;

        auto result = Transcriber::transcribe(req);
        if (result) {
            if (!result->empty()) state.push_line(*result);
        } else {
            state.set_error(result.error());
        }
    });

    while (state.capturing.load(std::memory_order_acquire)) {
        chunker.process_available();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

    state.set_status("Stopping");
    chunker.process_available();
    if (!state.app_closing.load(std::memory_order_acquire)) {
        chunker.flush();
    }

    state.set_status("Idle");
    state.worker_running.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Worker thread: transcribe an uploaded/dropped media file. Decodes the file's
// audio with Media Foundation, then runs it through the same VAD + HTTP pipeline.
// ---------------------------------------------------------------------------

static void file_worker_func(AppState& state, std::string path) {
    state.set_status("Decoding file...");
    state.clear_error();

    std::function<bool()> keep = [&state]() {
        return state.file_job_active.load(std::memory_order_acquire) &&
               !state.app_closing.load(std::memory_order_acquire);
    };

    int segment = 0;
    VadChunker chunker(state, [&state, &keep, &segment](std::vector<float> chunk) {
        Transcriber::Request req;
        req.wav_bytes      = Wav::encode_f32_to_wav(chunk.data(), chunk.size());
        req.api_key        = state.get_api_key();
        req.model          = state.get_model();
        req.prompt         = state.get_context_tail();
        req.should_continue = &keep;
        req.abort_flag      = &state.app_closing;

        ++segment;
        state.set_status("Transcribing file... (segment " + std::to_string(segment) + ")");

        auto result = Transcriber::transcribe(req);
        if (result) {
            if (!result->empty()) state.push_line(*result);
        } else {
            state.set_error(result.error());
        }
    });

    auto decoded = MediaDecoder::decode(
        path,
        [&chunker](const float* samples, size_t count) { chunker.feed(samples, count); },
        keep);

    if (!decoded) {
        state.set_error(decoded.error());
    } else if (keep()) {
        chunker.flush();
    }

    state.set_status(state.file_job_active.load() ? "Idle" : "Cancelled");
    state.file_job_active.store(false, std::memory_order_release);
    state.worker_running.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Settings panel that animates its height open/closed over ~0.2s.
// ---------------------------------------------------------------------------

struct AnimatedPanel {
    float height{0.0f};
    float full_height{118.0f};
    bool  open{true};

    void update(float dt) {
        const float speed = full_height / 0.2f;
        if (open) height = std::min(height + speed * dt, full_height);
        else      height = std::max(height - speed * dt, 0.0f);
    }
    bool is_visible() const { return height > 0.5f; }
};

// ---------------------------------------------------------------------------
// Attack on Titan / Survey Corps inspired dark theme.
// ---------------------------------------------------------------------------

static void apply_attack_on_titan_theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 2.0f;
    s.ChildRounding     = 2.0f;
    s.FrameRounding     = 2.0f;
    s.GrabRounding      = 2.0f;
    s.PopupRounding     = 2.0f;
    s.ScrollbarRounding = 2.0f;
    s.TabRounding       = 2.0f;
    s.FrameBorderSize   = 1.0f;
    s.WindowBorderSize  = 1.0f;
    s.ChildBorderSize   = 1.0f;
    s.WindowPadding     = ImVec2(16, 14);
    s.FramePadding      = ImVec2(11, 7);
    s.ItemSpacing       = ImVec2(10, 9);
    s.ScrollbarSize     = 14.0f;

    const ImVec4 ink      = ImVec4(0.07f, 0.06f, 0.05f, 1.00f);
    const ImVec4 panel    = ImVec4(0.11f, 0.10f, 0.08f, 1.00f);
    const ImVec4 panel2   = ImVec4(0.16f, 0.14f, 0.11f, 1.00f);
    const ImVec4 parch    = ImVec4(0.90f, 0.84f, 0.70f, 1.00f);
    const ImVec4 parchDim = ImVec4(0.60f, 0.55f, 0.45f, 1.00f);
    const ImVec4 blood    = ImVec4(0.62f, 0.13f, 0.11f, 1.00f);
    const ImVec4 bloodHi  = ImVec4(0.78f, 0.20f, 0.16f, 1.00f);
    const ImVec4 bloodLo  = ImVec4(0.42f, 0.10f, 0.09f, 1.00f);
    const ImVec4 bronze   = ImVec4(0.55f, 0.42f, 0.23f, 1.00f);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                 = parch;
    c[ImGuiCol_TextDisabled]         = parchDim;
    c[ImGuiCol_WindowBg]             = ink;
    c[ImGuiCol_ChildBg]              = ImVec4(0.09f, 0.08f, 0.06f, 1.00f);
    c[ImGuiCol_PopupBg]              = panel;
    c[ImGuiCol_Border]               = ImVec4(bronze.x, bronze.y, bronze.z, 0.45f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]              = panel2;
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.22f, 0.19f, 0.15f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.26f, 0.22f, 0.17f, 1.00f);
    c[ImGuiCol_TitleBg]              = ink;
    c[ImGuiCol_TitleBgActive]        = panel;
    c[ImGuiCol_TitleBgCollapsed]     = ink;
    c[ImGuiCol_MenuBarBg]            = panel;
    c[ImGuiCol_ScrollbarBg]          = ink;
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.30f, 0.25f, 0.18f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = bronze;
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.70f, 0.54f, 0.30f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.86f, 0.72f, 0.40f, 1.00f);
    c[ImGuiCol_SliderGrab]           = bronze;
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.75f, 0.58f, 0.33f, 1.00f);
    c[ImGuiCol_Button]               = bloodLo;
    c[ImGuiCol_ButtonHovered]        = blood;
    c[ImGuiCol_ButtonActive]         = bloodHi;
    c[ImGuiCol_Header]               = ImVec4(blood.x, blood.y, blood.z, 0.55f);
    c[ImGuiCol_HeaderHovered]        = blood;
    c[ImGuiCol_HeaderActive]         = bloodHi;
    c[ImGuiCol_Separator]            = ImVec4(bronze.x, bronze.y, bronze.z, 0.50f);
    c[ImGuiCol_SeparatorHovered]     = bronze;
    c[ImGuiCol_SeparatorActive]      = ImVec4(0.75f, 0.58f, 0.33f, 1.00f);
    c[ImGuiCol_ResizeGrip]           = ImVec4(bronze.x, bronze.y, bronze.z, 0.30f);
    c[ImGuiCol_ResizeGripHovered]    = bronze;
    c[ImGuiCol_ResizeGripActive]     = ImVec4(0.75f, 0.58f, 0.33f, 1.00f);
    c[ImGuiCol_TextSelectedBg]       = ImVec4(blood.x, blood.y, blood.z, 0.45f);
    c[ImGuiCol_NavHighlight]         = bronze;
    c[ImGuiCol_PlotLines]            = bronze;
    c[ImGuiCol_PlotHistogram]        = blood;
}

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

// Word-wrap text to a pixel width using the current font, preserving the hard
// line breaks already present (one per finalized transcript line).
static std::string wrap_text_to_width(const std::string& text, float max_width) {
    if (max_width < 24.0f) max_width = 24.0f;
    std::string out;
    out.reserve(text.size() + text.size() / 40 + 16);

    size_t line_start = 0;
    while (line_start <= text.size()) {
        size_t nl = text.find('\n', line_start);
        size_t line_end = (nl == std::string::npos) ? text.size() : nl;
        const std::string line = text.substr(line_start, line_end - line_start);

        std::string current;
        size_t i = 0;
        while (i < line.size()) {
            size_t sp = line.find(' ', i);
            size_t word_end = (sp == std::string::npos) ? line.size() : sp + 1;
            std::string word = line.substr(i, word_end - i);
            std::string trial = current + word;
            if (!current.empty() && ImGui::CalcTextSize(trial.c_str()).x > max_width) {
                out += current;
                out += '\n';
                current = word;
            } else {
                current.swap(trial);
            }
            i = word_end;
        }
        out += current;

        if (nl == std::string::npos) break;
        out += '\n';
        line_start = nl + 1;
    }
    return out;
}

static std::string format_mmss(double seconds) {
    if (seconds < 0) seconds = 0;
    int total = static_cast<int>(seconds);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", total / 60, total % 60);
    return buf;
}

// Opens the Windows "Save As" dialog and writes `text` to the chosen path.
static std::string save_transcript_to_file(const std::string& text) {
    if (text.empty()) return "Nothing to save.";

    char filename[MAX_PATH] = {};
    {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        std::snprintf(filename, sizeof(filename),
                      "transcript-%04d%02d%02d-%02d%02d%02d.txt",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = GetForegroundWindow();
    ofn.lpstrFilter = "Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = filename;
    ofn.nMaxFile    = sizeof(filename);
    ofn.lpstrDefExt = "txt";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameA(&ofn)) {
        return CommDlgExtendedError() == 0 ? std::string() : std::string("Save dialog failed.");
    }

    std::ofstream out(filename, std::ios::binary);
    if (!out) return std::string("Failed to open file for writing.");
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) return std::string("Failed to write file.");
    return std::string("Saved to ") + filename;
}

// Opens the Windows "Open" dialog filtered to media files. Returns a UTF-8 path.
static std::string open_media_file_dialog() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = GetForegroundWindow();
    ofn.lpstrFilter =
        L"Audio / Video\0*.mp4;*.mov;*.mkv;*.avi;*.wmv;*.webm;*.m4a;*.mp3;*.wav;*.flac;*.aac;*.ogg;*.mpg;*.mpeg;*.3gp;*.ts;*.wma\0"
        L"All Files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile  = MAX_PATH;
    ofn.Flags     = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) return {};

    int n = WideCharToMultiByte(CP_UTF8, 0, file, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, file, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// GLFW drop callback: queue the first dropped path for the worker.
static void drop_callback(GLFWwindow* window, int count, const char** paths) {
    if (count <= 0) return;
    auto* st = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (st) st->set_pending_file(paths[0]);
}

// Auto-scroll the read-only transcript to the bottom on the frame new text lands,
// without disturbing the user's selection on other frames.
static int transcript_scroll_cb(ImGuiInputTextCallbackData* data) {
    bool* request = static_cast<bool*>(data->UserData);
    if (request && *request) {
        data->CursorPos = data->BufTextLen;
        data->SelectionStart = data->SelectionEnd = data->CursorPos;
        *request = false;
    }
    return 0;
}

static GLuint make_texture_rgba(const ImageLoader::Rgba& img) {
    if (!img.valid()) return 0;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width, img.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, img.pixels.data());
    return tex;
}

// ---------------------------------------------------------------------------
// Entry point.
// ---------------------------------------------------------------------------

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    curl_global_init(CURL_GLOBAL_ALL);

    AppState state;

    {
        auto loaded = KeyStore::load();
        if (loaded) {
            std::lock_guard lk(state.settings_mutex);
            state.api_key = *loaded;
        }
    }

    if (!glfwInit()) {
        curl_global_cleanup();
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(900, 720, "TranscribeAI", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        curl_global_cleanup();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetWindowUserPointer(window, &state);
    glfwSetDropCallback(window, drop_callback);

    // Dark title bar to match the theme (DWMWA_USE_IMMERSIVE_DARK_MODE = 20).
    {
        HWND hwnd = glfwGetWin32Window(window);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // no imgui.ini clutter

    apply_attack_on_titan_theme();

    // Clean modern fonts (Segoe UI ships on every Windows 10/11 machine).
    ImFont* font_body = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 19.0f);
    ImFont* font_title = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", 32.0f);
    ImFont* font_sub   = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", 15.0f);
    if (font_body) io.FontDefault = font_body;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Embedded logo -> GL texture + window icon.
    GLuint logo_tex = 0;
    float logo_aspect = 1.0f;
    {
        ImageLoader::Rgba logo = ImageLoader::load_embedded_logo();
        if (logo.valid()) {
            logo_tex = make_texture_rgba(logo);
            logo_aspect = static_cast<float>(logo.width) / static_cast<float>(logo.height);
            GLFWimage icon{};
            icon.width  = logo.width;
            icon.height = logo.height;
            icon.pixels = logo.pixels.data();
            glfwSetWindowIcon(window, 1, &icon);
        }
    }

    char key_buf[2048] = {};
    {
        std::lock_guard lk(state.settings_mutex);
        size_t n = std::min(state.api_key.size(), sizeof(key_buf) - 1);
        std::memcpy(key_buf, state.api_key.data(), n);
        key_buf[n] = '\0';
    }

    bool show_key = false;
    const char* model_options[] = { "gpt-4o-mini-transcribe", "gpt-4o-transcribe", "whisper-1" };
    int model_index = 0;

    AnimatedPanel panel;
    std::string save_feedback;
    double save_feedback_timer = 0.0;

    float font_scale = 1.0f;
    double capture_start_time = 0.0;

    // Transcript display cache (wrapped, read-only, selectable).
    std::vector<char> transcript_buf(1, '\0');
    float  last_wrap_width = -1.0f;
    bool   transcript_scroll_request = false;
    size_t transcript_words = 0;

    std::thread worker;
    AudioCapture capture(state);

    auto set_feedback = [&](const std::string& msg, double secs) {
        save_feedback = msg;
        save_feedback_timer = secs;
    };

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        float dt = io.DeltaTime;
        panel.update(dt);
        io.FontGlobalScale = font_scale;

        if (save_feedback_timer > 0.0) {
            save_feedback_timer -= static_cast<double>(dt);
            if (save_feedback_timer <= 0.0) save_feedback.clear();
        }

        // Reap a finished worker without blocking the UI.
        if (!state.capturing.load(std::memory_order_acquire) &&
            !state.worker_running.load(std::memory_order_acquire) &&
            worker.joinable()) {
            worker.join();
        }

        // Pick up a queued file (from the Upload dialog or a window drop).
        {
            std::string f = state.take_pending_file();
            if (!f.empty()) {
                bool busy = state.capturing.load() || state.worker_running.load();
                if (busy) {
                    set_feedback("Busy - finish the current job first.", 3.0);
                } else if (state.get_api_key().empty()) {
                    state.set_error("Enter and save an API key before transcribing a file.");
                } else {
                    state.clear_error();
                    if (worker.joinable()) worker.join();
                    state.worker_running.store(true, std::memory_order_release);
                    state.file_job_active.store(true, std::memory_order_release);
                    worker = std::thread(file_worker_func, std::ref(state), f);
                }
            }
        }

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("TranscribeAI", nullptr,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        // --- Header band: logo + title + tagline ---
        {
            float header_h = 52.0f;
            if (logo_tex) {
                ImGui::Image((ImTextureID)(intptr_t)logo_tex,
                             ImVec2(header_h * logo_aspect, header_h));
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            if (font_title) ImGui::PushFont(font_title);
            ImGui::TextUnformatted("TRANSCRIBE AI");
            if (font_title) ImGui::PopFont();
            if (font_sub) ImGui::PushFont(font_sub);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.30f, 0.24f, 1.0f));
            ImGui::TextUnformatted("WAVEMIND  \xE2\x80\xA2  DEDICATE YOUR EARS");
            ImGui::PopStyleColor();
            if (font_sub) ImGui::PopFont();
            ImGui::EndGroup();
        }
        ImGui::Spacing();
        ImGui::Separator();

        // --- Settings toggle ---
        if (ImGui::Button(panel.open ? "Settings  [hide]" : "Settings  [show]")) {
            panel.open = !panel.open;
        }

        if (panel.is_visible()) {
            ImGui::BeginChild("##settings_inner", ImVec2(0.0f, panel.height), true,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

            ImGui::TextUnformatted("API Key:");
            ImGui::SameLine();
            ImGuiInputTextFlags key_flags = ImGuiInputTextFlags_None;
            if (!show_key) key_flags |= ImGuiInputTextFlags_Password;
            ImGui::SetNextItemWidth(380.0f);
            if (ImGui::InputText("##apikey", key_buf, sizeof(key_buf), key_flags)) {
                std::lock_guard lk(state.settings_mutex);
                state.api_key = key_buf;
            }
            ImGui::SameLine();
            ImGui::Checkbox("Show key", &show_key);
            ImGui::SameLine();
            if (ImGui::Button("Save key")) {
                auto res = KeyStore::save(std::string(key_buf));
                set_feedback(res ? std::string("Key saved.")
                                 : std::string("Error: ") + res.error(), 3.0);
            }

            ImGui::TextUnformatted("Model:  ");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::Combo("##model", &model_index, model_options, IM_ARRAYSIZE(model_options))) {
                std::lock_guard lk(state.settings_mutex);
                state.selected_model = model_options[model_index];
            }
            ImGui::SameLine(0.0f, 24.0f);
            ImGui::TextUnformatted("Text size");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140.0f);
            ImGui::SliderFloat("##fontscale", &font_scale, 0.8f, 1.6f, "%.2fx");

            ImGui::EndChild();
        }

        ImGui::Separator();

        // --- Primary controls: live capture + file upload ---
        bool capturing   = state.capturing.load(std::memory_order_acquire);
        bool worker_alive = state.worker_running.load(std::memory_order_acquire);
        bool file_active = state.file_job_active.load(std::memory_order_acquire);

        if (capturing) {
            if (ImGui::Button("Stop Capture", ImVec2(150, 32))) {
                state.capturing.store(false, std::memory_order_release);
                capture.stop();
            }
        } else if (worker_alive && !file_active) {
            ImGui::BeginDisabled();
            ImGui::Button("Stopping...", ImVec2(150, 32));
            ImGui::EndDisabled();
        } else {
            bool busy = worker_alive; // a file job is running
            if (busy) ImGui::BeginDisabled();
            if (ImGui::Button("Start Capture", ImVec2(150, 32))) {
                std::string key = state.get_api_key();
                if (key.empty()) {
                    state.set_error("Enter an API key before starting.");
                } else {
                    state.clear_error();
                    auto start_res = capture.start();
                    if (!start_res) {
                        state.set_error("Audio capture failed: " + start_res.error());
                    } else {
                        if (worker.joinable()) worker.join();
                        state.capturing.store(true, std::memory_order_release);
                        state.worker_running.store(true, std::memory_order_release);
                        capture_start_time = ImGui::GetTime();
                        worker = std::thread(worker_thread_func, std::ref(state));
                    }
                }
            }
            if (busy) ImGui::EndDisabled();
        }

        ImGui::SameLine();
        {
            bool busy = capturing || worker_alive;
            if (busy) ImGui::BeginDisabled();
            if (ImGui::Button("Upload & Transcribe", ImVec2(190, 32))) {
                std::string f = open_media_file_dialog();
                if (!f.empty()) state.set_pending_file(f);
            }
            if (busy) ImGui::EndDisabled();
        }

        if (file_active) {
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(90, 32))) {
                state.file_job_active.store(false, std::memory_order_release);
            }
        }

        if (capturing) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.25f, 0.20f, 1.0f));
            ImGui::Text("  REC  %s", format_mmss(ImGui::GetTime() - capture_start_time).c_str());
            ImGui::PopStyleColor();
        }

        ImGui::TextDisabled("Tip: drag a video or audio file onto this window to transcribe it.");
        ImGui::Separator();

        // --- Transcript action bar ---
        {
            bool has_text = state.transcript_line_count() > 0;
            if (!has_text) ImGui::BeginDisabled();
            if (ImGui::Button("Clear")) {
                state.clear_transcript();
                set_feedback("Transcript cleared.", 2.0);
            }
            ImGui::SameLine();
            if (ImGui::Button("Copy All")) {
                ImGui::SetClipboardText(state.get_transcript_text().c_str());
                set_feedback("Copied to clipboard.", 2.0);
            }
            ImGui::SameLine();
            if (ImGui::Button("Save...")) {
                std::string msg = save_transcript_to_file(state.get_transcript_text());
                if (!msg.empty()) set_feedback(msg, 4.0);
            }
            if (!has_text) ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::TextDisabled("(%zu lines, %zu words)",
                                state.transcript_line_count(), transcript_words);

            if (!save_feedback.empty()) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.86f, 0.72f, 0.40f, 1.0f));
                ImGui::TextUnformatted(save_feedback.c_str());
                ImGui::PopStyleColor();
            }
        }

        // --- Selectable transcript (read-only multiline so the user can drag to
        //     select and Ctrl+C any section). Wrapped to the current width. ---
        float status_height = ImGui::GetTextLineHeightWithSpacing() * 2.0f + 14.0f;
        float avail_h = std::max(ImGui::GetContentRegionAvail().y - status_height, 80.0f);
        float avail_w = ImGui::GetContentRegionAvail().x;

        bool dirty = false;
        {
            std::lock_guard lk(state.transcript_mutex);
            if (state.transcript_dirty) { dirty = true; state.transcript_dirty = false; }
        }
        bool width_changed = std::fabs(avail_w - last_wrap_width) > 6.0f;
        if (dirty || width_changed || last_wrap_width < 0.0f) {
            std::string raw = state.get_transcript_text();
            transcript_words = 0;
            bool in_word = false;
            for (char ch : raw) {
                bool ws = (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r');
                if (!ws && !in_word) { ++transcript_words; in_word = true; }
                else if (ws) in_word = false;
            }
            std::string wrapped = wrap_text_to_width(raw, avail_w - 28.0f);
            transcript_buf.assign(wrapped.begin(), wrapped.end());
            transcript_buf.push_back('\0');
            last_wrap_width = avail_w;
            if (dirty) transcript_scroll_request = true;
        }

        ImGui::InputTextMultiline("##transcript_view",
                                  transcript_buf.data(), transcript_buf.size(),
                                  ImVec2(-FLT_MIN, avail_h),
                                  ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_CallbackAlways,
                                  transcript_scroll_cb, &transcript_scroll_request);

        // --- Status line ---
        ImGui::Separator();
        {
            auto [status, error] = state.get_status();
            ImGui::TextDisabled("Status:");
            ImGui::SameLine();
            ImGui::TextUnformatted(status.c_str());
            if (!error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.35f, 0.30f, 1.0f));
                ImGui::TextWrapped("Error: %s", error.c_str());
                ImGui::PopStyleColor();
            }
        }

        ImGui::End();

        ImGui::Render();
        int display_w = 0, display_h = 0;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.05f, 0.045f, 0.04f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // Clean shutdown: abort in-flight work and join the worker before teardown.
    state.app_closing.store(true, std::memory_order_release);
    state.capturing.store(false, std::memory_order_release);
    state.file_job_active.store(false, std::memory_order_release);
    capture.stop();
    if (worker.joinable()) worker.join();

    if (logo_tex) glDeleteTextures(1, &logo_tex);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    curl_global_cleanup();
    return 0;
}
