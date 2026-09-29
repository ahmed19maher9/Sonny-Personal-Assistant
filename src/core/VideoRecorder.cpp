#include "VideoRecorder.h"
#include "ImageUtil.h"
#include "Logger.h"
#include "PathUtil.h"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <utility>

namespace Jarvis {
namespace {

const char* kComponent = "VideoRecorder";

// AVI is a RIFF file: every chunk is a FOURCC, a little-endian size and that
// many payload bytes, padded to an even length.
#pragma pack(push, 1)

struct Avih {
    uint32_t micro_seconds_per_frame;
    uint32_t max_bytes_per_second;
    uint32_t padding_granularity;
    uint32_t flags;
    uint32_t total_frames;
    uint32_t initial_frames;
    uint32_t streams;
    uint32_t suggested_buffer_size;
    uint32_t width;
    uint32_t height;
    uint32_t reserved[4];
};

struct Strh {
    char type[4];          // "vids"
    char handler[4];       // "MJPG"
    uint32_t flags;
    uint16_t priority;
    uint16_t language;
    uint32_t initial_frames;
    uint32_t scale;
    uint32_t rate;
    uint32_t start;
    uint32_t length;
    uint32_t suggested_buffer_size;
    uint32_t quality;
    uint32_t sample_size;
    int16_t frame_left;
    int16_t frame_top;
    int16_t frame_right;
    int16_t frame_bottom;
};

struct Strf {
    uint32_t size;         // sizeof(BITMAPINFOHEADER)
    int32_t width;
    int32_t height;        // positive: the JPEG payload is stored top-down
    uint16_t planes;
    uint16_t bit_count;
    uint32_t compression;  // 'MJPG'
    uint32_t image_size;
    int32_t x_pixels_per_meter;
    int32_t y_pixels_per_meter;
    uint32_t colors_used;
    uint32_t colors_important;
};

struct IndexEntry {
    uint32_t id;
    uint32_t flags;
    uint32_t offset;
    uint32_t length;
};

#pragma pack(pop)

constexpr uint32_t kAvifHasIndex = 0x00000010;
constexpr uint32_t kAviiFKeyFrame = 0x00000010;
constexpr uint32_t kNoQuality = 0xFFFFFFFF;
constexpr uint32_t kStreamChunkId = 0x63643030;   // '00dc'
constexpr uint32_t kMjpgFourcc = 0x47504A4D;      // 'MJPG'
constexpr uint32_t kRiffFourcc = 0x46464952;      // 'RIFF'
constexpr uint32_t kAviFourcc = 0x20495641;       // 'AVI '
constexpr uint32_t kListFourcc = 0x5453494C;      // 'LIST'
constexpr uint32_t kHdrlFourcc = 0x6C726468;      // 'hdrl'
constexpr uint32_t kMoviFourcc = 0x69766F6D;      // 'movi'
constexpr uint32_t kIdx1Fourcc = 0x31786469;      // 'idx1'
constexpr uint32_t kAvihFourcc = 0x68697661;      // 'avih'
constexpr uint32_t kStrlFourcc = 0x6C727473;      // 'strl'
constexpr uint32_t kStrhFourcc = 0x68727473;      // 'strh'
constexpr uint32_t kStrfFourcc = 0x66727473;      // 'strf'

bool write_bytes(std::FILE* file, const void* data, size_t size) {
    return fwrite(data, 1, size, file) == size;
}

bool write_u32(std::FILE* file, uint32_t value) {
    return write_bytes(file, &value, sizeof(value));
}

// A chunk whose payload is already in memory.
bool write_chunk(std::FILE* file, uint32_t fourcc, const void* payload, uint32_t size) {
    if (!write_u32(file, fourcc) || !write_u32(file, size)) return false;
    if (size > 0 && !write_bytes(file, payload, size)) return false;
    if (size % 2 == 1) {                    // RIFF chunks are word aligned
        const unsigned char pad = 0;
        return write_bytes(file, &pad, 1);
    }
    return true;
}

long long tick_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

VideoRecorder::VideoRecorder() = default;

VideoRecorder::~VideoRecorder() {
    stop();
}

bool VideoRecorder::start(const std::string& output_path, int width, int height,
                          double frame_rate, int max_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (recording_) {
        last_error_ = "A recording is already in progress.";
        return false;
    }
    if (output_path.empty()) {
        last_error_ = "No output path was given.";
        return false;
    }
    if (width <= 0 || height <= 0 || width > 32767 || height > 32767) {
        last_error_ = "The camera did not report a usable frame size.";
        return false;
    }
    if (frame_rate < 1.0 || frame_rate > 240.0) frame_rate = 30.0;
    if (max_seconds <= 0) max_seconds = 600;

    const std::wstring wide = pathutil::utf8_to_wide(output_path);
    if (_wfopen_s(&file_, wide.c_str(), L"wb") != 0 || !file_) {
        file_ = nullptr;
        last_error_ = "Could not open " + output_path + " for writing.";
        LOG_ERROR(kComponent, last_error_);
        return false;
    }

    if (!write_headers(width, height, frame_rate)) {
        last_error_ = "Could not write the AVI header to " + output_path + ".";
        LOG_ERROR(kComponent, last_error_);
        close_file();
        return false;
    }

    width_ = width;
    height_ = height;
    frame_rate_ = frame_rate;
    max_frames_ = (long long)(max_seconds * frame_rate + 0.5);
    frames_written_ = 0;
    last_frame_tick_ = 0;
    output_path_ = output_path;
    last_error_.clear();
    recording_ = true;

    LOG_INFO(kComponent, "Recording " + std::to_string(width) + "x" + std::to_string(height) +
        " at " + std::to_string((int)frame_rate) + " fps to " + output_path);
    return true;
}

bool VideoRecorder::write_headers(int width, int height, double frame_rate) {
    // RIFF header. The total size is only known once the file is closed, so
    // riff_size_offset_ remembers where to patch it.
    if (!write_u32(file_, kRiffFourcc)) return false;
    riff_size_offset_ = _ftelli64(file_);
    if (!write_u32(file_, 0) || !write_u32(file_, kAviFourcc)) return false;

    // hdrl: main header, then the single video stream's header list.
    const uint32_t strl_size = 4 + 8 + sizeof(Strh) + 8 + sizeof(Strf);
    const uint32_t hdrl_size = 4 + 8 + sizeof(Avih) + 8 + strl_size;
    if (!write_u32(file_, kListFourcc) || !write_u32(file_, hdrl_size) ||
        !write_u32(file_, kHdrlFourcc)) {
        return false;
    }

    Avih avih = {};
    avih.micro_seconds_per_frame = static_cast<uint32_t>(1000000.0 / frame_rate + 0.5);
    avih.flags = kAvifHasIndex;
    avih.total_frames = 0;                    // patched by stop()
    avih.streams = 1;
    avih.width = static_cast<uint32_t>(width);
    avih.height = static_cast<uint32_t>(height);
    // The chunk id sits here now; the field is 8 bytes of header further in.
    avih_total_frames_offset_ = _ftelli64(file_) + 8 + offsetof(Avih, total_frames);
    if (!write_chunk(file_, kAvihFourcc, &avih, sizeof(avih))) return false;

    if (!write_u32(file_, kListFourcc) || !write_u32(file_, strl_size) ||
        !write_u32(file_, kStrlFourcc)) {
        return false;
    }

    Strh strh = {};
    memcpy(strh.type, "vids", 4);
    memcpy(strh.handler, "MJPG", 4);
    strh.scale = 1;                          // stream time is in `rate` units
    strh.rate = static_cast<uint32_t>(frame_rate + 0.5);
    strh.length = 0;                         // patched by stop()
    strh.quality = kNoQuality;
    strh.frame_right = static_cast<int16_t>(width);
    strh.frame_bottom = static_cast<int16_t>(height);
    strh_length_offset_ = _ftelli64(file_) + 8 + offsetof(Strh, length);
    if (!write_chunk(file_, kStrhFourcc, &strh, sizeof(strh))) return false;

    Strf strf = {};
    strf.size = sizeof(Strf);
    strf.width = width;
    strf.height = height;                    // positive: payload is top-down
    strf.planes = 1;
    strf.bit_count = 24;
    strf.compression = kMjpgFourcc;
    strf.image_size = static_cast<uint32_t>(width * height * 3);
    if (!write_chunk(file_, kStrfFourcc, &strf, sizeof(strf))) return false;

    // The frame chunks go inside this list; its size is patched by stop() too.
    if (!write_u32(file_, kListFourcc)) return false;
    movi_size_offset_ = _ftelli64(file_);
    if (!write_u32(file_, 0) || !write_u32(file_, kMoviFourcc)) return false;
    movi_fourcc_offset_ = _ftelli64(file_) - 4;
    return true;
}

bool VideoRecorder::add_frame(const unsigned char* rgb, int width, int height) {
    if (!rgb || width <= 0 || height <= 0) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!recording_ || !file_) return false;
    if (width != width_ || height != height_) return false;   // size renegotiated

    // Pacing: the camera can push frames faster than the declared rate and the
    // encoder can be slower still. Dropping the surplus is what keeps the
    // finished video the same length as the recording.
    const long long now = tick_now_ms();
    if (last_frame_tick_ != 0 && now - last_frame_tick_ < (long long)(1000.0 / frame_rate_)) {
        return true;                          // too early, but not an error
    }
    last_frame_tick_ = now;

    std::vector<unsigned char> jpeg;
    std::string error;
    if (!imageutil::encode_jpeg(rgb, width, height, imageutil::kVideoJpegQuality, jpeg, error)) {
        LOG_WARN(kComponent, "Dropping a frame: " + error);
        return true;
    }

    const long long chunk_offset = _ftelli64(file_) - movi_fourcc_offset_;
    if (!write_chunk(file_, kStreamChunkId, jpeg.data(), static_cast<uint32_t>(jpeg.size()))) {
        last_error_ = "Writing a frame to the AVI failed.";
        LOG_ERROR(kComponent, last_error_);
        stop_locked();
        return false;
    }
    frame_index_.emplace_back(static_cast<uint32_t>(chunk_offset),
                              static_cast<uint32_t>(jpeg.size()));
    ++frames_written_;

    if (max_frames_ > 0 && frames_written_ >= max_frames_) {
        LOG_WARN(kComponent, "Duration cap reached - finishing the recording automatically");
        stop_locked();
    }
    return true;
}

bool VideoRecorder::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_locked();
    return !last_error_.empty() ? false : true;
}

void VideoRecorder::stop_locked() {
    if (!file_) return;

    if (frames_written_ == 0) {
        last_error_ = "No frames were captured, so there is nothing to save.";
    } else {
        write_index();
        patch_sizes();
        if (fflush(file_) != 0) {
            last_error_ = "Flushing " + output_path_ + " failed.";
        }
    }
    const bool ok = last_error_.empty();
    const long long frames = frames_written_;
    const double seconds = (frame_rate_ > 0.0) ? static_cast<double>(frames) / frame_rate_ : 0.0;
    close_file();
    recording_ = false;

    if (ok) {
        LOG_INFO(kComponent, "Saved " + std::to_string(frames) + " frames (" +
            std::to_string((int)(seconds * 10.0) / 10.0) + "s) to " + output_path_);
    } else {
        LOG_ERROR(kComponent, last_error_);
    }
}

void VideoRecorder::write_index() {
    if (!file_ || frame_index_.empty()) return;

    if (!write_u32(file_, kIdx1Fourcc) ||
        !write_u32(file_, static_cast<uint32_t>(frame_index_.size() * sizeof(IndexEntry)))) {
        return;
    }
    for (const auto& frame : frame_index_) {
        IndexEntry entry = {};
        entry.id = kStreamChunkId;
        entry.flags = kAviiFKeyFrame;         // every Motion-JPEG frame is one
        entry.offset = frame.first;
        entry.length = frame.second;
        if (!write_bytes(file_, &entry, sizeof(entry))) break;
    }
}

void VideoRecorder::patch_sizes() {
    // Everything the header could only guess at is written now that the file
    // length, the frame count and the index are known.
    const long long end = _ftelli64(file_);
    auto patch_u32 = [this](long long offset, uint32_t value) {
        if (offset < 0) return;
        if (_fseeki64(file_, offset, SEEK_SET) != 0) return;
        write_u32(file_, value);
    };
    patch_u32(riff_size_offset_, static_cast<uint32_t>(end - 8));
    patch_u32(movi_size_offset_, static_cast<uint32_t>(end - (movi_size_offset_ + 4)));
    patch_u32(avih_total_frames_offset_, static_cast<uint32_t>(frames_written_));
    patch_u32(strh_length_offset_, static_cast<uint32_t>(frames_written_));
    _fseeki64(file_, end, SEEK_SET);
}

void VideoRecorder::close_file() {
    if (!file_) return;
    fclose(file_);
    file_ = nullptr;
}

long long VideoRecorder::frames_written() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_written_;
}

double VideoRecorder::recorded_seconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frame_rate_ <= 0.0) return 0.0;
    return static_cast<double>(frames_written_) / frame_rate_;
}

const std::string& VideoRecorder::output_path() const {
    return output_path_;
}

std::string VideoRecorder::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

} // namespace Jarvis
