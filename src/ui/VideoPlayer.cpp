#include "VideoPlayer.h"
#include "Logger.h"
#include "AudioPlayback.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <filesystem>

#include <d3d9.h> // For MFCreateDXGISurfaceBuffer
#include <mfapi.h>
#include <mfidl.h>
#include <Mfreadwrite.h>
#include <mferror.h>
#include <shlwapi.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "dwmapi.lib")

VideoPlayer::VideoPlayer() {}
VideoPlayer::~VideoPlayer() { shutdown(); }

bool VideoPlayer::get_video_dimensions(const std::string& video_path, int& width, int& height) {
    width = 0;
    height = 0;

    // Check if file exists
    if (!std::filesystem::exists(video_path)) {
        return false;
    }

    // Convert to absolute path if it's relative
    std::string absolute_path = video_path;
    if (video_path.length() < 2 || (video_path[1] != ':' && video_path[0] != '\\' && video_path[0] != '/')) {
        char current_dir[MAX_PATH];
        GetCurrentDirectoryA(MAX_PATH, current_dir);
        absolute_path = std::string(current_dir) + "\\" + video_path;
    }

    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, absolute_path.c_str(), -1, wpath, MAX_PATH);

    // Initialize Media Foundation if not already initialized
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool com_initialized = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    if (!com_initialized) {
        return false;
    }

    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    bool mf_started = SUCCEEDED(hr);
    if (!mf_started) {
        if (com_initialized) CoUninitialize();
        return false;
    }

    IMFSourceReader* r = nullptr;
    hr = MFCreateSourceReaderFromURL(wpath, nullptr, &r);
    if (FAILED(hr)) {
        if (mf_started) MFShutdown();
        if (com_initialized) CoUninitialize();
        return false;
    }

    IMFMediaType* nat = nullptr;
    hr = r->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &nat);
    if (SUCCEEDED(hr)) {
        UINT32 vw = 0, vh = 0;
        MFGetAttributeSize(nat, MF_MT_FRAME_SIZE, &vw, &vh);
        width = (int)vw;
        height = (int)vh;
        nat->Release();
    }

    r->Release();
    if (mf_started) MFShutdown();
    if (com_initialized) CoUninitialize();

    return width > 0 && height > 0;
}

bool VideoPlayer::initialize() {
    if (initialized_) return true;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        { std::ostringstream ss; ss << "COM failed: " << std::hex << hr; LOG_ERROR("VideoPlayer", ss.str()); }
        return false;
    }

    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) { LOG_ERROR("VideoPlayer", "MFStartup failed"); return false; }
    mf_initialized_ = true;
    
    initialized_ = true;
    LOG_VIDEOPLAYER("Initialized");
    return initialized_;
}

bool VideoPlayer::open_media_file(const std::string& path) {
    { std::ostringstream ss; ss << "Open: " << path; LOG_VIDEOPLAYER(ss.str()); }
    close_media_file();

    // Check if file exists before attempting to open
    if (!std::filesystem::exists(path)) {
        std::ostringstream ss; ss << "Video file does not exist: " << path; LOG_ERROR("VideoPlayer", ss.str()); return false;
    }

    // Convert to absolute path if it's relative
    std::string absolute_path = path;
    // Check if path is relative (doesn't start with drive letter or backslash)
    if (path.length() < 2 || (path[1] != ':' && path[0] != '\\' && path[0] != '/')) {
        char current_dir[MAX_PATH];
        GetCurrentDirectoryA(MAX_PATH, current_dir);
        absolute_path = std::string(current_dir) + "\\" + path;
    }

    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, absolute_path.c_str(), -1, wpath, MAX_PATH);

    IMFSourceReader* r = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(wpath, nullptr, &r);
    if (FAILED(hr)) { 
        std::ostringstream ss; ss << "MFCreateSourceReaderFromURL failed: " << std::hex << hr << " for path: " << absolute_path; 
        LOG_ERROR("VideoPlayer", ss.str()); 
        return false; 
    }

    IMFMediaType* nat = nullptr;
    hr = r->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &nat);
    if (FAILED(hr)) { r->Release(); return false; }

    UINT32 vw = 0, vh = 0;
    MFGetAttributeSize(nat, MF_MT_FRAME_SIZE, &vw, &vh);

    GUID st;
    nat->GetGUID(MF_MT_SUBTYPE, &st);
    { std::ostringstream ss; ss << "Native: " << vw << "x" << vh; if (st == MFVideoFormat_NV12) ss << " NV12"; else if (st == MFVideoFormat_RGB32) ss << " RGB32"; else ss << " OTHER"; LOG_VIDEOPLAYER(ss.str()); }
    nat->Release();

    // Store actual video dimensions
    video_width_ = (int)vw;
    video_height_ = (int)vh;

    if (vw == 0) vw = (UINT32)window_width_;
    if (vh == 0) vh = (UINT32)window_height_;

    // Try RGB32
    IMFMediaType* ot = nullptr;
    hr = MFCreateMediaType(&ot);
    if (SUCCEEDED(hr)) {
        ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        ot->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        MFSetAttributeSize(ot, MF_MT_FRAME_SIZE, vw, vh);
        hr = r->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, ot);
        ot->Release();
    }

    if (SUCCEEDED(hr)) {
        LOG_VIDEOPLAYER("Using RGB32");
    } else {
        // Reset to NV12 native format
        IMFMediaType* nt = nullptr;
        hr = MFCreateMediaType(&nt);
        if (SUCCEEDED(hr)) {
            nt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            nt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            MFSetAttributeSize(nt, MF_MT_FRAME_SIZE, vw, vh);
            r->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, nt);
            nt->Release();
        }
        LOG_VIDEOPLAYER("Using NV12");
    }

    r->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    window_width_ = (int)vw; window_height_ = (int)vh;
    source_reader_ = r;

    // Set window size now that we know the video dimensions
    if (hwnd_) {
        SetWindowPos(hwnd_, HWND_TOPMOST, window_x_, window_y_, window_width_, window_height_, SWP_SHOWWINDOW);
    }

    LOG_VIDEOPLAYER("OK");
    return true;
}

void VideoPlayer::close_media_file() {
    close_audio_stream();
    if (source_reader_) { source_reader_->Release(); source_reader_ = nullptr; }
}

bool VideoPlayer::open_audio_stream() {
    if (!source_reader_) return false;

    // Try to get the native audio media type. If it fails, there's no audio stream.
    IMFMediaType* audio_nat = nullptr;
    HRESULT hr = source_reader_->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &audio_nat);
    if (FAILED(hr)) {
        LOG_VIDEOPLAYER("No audio stream");
        return false; // No audio stream
    }

    UINT32 rate = 0, channels = 0;
    MFGetAttributeRatio(audio_nat, MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate, nullptr);
    MFGetAttributeSize(audio_nat, MF_MT_AUDIO_NUM_CHANNELS, &channels, nullptr);

    GUID subtype;
    audio_nat->GetGUID(MF_MT_SUBTYPE, &subtype);
    { std::ostringstream ss; ss << "Audio stream: rate=" << rate << " channels=" << channels; LOG_VIDEOPLAYER(ss.str()); }
    audio_nat->Release();

    if (rate == 0) rate = 48000;
    if (channels == 0) channels = 2;

    // Set audio output type to PCM (uncompressed float)
    IMFMediaType* audio_out = nullptr;
    hr = MFCreateMediaType(&audio_out);
    if (SUCCEEDED(hr)) {
        audio_out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        audio_out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        audio_out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
        audio_out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        audio_out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        audio_out->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 4);
        audio_out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * 4);
        hr = source_reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, audio_out);
        audio_out->Release();
    }

    if (SUCCEEDED(hr)) {
        audio_sample_rate_ = rate;
        audio_channels_ = channels;
        audio_stream_available_ = true;
        { std::ostringstream ss; ss << "Audio stream ready: " << audio_sample_rate_ << "Hz " << audio_channels_ << "ch"; LOG_VIDEOPLAYER(ss.str()); }
        return true;
    }

    LOG_VIDEOPLAYER("Failed to set audio format");
    return false;
}

void VideoPlayer::close_audio_stream() {
    audio_stream_available_ = false;
}

bool VideoPlayer::read_audio_frame(std::vector<float>& audio_out) {
    if (!source_reader_ || !audio_stream_available_) return false;

    DWORD flags = 0;
    LONGLONG st = 0;
    IMFSample* samp = nullptr;
    HRESULT hr = source_reader_->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &st, &samp);
    if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM) || !samp) {
        return false;
    }

    IMFMediaBuffer* mb = nullptr;
    if (FAILED(samp->GetBufferByIndex(0, &mb))) { samp->Release(); return false; }

    BYTE* data = nullptr;
    DWORD len = 0;
    if (FAILED(mb->Lock(&data, nullptr, &len))) { mb->Release(); samp->Release(); return false; }

    // Convert PCM float data (32-bit floats)
    int sample_count = len / sizeof(float);
    audio_out.resize(sample_count);
    memcpy(audio_out.data(), data, len);

    mb->Unlock();
    mb->Release();
    samp->Release();
    return true;
}

bool VideoPlayer::read_video_frame(std::vector<uint8_t>& out, int& width, int& height, long long& duration) {
    if (!source_reader_) return false;

    DWORD flags = 0;
    LONGLONG st = 0, dur = 0;
    IMFSample* samp = nullptr;
    HRESULT hr = source_reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &st, &samp);
    if (FAILED(hr)) { std::ostringstream ss; ss << "ReadSample error: " << std::hex << hr; LOG_ERROR("VideoPlayer", ss.str()); return false; }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { if (samp) samp->Release(); return false; }
    if (!samp) return false;

    samp->GetSampleDuration(&dur);

    IMFMediaBuffer* mb = nullptr;
    if (FAILED(samp->GetBufferByIndex(0, &mb))) { samp->Release(); return false; }

    BYTE* data = nullptr;
    DWORD len = 0;
    if (FAILED(mb->Lock(&data, nullptr, &len))) { mb->Release(); samp->Release(); return false; }

    // Check current media type to know if RGB32 or NV12
    IMFMediaType* cur = nullptr;
    GUID fmt = GUID_NULL;
    if (SUCCEEDED(source_reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) {
        cur->GetGUID(MF_MT_SUBTYPE, &fmt);
        cur->Release();
    }

    width = window_width_;
    height = window_height_;
    out.resize((size_t)width * (size_t)height * 4, 0);

    if (fmt == MFVideoFormat_RGB32) {
        // Direct copy (BGRA pixels)
        memcpy(out.data(), data, std::min<DWORD>(len, (DWORD)out.size()));
    } else {
        // NV12: Y plane first, then interleaved UV
        int y_stride = width; // usually width-aligned
        int uv_stride = width; // NV12 UV plane is width bytes per row
        const uint8_t* y_plane = data;
        const uint8_t* uv_plane = data + (size_t)y_stride * height;
        nv12_to_bgra(y_plane, uv_plane, width, height, y_stride, uv_stride, out.data());
    }

    mb->Unlock();
    mb->Release();
    samp->Release();

    duration = dur;
    return true;
}

void VideoPlayer::nv12_to_bgra(const uint8_t* y, const uint8_t* uv, int w, int h, int y_stride, int uv_stride, uint8_t* bgra) {
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int yi = row * y_stride + col;
            int uv_row = row / 2;
            int uv_col = col / 2;
            int uvi = uv_row * uv_stride + uv_col * 2;
            int Y = y[yi] - 16;
            int U = uv[uvi] - 128;
            int V = uv[uvi + 1] - 128;
            if (Y < 0) Y = 0;
            int R = (298 * Y + 409 * V + 128) >> 8;
            int G = (298 * Y - 100 * U - 208 * V + 128) >> 8;
            int B = (298 * Y + 516 * U + 128) >> 8;
            if (R > 255) R = 255; if (R < 0) R = 0;
            if (G > 255) G = 255; if (G < 0) G = 0;
            if (B > 255) B = 255; if (B < 0) B = 0;
            int out_idx = row * w * 4 + col * 4;
            bgra[out_idx + 0] = (uint8_t)B; // B
            bgra[out_idx + 1] = (uint8_t)G; // G
            bgra[out_idx + 2] = (uint8_t)R; // R
            bgra[out_idx + 3] = 255;        // A
        }
    }
}

void VideoPlayer::extract_foreground(const uint8_t* bgra, uint8_t* out_bgra, int w, int h) {
    // Optimized hard binary chroma key with reduced branching and improved memory access
    // Process pixels in a single pass with minimal conditional branches
    const int total_pixels = w * h;
    const int stride = 4;
    for (int i = 0; i < total_pixels; i++) {
        int idx = i * stride;
        int b = bgra[idx + 0];
        int g = bgra[idx + 1];
        int r = bgra[idx + 2];

        // Optimized green detection using arithmetic instead of multiple branches
        // Green is dominant if g > r and g > b, and green is strong enough
        int green_dominance = (g - r) + (g - b);
        int green_strength = g - std::max(r, b);
        bool is_green = (green_dominance > 0) && (green_strength > 10);

        // Binary decision: is this a green screen pixel?
        // Use the optimized is_green calculation directly
        if (is_green) {
            out_bgra[idx + 0] = 0;
            out_bgra[idx + 1] = 0;
            out_bgra[idx + 2] = 0;
            out_bgra[idx + 3] = 0;
        } else {
            out_bgra[idx + 0] = bgra[idx + 0];
            out_bgra[idx + 1] = bgra[idx + 1];
            out_bgra[idx + 2] = bgra[idx + 2];
            out_bgra[idx + 3] = 255;
        }
    }
}

void VideoPlayer::playback_thread_func(const std::string& path, bool loop) {
    LOG_VIDEOPLAYER("Thread started");

    // Initialize COM for this thread, as Media Foundation requires it.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        { std::ostringstream ss; ss << "Thread CoInitializeEx failed: " << std::hex << hr; LOG_ERROR("VideoPlayer", ss.str()); }
        return;
    }

    if (!window_created_) {
        hinstance_ = GetModuleHandleA(nullptr);
        WNDCLASSEXA wc = {};
        wc.cbSize = sizeof(WNDCLASSEXA);
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = hinstance_;
        wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = "VP_ChromaKey";
        if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { return; }

        hwnd_ = CreateWindowExA(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "VP_ChromaKey", "Video", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, hinstance_, nullptr);
        if (!hwnd_) { return; }
        ShowWindow(hwnd_, SW_SHOW);
        window_created_ = true;

        // Set window position after creation
        SetWindowPos(hwnd_, HWND_TOPMOST, window_x_, window_y_, window_width_, window_height_, SWP_SHOWWINDOW | SWP_NOZORDER);
    }

    do {
        stop_requested_ = false;
        if (!open_media_file(path)) { playing_ = false; if (completion_callback_) completion_callback_(); return; }

        // Re-center the window now that we know the actual video dimensions
        {
            const int screen_w = GetSystemMetrics(SM_CXSCREEN);
            const int screen_h = GetSystemMetrics(SM_CYSCREEN);
            const int new_x = (screen_w - window_width_) / 2;
            const int new_y = (screen_h - window_height_) / 2;
            window_x_ = new_x;
            window_y_ = new_y;
            if (hwnd_) {
                SetWindowPos(hwnd_, HWND_TOPMOST, window_x_, window_y_, window_width_, window_height_, SWP_SHOWWINDOW | SWP_NOZORDER);
            }
        }

        int frames = 0;
        long long dur = 0;

        while (!stop_requested_) {
            int w = 0, h = 0;
            std::vector<uint8_t> frame;
            if (!read_video_frame(frame, w, h, dur)) {
                LOG_VIDEOPLAYER("EOF after " + std::to_string(frames) + " frames");
                break;
            }
            if (stop_requested_) break;
            frames++;

            // Apply chroma key in software only if enabled
            std::vector<uint8_t> fg(frame.size(), 0);
            if (enable_chroma_key_) {
                extract_foreground(frame.data(), fg.data(), w, h);
            } else {
                fg = frame; // Use original frame without chroma key
            }

            // Display via UpdateLayeredWindow for alpha transparency support
            HDC hdc = GetDC(nullptr);
            HDC mdc = CreateCompatibleDC(hdc);
            if (mdc) {
                BITMAPINFO bi = {};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = w;
                bi.bmiHeader.biHeight = -(LONG)h;
                bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
                void* bits = nullptr;
                HBITMAP hb = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
                if (hb && bits) {
                    memcpy(bits, fg.data(), (size_t)w * (size_t)h * 4);
                    HBITMAP old = (HBITMAP)SelectObject(mdc, hb);
                    SIZE sz = {(LONG)w, (LONG)h};
                    POINT pt = {window_x_, window_y_};
                    POINT ptSrc = {0, 0};
                    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

                    // Resize window to match video size before first UpdateLayeredWindow
                    if (frames == 1) {
                        SetWindowPos(hwnd_, nullptr, window_x_, window_y_, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
                    }

                    BOOL result = UpdateLayeredWindow(hwnd_, hdc, &pt, &sz, mdc, &ptSrc, 0, &bf, ULW_ALPHA);
                    if (!result) {
                        { std::ostringstream ss; ss << "UpdateLayeredWindow failed: " << GetLastError(); LOG_ERROR("VideoPlayer", ss.str()); }
                    }
                    SelectObject(mdc, old); DeleteObject(hb);
                }
                DeleteDC(mdc);
            }
            ReleaseDC(nullptr, hdc);

            if (frames == 1) {
                { std::ostringstream ss; ss << "First frame shown at position (" << window_x_ << "," << window_y_ << ") size " << w << "x" << h; LOG_VIDEOPLAYER(ss.str()); }
                { std::ostringstream ss; ss << "Chroma key enabled: " << (enable_chroma_key_ ? "yes" : "no"); LOG_VIDEOPLAYER(ss.str()); }
            }

            double ms = 33.0;
            if (dur > 0 && dur < 10000000) ms = (double)dur / 10000.0;
            if (ms > 100.0) ms = 33.0;
            if (ms > 1.0) Sleep((DWORD)ms);
        }

        close_media_file();
        if (!loop || stop_requested_) break;
    } while (loop);

    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    playing_ = false; stop_requested_ = false;
    LOG_VIDEOPLAYER("Playback done");
    if (completion_callback_) completion_callback_();

    // Notify waiting threads that playback is finished
    {
        std::lock_guard<std::mutex> lock(playback_mutex_);
        playback_finished_ = true;
    }
    playback_cv_.notify_all();

    // Uninitialize COM for this thread
    CoUninitialize();
}

bool VideoPlayer::play_file(const std::string& path, bool wait_for_completion, bool loop, bool enable_chroma_key) {
    if (!initialized_) return false;
    stop();
    LOG_VIDEOPLAYER("Play: " + path);
    playing_ = true;
    playback_finished_ = false;
    enable_chroma_key_ = enable_chroma_key;

    playback_thread_ = std::make_unique<std::thread>(&VideoPlayer::playback_thread_func, this, path, loop);

    if (wait_for_completion) {
        // Wait for playback to finish using condition variable
        std::unique_lock<std::mutex> lock(playback_mutex_);
        playback_cv_.wait(lock, [this] { return playback_finished_.load(); });
        if (playback_thread_ && playback_thread_->joinable()) {
            playback_thread_->join();
        }
        playback_thread_.reset();
    }
    // Do NOT detach - keep thread joinable so stop() can wait for it
    return true;
}

void VideoPlayer::stop() {
    if (playing_) {
        stop_requested_ = true;
        // Wait for the playback thread to finish before proceeding
        if (playback_thread_ && playback_thread_->joinable()) {
            // Wait with timeout to avoid deadlock
            auto wait_start = std::chrono::steady_clock::now();
            while (true) {
                // Check if thread has exited
                if (!playback_thread_->joinable()) break;
                // Give the thread time to see stop_requested_ and exit
                if (std::chrono::steady_clock::now() - wait_start > std::chrono::seconds(5)) {
                    // Timeout - force detach to avoid deadlock
                    playback_thread_->detach();
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        playing_ = false;
    }
    close_media_file();
    if (hwnd_) {
        ShowWindow(hwnd_, SW_HIDE);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    window_created_ = false; // Reset so a new window is created for next video
}

void VideoPlayer::set_completion_callback(std::function<void()> cb) { completion_callback_ = cb; }

void VideoPlayer::shutdown() {
    if (!initialized_) return;
    // stop() is called from destructor, no need to call it here again
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    if (mf_initialized_) { MFShutdown(); mf_initialized_ = false; }
    CoUninitialize();
    initialized_ = false;
    LOG_VIDEOPLAYER("Shutdown");
}

void VideoPlayer::set_position(int x, int y, int w, int h) {
    window_x_ = x; window_y_ = y; window_width_ = w; window_height_ = h;
    // Don't set window position here - it will be set after window creation in playback thread
}

void VideoPlayer::set_chroma_key_color(uint8_t r, uint8_t g, uint8_t b, float tolerance) {
    chroma_key_.key_r = r / 255.0f;
    chroma_key_.key_g = g / 255.0f;
    chroma_key_.key_b = b / 255.0f;
    chroma_key_.tolerance = std::max(0.01f, std::min(1.0f, tolerance));
}