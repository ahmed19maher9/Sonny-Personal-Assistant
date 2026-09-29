#pragma once
// ============================================================================
// VideoRecorder.h - Motion-JPEG AVI recorder for camera frames.
//
// CameraCapture only ever holds one decoded frame, so recording is fed from the
// DirectShow sample-grabber callback: start() writes the AVI header, add_frame()
// appends one JPEG-encoded frame, stop() writes the index and patches the sizes
// the header could not know up front.
//
// The container is written by hand instead of going through Media Foundation
// because the encoder is then the only variable: a Motion-JPEG AVI plays in
// Windows Media Player, VLC and mpv, and the frames are produced by GDI+, which
// the app already depends on for still images.
//
// Threading: add_frame() runs on the sample-grabber thread, start()/stop() run
// on the tool thread. All of them serialise on one internal mutex.
// ============================================================================

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Jarvis {

class VideoRecorder {
public:
    VideoRecorder();
    ~VideoRecorder();

    VideoRecorder(const VideoRecorder&) = delete;
    VideoRecorder& operator=(const VideoRecorder&) = delete;

    // Opens `output_path` (.avi) and records WIDTH x HEIGHT frames at
    // FRAME_RATE fps. Once MAX_SECONDS worth of time has been recorded the file
    // is finalised, so a forgotten recording cannot fill the disk.
    bool start(const std::string& output_path, int width, int height,
               double frame_rate, int max_seconds);

    // Appends one top-down RGB24 frame. Frames whose size differs from the
    // recording are ignored, and frames that arrive faster than FRAME_RATE are
    // dropped so the finished video runs in real time even when the encoder
    // cannot keep up.
    bool add_frame(const unsigned char* rgb, int width, int height);

    // Writes the index and closes the file. Safe to call when not recording.
    bool stop();

    bool is_recording() const { return recording_; }
    long long frames_written() const;
    double recorded_seconds() const;
    const std::string& output_path() const;
    std::string last_error() const;

private:
    bool write_headers(int width, int height, double frame_rate);
    void write_index();
    // Fills in the sizes the header could not know before the frames were
    // written (RIFF length, movi list length, frame counts).
    void patch_sizes();
    void close_file();
    // Finalises the file. Called with mutex_ already held, because both stop()
    // and add_frame() (on the duration cap) reach it.
    void stop_locked();

    std::atomic<bool> recording_{false};
    mutable std::mutex mutex_;

    std::FILE* file_ = nullptr;
    // Offsets of the header fields stop() has to write back, and the position
    // frame chunk offsets are counted from.
    long long riff_size_offset_ = -1;
    long long movi_size_offset_ = -1;
    long long movi_fourcc_offset_ = -1;
    long long avih_total_frames_offset_ = -1;
    long long strh_length_offset_ = -1;

    int width_ = 0;
    int height_ = 0;
    double frame_rate_ = 30.0;
    long long max_frames_ = 0;
    long long frames_written_ = 0;
    // Guards the frame pacing: frames that arrive before 1 / frame_rate
    // seconds have elapsed are dropped instead of speeding the video up.
    long long last_frame_tick_ = 0;
    // (offset relative to the 'movi' FOURCC, frame size) per frame.
    std::vector<std::pair<uint32_t, uint32_t>> frame_index_;

    std::string output_path_;
    std::string last_error_;
};

} // namespace Jarvis
