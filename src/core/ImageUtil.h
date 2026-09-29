#pragma once
// ============================================================================
// ImageUtil.h - shared RGB24 -> image helpers (GDI+).
//
// Both the still photos and the video frames go through here: the photos are
// written straight to a PNG/JPEG file, the video frames are encoded to JPEG in
// memory for the AVI muxer in VideoRecorder.
//
// GDI+ is started once per process and never shut down: captures happen on
// whichever thread takes a photo, and a shutdown here would race with one.
// ============================================================================

#include <windows.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "PathUtil.h"

namespace Jarvis {
namespace imageutil {

// JPEG quality used for video frames. 80 keeps 640x480 around 40 KB a frame,
// which is sharp enough to read text on a desk without filling the disk.
inline const int kVideoJpegQuality = 80;

inline void start() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        ULONG_PTR token = 0;
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
            // Nothing can be encoded without GDI+; the callers turn the missing
            // image into an error on their own when the save below fails.
            OutputDebugStringA("GdiplusStartup failed\n");
        }
    });
}

inline CLSID encoder(const wchar_t* mime_type) {
    UINT count = 0;
    UINT bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || bytes == 0) {
        return CLSID();
    }
    std::vector<BYTE> storage(bytes);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok) {
        return CLSID();
    }
    for (UINT i = 0; i < count; ++i) {
        if (encoders[i].MimeType && wcscmp(encoders[i].MimeType, mime_type) == 0) {
            return encoders[i].Clsid;
        }
    }
    return CLSID();
}

// GDI+ wants every row 4-byte aligned; camera rows are width * 3.
inline void to_aligned_rows(const unsigned char* rgb, int width, int height,
                            std::vector<unsigned char>& out) {
    const size_t stride = (static_cast<size_t>(width) * 3 + 3) & ~static_cast<size_t>(3);
    out.assign(stride * static_cast<size_t>(height), 0);
    for (int y = 0; y < height; ++y) {
        memcpy(out.data() + static_cast<size_t>(y) * stride,
               rgb + static_cast<size_t>(y) * width * 3, static_cast<size_t>(width) * 3);
    }
}

// Fills `parameters` with a single quality setting. `storage` must outlive the
// call: EncoderParameter keeps a pointer to the value, not the value itself.
inline void set_quality(Gdiplus::EncoderParameters& parameters, ULONG& storage, int quality) {
    storage = static_cast<ULONG>(quality);
    parameters.Count = 1;
    parameters.Parameter[0].Guid = Gdiplus::EncoderQuality;
    parameters.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
    parameters.Parameter[0].NumberOfValues = 1;
    parameters.Parameter[0].Value = &storage;
}

// Writes a top-down RGB24 frame as a PNG or JPEG file. `format` is "png" or
// "jpg"; anything else falls back to PNG.
inline bool save_file(const unsigned char* rgb, int width, int height,
                      const std::string& utf8_path, const std::string& format,
                      std::string& out_error) {
    start();
    const bool jpeg = (format == "jpg" || format == "jpeg");
    const CLSID clsid = encoder(jpeg ? L"image/jpeg" : L"image/png");
    if (clsid == CLSID()) {
        out_error = jpeg ? "No JPEG encoder is available." : "No PNG encoder is available.";
        return false;
    }

    std::vector<unsigned char> rows;
    to_aligned_rows(rgb, width, height, rows);

    Gdiplus::Bitmap bitmap(width, height, static_cast<INT>(rows.size() / static_cast<size_t>(height)),
                           PixelFormat24bppRGB, rows.data());
    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        out_error = "The captured frame could not be prepared for encoding.";
        return false;
    }

    ULONG quality = 0;
    Gdiplus::EncoderParameters parameters;
    set_quality(parameters, quality, jpeg ? 92 : kVideoJpegQuality);

    const std::wstring wide = pathutil::utf8_to_wide(utf8_path);
    if (bitmap.Save(wide.c_str(), &clsid, &parameters) != Gdiplus::Ok) {
        out_error = "Writing " + utf8_path + " failed.";
        return false;
    }
    return true;
}

// Encodes a frame to JPEG in memory (used for the video stream).
inline bool encode_jpeg(const unsigned char* rgb, int width, int height, int quality_level,
                        std::vector<unsigned char>& out_jpeg, std::string& out_error) {
    start();
    const CLSID clsid = encoder(L"image/jpeg");
    if (clsid == CLSID()) {
        out_error = "No JPEG encoder is available.";
        return false;
    }

    std::vector<unsigned char> rows;
    to_aligned_rows(rgb, width, height, rows);
    Gdiplus::Bitmap bitmap(width, height, static_cast<INT>(rows.size() / static_cast<size_t>(height)),
                           PixelFormat24bppRGB, rows.data());
    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        out_error = "The frame could not be prepared for encoding.";
        return false;
    }

    ULONG quality = 0;
    Gdiplus::EncoderParameters parameters;
    set_quality(parameters, quality, quality_level);

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, 0);
    if (!memory) {
        out_error = "Out of memory while encoding a frame.";
        return false;
    }
    IStream* stream = nullptr;
    HRESULT hr = CreateStreamOnHGlobal(memory, TRUE, &stream);
    if (FAILED(hr) || !stream) {
        GlobalFree(memory);
        out_error = "Could not create the encoder stream.";
        return false;
    }

    const Gdiplus::Status saved = bitmap.Save(stream, &clsid, &parameters);
    bool ok = false;
    if (saved == Gdiplus::Ok) {
        LARGE_INTEGER start = {};
        hr = stream->Seek(start, STREAM_SEEK_SET, nullptr);
        const SIZE_T size = GlobalSize(memory);
        if (SUCCEEDED(hr) && size > 0) {
            std::vector<unsigned char> bytes(static_cast<size_t>(size));
            ULONG read = 0;
            if (SUCCEEDED(stream->Read(bytes.data(), static_cast<ULONG>(size), &read)) && read > 0) {
                bytes.resize(read);
                // GDI+ pads a stream save with a trailing null; AVI frame sizes
                // must be exact, and a decoder ignores the padding either way.
                while (!bytes.empty() && bytes.back() == 0) bytes.pop_back();
                out_jpeg.swap(bytes);
                ok = true;
            }
        }
    }
    if (!ok) {
        out_error = "Encoding the frame to JPEG failed.";
    }
    stream->Release();   // releases the global too (fDeleteOnRelease was TRUE)
    return ok;
}

} // namespace imageutil
} // namespace Jarvis
