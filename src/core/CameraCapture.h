#pragma once

#include <vector>
#include <memory>
#include <atomic>
#include <functional>
#include <thread>
#include <mutex>
#include <string>

namespace Jarvis {

class VideoRecorder;

class CameraCapture {
public:
    CameraCapture();
    ~CameraCapture();

    bool initialize();
    void shutdown();

    bool is_running() const { return running_; }
    bool start();
    void stop();

    // Set the camera device name to use (empty = default/first camera)
    void set_camera_device(const std::string& device_name) { camera_device_name_ = device_name; }

    // Capture a single frame. Returns RGB pixel data (width * height * 3).
    // Returns empty vector on failure.
    std::vector<unsigned char> capture_frame(int& out_width, int& out_height);

    // Enumerate available camera devices
    static std::vector<std::string> enumerate_cameras();

    // The orchestrator owns the one CameraCapture instance; the camera tool
    // reaches it through this handle instead of a back-pointer.
    static CameraCapture* active();

    // Where photos and videos are written, under %USERPROFILE%\Pictures:
    //   <root>\Photos\Sonny_Photo_<timestamp>.<ext>
    //   <root>\Videos\Sonny_Video_<timestamp>.avi
    static std::string capture_root_dir();
    static std::string photos_dir();
    static std::string videos_dir();
    static bool ensure_directory(const std::string& directory, std::string& out_error);
    // Builds an unused "<prefix>_<timestamp>.<ext>" path inside `directory`.
    static std::string make_capture_path(const std::string& directory, const std::string& prefix,
                                        const std::string& extension);

    // Saves the newest frame as a PNG (extension "png") or JPEG ("jpg"),
    // starting the camera for the moment if it is not already running.
    bool save_photo(const std::string& directory, const std::string& extension,
                    std::string& out_path, std::string& out_error);
    bool save_photo(std::string& out_path, std::string& out_error);

    // Records every grabbed frame into an AVI until stop_recording().
    bool start_recording(const std::string& path, std::string& out_error);
    bool stop_recording(std::string& out_path, std::string& out_error);
    bool is_recording() const;
    std::string recording_path() const;
    double recording_seconds() const;

    // Frames per second reported by the camera's media type (30 when unknown).
    double source_frame_rate() const;

private:
    void capture_loop();
    // Feeds one freshly grabbed frame to an active recording. Called from the
    // sample-grabber thread with mutex_ held, hence the strict lock order:
    // mutex_ may be taken before recorder_mutex_, never the other way round.
    void feed_recorder(const unsigned char* rgb, int width, int height);
    // Brings the camera up if needed and waits for the first frame. Sets
    // camera_started_here_ so the camera is not left running afterwards.
    bool ensure_frame_ready(int timeout_ms, int& out_width, int& out_height, std::string& out_error);
    void release_camera_if_started_here();

    std::atomic<bool> running_;
    std::atomic<bool> stop_requested_;
    std::thread capture_thread_;
    mutable std::mutex mutex_;
    // Lives on the capture thread: hands every grabbed frame to the recorder.
    // A member (not a local of start()) because the thread outlives start().
    std::function<void(const unsigned char*, int, int)> frame_sink_;

    std::vector<unsigned char> latest_frame_;
    int latest_width_ = 0;
    int latest_height_ = 0;
    double source_frame_rate_ = 30.0;
    std::string camera_device_name_;

    std::unique_ptr<VideoRecorder> recorder_;
    mutable std::mutex recorder_mutex_;
    bool camera_started_here_ = false;

    static std::atomic<CameraCapture*> active_instance_;
};

} // namespace Jarvis
