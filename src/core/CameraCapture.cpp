#include "CameraCapture.h"
#include "ImageUtil.h"
#include "Logger.h"
#include "PathUtil.h"
#include "VideoRecorder.h"

#ifdef _WIN32
#include <windows.h>
#include <initguid.h>
#include <dshow.h>
#include <mfobjects.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")

// Windows SDK 10.0.26100.0 dropped the legacy DirectShow Sample Grabber header
// (qedit.h): the CLSIDs/IIDs it defined and the two interfaces it declared are
// reproduced here with the values from qedit.h and the DirectShow docs. Each
// IID must match the UUID in the MIDL_INTERFACE declaration below, otherwise
// QueryInterface fails with E_NOINTERFACE (0x80004002).
#ifndef CLSID_SampleGrabber
// {C1F400A0-3F08-11d3-9F0B-006008039E37} - implemented by qedit.dll
DEFINE_GUID(CLSID_SampleGrabber,
    0xc1f400a0, 0x3f08, 0x11d3, 0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37);
#endif

#ifndef CLSID_NullRenderer
// {C1F400A4-3F08-11d3-9F0B-006008039E37} - discards the samples the grabber
// forwards, so the graph needs no video preview window on the desktop.
DEFINE_GUID(CLSID_NullRenderer,
    0xc1f400a4, 0x3f08, 0x11d3, 0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37);
#endif

#ifndef CLSID_VideoInputDevice
// Same GUID as CLSID_VideoInputDeviceCategory in uuids.h
// ({860BB310-5D01-11d0-BD3B-00A0C911CE86})
DEFINE_GUID(CLSID_VideoInputDevice,
    0x860bb310, 0x5d01, 0x11d0, 0xbd, 0x3b, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86);
#endif

#ifndef IID_ISampleGrabber
// {6B652FFF-11FE-4fce-92AD-0266B5D7C78F}
DEFINE_GUID(IID_ISampleGrabber,
    0x6b652fff, 0x11fe, 0x4fce, 0x92, 0xad, 0x02, 0x66, 0xb5, 0xd7, 0xc7, 0x8f);
#endif

#ifndef IID_ISampleGrabberCB
// {0579154A-2B53-4994-B0D0-E773148EFF85}
DEFINE_GUID(IID_ISampleGrabberCB,
    0x0579154a, 0x2b53, 0x4994, 0xb0, 0xd0, 0xe7, 0x73, 0x14, 0x8e, 0xff, 0x85);
#endif

// ISampleGrabberCB: the callback the grabber invokes for every sample. The
// vtable order/signatures come from qedit.h (SampleCB, then BufferCB), so they
// must stay exactly as declared.
MIDL_INTERFACE("0579154a-2b53-4994-b0d0-e773148eff85")
ISampleGrabberCB : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE SampleCB(
        double SampleTime,
        IMediaSample *pSample) = 0;

    virtual HRESULT STDMETHODCALLTYPE BufferCB(
        double SampleTime,
        BYTE *pBuffer,
        long BufferLen) = 0;
};

// ISampleGrabber (qedit.h). The methods are listed in COM vtable order and are
// called straight into qedit.dll's implementation, so neither the order nor the
// signatures may be changed.
MIDL_INTERFACE("6b652fff-11fe-4fce-92ad-0266b5d7c78f")
ISampleGrabber : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(
        BOOL OneShot) = 0;

    virtual HRESULT STDMETHODCALLTYPE SetMediaType(
        const AM_MEDIA_TYPE *pType) = 0;

    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(
        AM_MEDIA_TYPE *pType) = 0;

    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(
        BOOL BufferThem) = 0;

    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(
        long *pBufferSize,
        long *pBuffer) = 0;

    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(
        IMediaSample **ppSample) = 0;

    virtual HRESULT STDMETHODCALLTYPE SetCallback(
        ISampleGrabberCB *pCallback,
        long WhichMethodToCallback) = 0;
};

// Releases what DirectShow placed inside a caller-owned AM_MEDIA_TYPE (as
// returned by ISampleGrabber::GetConnectedMediaType). The struct itself belongs
// to the caller, so only pbFormat/pUnk are freed.
inline void FreeMediaTypeFormat(AM_MEDIA_TYPE &mt)
{
    if (mt.pbFormat) CoTaskMemFree(mt.pbFormat);
    if (mt.pUnk) mt.pUnk->Release();
    mt.pbFormat = nullptr;
    mt.pUnk = nullptr;
}

// HRESULTs are readable in hex (0x80004002) but printed as a large negative
// decimal by std::to_string(), which hides exactly which failure it was.
static std::string hresult_to_hex(HRESULT hr)
{
    char buffer[16];
    sprintf_s(buffer, sizeof(buffer), "0x%08lX",
              static_cast<unsigned long>(static_cast<unsigned int>(hr)));
    return std::string(buffer);
}

// Captures are named after the moment they were taken, so several taken in the
// same minute stay distinguishable.
static std::string timestamp_suffix()
{
    const std::time_t now = std::time(nullptr);
    std::tm parts;
    localtime_s(&parts, &now);
    char buffer[32];
    buffer[0] = '\0';
    strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &parts);
    return std::string(buffer);
}

#ifndef PIN_CATEGORY_VIDEO
DEFINE_GUID(PIN_CATEGORY_VIDEO,
    0x73646576, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
#endif
#endif

namespace Jarvis {

// A recording that nobody stops is finalised automatically after this long.
static const int kMaxRecordingSeconds = 600;

std::atomic<CameraCapture*> CameraCapture::active_instance_{nullptr};

std::vector<std::string> CameraCapture::enumerate_cameras() {
#ifdef _WIN32
    std::vector<std::string> camera_names;

    // Try to initialize COM - if it's already initialized, use the existing instance
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool com_initialized = (hr == S_OK || hr == S_FALSE);
    // S_FALSE means COM was already initialized

    ICreateDevEnum* devEnum = nullptr;
    hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&devEnum));
    if (FAILED(hr) || !devEnum) {
        std::cerr << "CameraCapture::enumerate_cameras: Failed to create System Device Enumerator" << std::endl;
        if (com_initialized) CoUninitialize();
        return camera_names;
    }

    IEnumMoniker* enumMoniker = nullptr;
    hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDevice, &enumMoniker, 0);
    if (FAILED(hr) || !enumMoniker) {
        std::cerr << "CameraCapture::enumerate_cameras: No video capture devices found" << std::endl;
        devEnum->Release();
        if (com_initialized) CoUninitialize();
        return camera_names;
    }

    IMoniker* moniker = nullptr;
    ULONG fetched = 0;
    int camera_count = 0;
    while (enumMoniker->Next(1, &moniker, &fetched) == S_OK && moniker) {
        IPropertyBag* propBag = nullptr;
        hr = moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&propBag));
        if (SUCCEEDED(hr) && propBag) {
            VARIANT var;
            VariantInit(&var);
            hr = propBag->Read(L"FriendlyName", &var, nullptr);
            if (SUCCEEDED(hr)) {
                // Convert BSTR to std::string
                int len = WideCharToMultiByte(CP_UTF8, 0, var.bstrVal, -1, nullptr, 0, nullptr, nullptr);
                if (len > 0) {
                    std::string name(len - 1, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, var.bstrVal, -1, &name[0], len, nullptr, nullptr);
                    camera_names.push_back(name);
                    camera_count++;
                    std::cout << "Found camera: " << name << std::endl;
                }
                VariantClear(&var);
            }
            propBag->Release();
        }
        moniker->Release();
    }

    std::cout << "CameraCapture::enumerate_cameras: Found " << camera_count << " cameras" << std::endl;

    enumMoniker->Release();
    devEnum->Release();
    if (com_initialized) CoUninitialize();

    return camera_names;
#else
    return {}; // Not implemented on non-Windows platforms
#endif
}

CameraCapture::CameraCapture() : running_(false), stop_requested_(false) {
    // The recording sink is a member, not a local of start(): the capture
    // thread outlives start() and keeps feeding frames to it.
    frame_sink_ = [this](const unsigned char* rgb, int width, int height) {
        feed_recorder(rgb, width, height);
    };
    active_instance_.store(this);
}

CameraCapture::~CameraCapture() {
    shutdown();
    CameraCapture* self = active_instance_.load();
    if (self == this) active_instance_.store(nullptr);
}

CameraCapture* CameraCapture::active() {
    return active_instance_.load();
}

bool CameraCapture::initialize() {
    return true;
}

void CameraCapture::shutdown() {
    stop();
}

#ifdef _WIN32
// SampleGrabber callback: turns every sample the graph pushes into top-down
// RGB24 (width * height * 3), the layout capture_frame() hands to the vision
// path, and stores it for the orchestrator to pick up.
class SGCallback : public ISampleGrabberCB {
public:
    SGCallback(std::vector<unsigned char>* frame, int* width, int* height,
               int* source_stride, int* flip_vertical, std::mutex* m,
               double* frame_rate, std::function<void(const unsigned char*, int, int)>* on_frame)
        : refCount(1), frame_(frame), width_(width), height_(height),
          source_stride_(source_stride), flip_vertical_(flip_vertical), mtx_(m),
          frame_rate_(frame_rate), on_frame_(on_frame) {}

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) {
        if (riid == IID_IUnknown || riid == IID_ISampleGrabberCB) {
            *ppv = (void*)this;
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() { return InterlockedIncrement(&refCount); }
    STDMETHOD_(ULONG, Release)() {
        LONG c = InterlockedDecrement(&refCount);
        if (c == 0) delete this;
        return c;
    }
    STDMETHOD(BufferCB)(double SampleTime, BYTE* pBuffer, long BufferLen) {
        if (!pBuffer || BufferLen <= 0) return S_OK;

        // The negotiated media type carries no frame rate, so it is measured
        // from the timestamps the graph stamps on the samples. Only touched by
        // the streaming thread.
        if (SampleTime > 0.0) {
            if (last_sample_time_ > 0.0) {
                const double delta = SampleTime - last_sample_time_;
                if (delta > 0.0005 && delta < 1.0) {
                    const double instant = 1.0 / delta;
                    measured_fps_ = (timed_samples_ == 0) ? instant
                                                         : (measured_fps_ * 0.8 + instant * 0.2);
                    if (++timed_samples_ >= 8 && frame_rate_) {
                        std::lock_guard<std::mutex> rate_lock(*mtx_);
                        *frame_rate_ = measured_fps_;
                    }
                }
            }
            last_sample_time_ = SampleTime;
        }

        std::lock_guard<std::mutex> lock(*mtx_);
        const int width = width_ ? *width_ : 0;
        const int height = height_ ? *height_ : 0;
        // 3 = RGB24 (samples arrive as-is), 4 = RGB32 (BGRA, converted below).
        const int stride = source_stride_ ? *source_stride_ : 0;
        if (width <= 0 || height <= 0 || (stride != 3 && stride != 4)) return S_OK;

        const long pixel_count = static_cast<long>(width) * height;
        if (BufferLen < pixel_count * stride) return S_OK;  // truncated sample

        // RGB24 samples store R,G,B while RGB32 samples are BGRA, so the red and
        // blue byte offsets differ between the two layouts.
        const bool flip = flip_vertical_ && *flip_vertical_ != 0;

        frame_->resize(static_cast<size_t>(pixel_count) * 3);
        unsigned char* destination = frame_->data();

        if (stride == 3) {
            // Already RGB24: copy whole rows (a flat copy when the rows are
            // already in top-down order, which is the usual case).
            const size_t row_bytes = static_cast<size_t>(width) * 3;
            for (long row = 0; row < height; ++row) {
                const unsigned char* source_row = pBuffer +
                    (flip ? (height - 1 - row) : row) * width * 3;
                std::memcpy(destination + static_cast<size_t>(row) * row_bytes,
                            source_row, row_bytes);
            }
        } else {
            const long red_offset = 2;    // RGB32 is BGRA
            const long green_offset = 1;
            const long blue_offset = 0;
            size_t out = 0;
            for (long row = 0; row < height; ++row) {
                // A positive biHeight means the sample rows are stored bottom-up.
                const unsigned char* source_row = pBuffer +
                    (flip ? (height - 1 - row) : row) * width * stride;
                for (long column = 0; column < width; ++column) {
                    const unsigned char* pixel = source_row + column * stride;
                    destination[out++] = pixel[red_offset];
                    destination[out++] = pixel[green_offset];
                    destination[out++] = pixel[blue_offset];
                }
            }
        }

        // Hand the frame to an active recording. Still holding *mtx_, so the
        // recorder copies it before another sample can overwrite frame_.
        if (on_frame_ && *on_frame_) {
            (*on_frame_)(frame_->data(), width, height);
        }
        return S_OK;
    }
    STDMETHOD(SampleCB)(double SampleTime, IMediaSample* pSample) {
        (void)SampleTime;
        (void)pSample;
        return S_OK;
    }

private:
    LONG refCount;
    std::vector<unsigned char>* frame_;
    int* width_;
    int* height_;
    int* source_stride_;
    int* flip_vertical_;
    std::mutex* mtx_;
    double* frame_rate_;
    std::function<void(const unsigned char*, int, int)>* on_frame_;
    double last_sample_time_ = -1.0;
    double measured_fps_ = 0.0;
    int timed_samples_ = 0;
};
#endif

bool CameraCapture::start() {
#ifdef _WIN32
    if (running_) return true;
    stop_requested_ = false;
    running_ = true;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_frame_.clear();
        latest_width_ = 0;
        latest_height_ = 0;
    }

    capture_thread_ = std::thread([this]() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) {
            LOG_ERROR("CameraCapture", "CoInitializeEx failed in capture thread");
            running_ = false;
            return;
        }

        // Enumerate video capture devices (webcams)
        ICreateDevEnum* devEnum = nullptr;
        hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&devEnum));
        if (FAILED(hr) || !devEnum) {
            LOG_WARN("CameraCapture", "Failed to create System Device Enumerator");
            CoUninitialize();
            running_ = false;
            return;
        }

        IEnumMoniker* enumMoniker = nullptr;
        hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDevice, &enumMoniker, 0);
        if (FAILED(hr) || !enumMoniker) {
            LOG_WARN("CameraCapture", "No video capture devices found");
            devEnum->Release();
            CoUninitialize();
            running_ = false;
            return;
        }

        // Find the requested camera device by name
        IMoniker* selectedMoniker = nullptr;
        IMoniker* moniker = nullptr;
        ULONG fetched = 0;
        bool found_requested = false;

        // Reset enumerator to start from beginning
        enumMoniker->Reset();

        while (enumMoniker->Next(1, &moniker, &fetched) == S_OK && moniker) {
            IPropertyBag* propBag = nullptr;
            hr = moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&propBag));
            if (SUCCEEDED(hr) && propBag) {
                VARIANT var;
                VariantInit(&var);
                hr = propBag->Read(L"FriendlyName", &var, nullptr);
                if (SUCCEEDED(hr)) {
                    // Convert BSTR to std::string
                    int len = WideCharToMultiByte(CP_UTF8, 0, var.bstrVal, -1, nullptr, 0, nullptr, nullptr);
                    if (len > 0) {
                        std::string name(len - 1, '\0');
                        WideCharToMultiByte(CP_UTF8, 0, var.bstrVal, -1, &name[0], len, nullptr, nullptr);

                        // Check if this is the requested camera
                        if (!camera_device_name_.empty() && name == camera_device_name_) {
                            selectedMoniker = moniker;
                            found_requested = true;
                            LOG_INFO("CameraCapture", "Selected camera: " + name);
                            moniker->AddRef(); // Add reference since we're keeping it
                            VariantClear(&var);
                            propBag->Release();
                            // Release other monikers
                            while (enumMoniker->Next(1, &moniker, &fetched) == S_OK && moniker) {
                                moniker->Release();
                            }
                            break;
                        }
                    }
                    VariantClear(&var);
                }
                propBag->Release();
            }

            // Keep the first camera as fallback if no specific camera was requested
            if (!selectedMoniker) {
                selectedMoniker = moniker;
                selectedMoniker->AddRef();
            } else {
                moniker->Release();
            }
        }

        if (!selectedMoniker) {
            LOG_WARN("CameraCapture", "No video capture device found");
            enumMoniker->Release();
            devEnum->Release();
            CoUninitialize();
            running_ = false;
            return;
        }

        if (!found_requested && !camera_device_name_.empty()) {
            LOG_WARN("CameraCapture", "Requested camera '" + camera_device_name_ + "' not found, using default camera");
        }

        moniker = selectedMoniker;
        LOG_INFO("CameraCapture", "Found video capture device");

        // Create filter graph
        IGraphBuilder* graphBuilder = nullptr;
        hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IGraphBuilder, (void**)&graphBuilder);
        if (FAILED(hr) || !graphBuilder) {
            LOG_ERROR("CameraCapture", "Failed to create Filter Graph");
            moniker->Release();
            enumMoniker->Release();
            devEnum->Release();
            CoUninitialize();
            running_ = false;
            return;
        }

        ICaptureGraphBuilder2* capBuilder = nullptr;
        hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER,
                              IID_ICaptureGraphBuilder2, (void**)&capBuilder);
        if (FAILED(hr) || !capBuilder) {
            LOG_ERROR("CameraCapture", "Failed to create Capture Graph Builder");
            graphBuilder->Release();
            moniker->Release();
            enumMoniker->Release();
            devEnum->Release();
            CoUninitialize();
            running_ = false;
            return;
        }
        capBuilder->SetFiltergraph(graphBuilder);

        // Bind moniker to capture filter
        IBaseFilter* capFilter = nullptr;
        hr = moniker->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)&capFilter);
        if (FAILED(hr) || !capFilter) {
            LOG_ERROR("CameraCapture", "Failed to bind capture device to filter");
            capBuilder->Release();
            graphBuilder->Release();
            moniker->Release();
            enumMoniker->Release();
            devEnum->Release();
            CoUninitialize();
            running_ = false;
            return;
        }
        graphBuilder->AddFilter(capFilter, L"Capture");

        // Create Sample Grabber filter (create as IBaseFilter first, then QI to ISampleGrabber)
        IBaseFilter* grabberFilter = nullptr;
        ISampleGrabber* sampleGrabber = nullptr;
        hr = CoCreateInstance(CLSID_SampleGrabber, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IBaseFilter, (void**)&grabberFilter);
        if (FAILED(hr) || !grabberFilter) {
            LOG_ERROR("CameraCapture", "Failed to create Sample Grabber filter (HRESULT: " +
                hresult_to_hex(hr) + ")");
            capFilter->Release();
            capBuilder->Release();
            graphBuilder->Release();
            moniker->Release();
            enumMoniker->Release();
            devEnum->Release();
            CoUninitialize();
            running_ = false;
            return;
        }
        hr = grabberFilter->QueryInterface(IID_ISampleGrabber, (void**)&sampleGrabber);
        if (FAILED(hr) || !sampleGrabber) {
            LOG_ERROR("CameraCapture", "Sample Grabber does not support ISampleGrabber (HRESULT: " +
                hresult_to_hex(hr) + ")");
            grabberFilter->Release();
            capFilter->Release();
            capBuilder->Release();
            graphBuilder->Release();
            moniker->Release();
            enumMoniker->Release();
            devEnum->Release();
            CoUninitialize();
            running_ = false;
            return;
        }

        // Frames are consumed by the grabber, so the graph is terminated by a
        // Null Renderer. Rendering the capture pin with the default renderer
        // instead would open a video preview window on the desktop.
        IBaseFilter* nullRenderer = nullptr;
        hr = CoCreateInstance(CLSID_NullRenderer, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IBaseFilter, (void**)&nullRenderer);
        if (FAILED(hr) || !nullRenderer) {
            LOG_WARN("CameraCapture", "Null Renderer unavailable (HRESULT: " + hresult_to_hex(hr) +
                ") - falling back to the default video renderer");
            nullRenderer = nullptr;
        } else {
            graphBuilder->AddFilter(nullRenderer, L"NullRenderer");
        }

        // Ask the grabber for uncompressed samples. Only the media type is fixed
        // here: the frame size and byte order are whatever the graph negotiates
        // between the camera pin and the grabber, and are read back with
        // GetConnectedMediaType() below.
        AM_MEDIA_TYPE mt = {};
        mt.majortype = MEDIATYPE_Video;
        mt.subtype = MEDIASUBTYPE_RGB24;
        mt.formattype = FORMAT_VideoInfo;
        hr = sampleGrabber->SetMediaType(&mt);
        if (FAILED(hr)) {
            LOG_WARN("CameraCapture", "RGB24 not supported, trying RGB32");
            mt.subtype = MEDIASUBTYPE_RGB32;
            hr = sampleGrabber->SetMediaType(&mt);
            if (FAILED(hr)) {
                LOG_ERROR("CameraCapture", "Failed to set media type on Sample Grabber");
            }
        }

        // Describes the negotiated sample layout for the callback. Both values
        // are filled in after RenderStream() and only read from the streaming
        // thread once the graph runs, so no locking is needed here.
        int source_stride = 0;          // bytes per pixel: 3 = RGB24, 4 = RGB32
        int source_flip_vertical = 0;   // 1 when the rows arrive bottom-up

        SGCallback* cb = new SGCallback(&latest_frame_, &latest_width_, &latest_height_,
                                        &source_stride, &source_flip_vertical, &mutex_,
                                        &source_frame_rate_, &frame_sink_);
        sampleGrabber->SetCallback(cb, 1);

        // Add grabber filter to graph
        graphBuilder->AddFilter(grabberFilter, L"SampleGrabber");

        // Render the capture stream: camera pin -> Sample Grabber -> Null Renderer.
        // PIN_CATEGORY_CAPTURE (not PIN_CATEGORY_VIDEO) is the pin category a
        // capture-only device exposes: with PIN_CATEGORY_VIDEO RenderStream fails
        // with E_INVALIDARG (0x80070057) on cameras that have no preview pin.
        // The Capture Graph Builder inserts whatever conversion filters are needed
        // to honour the grabber's RGB24 request (webcams typically offer only
        // MJPG/YUY2 natively).
        hr = capBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                      capFilter, grabberFilter, nullRenderer);
        if (FAILED(hr)) {
            LOG_ERROR("CameraCapture", "Failed to render video stream (HRESULT: " + hresult_to_hex(hr) + ")");
            // The graph builder and the Sample Grabber hold references to the
            // filters and to the callback, so they are released first.
            capBuilder->Release();
            graphBuilder->Release();
            if (nullRenderer) nullRenderer->Release();
            grabberFilter->Release();
            sampleGrabber->Release();
            capFilter->Release();
            cb->Release();
            moniker->Release();
            enumMoniker->Release();
            devEnum->Release();
            CoUninitialize();
            running_ = false;
            return;
        }

        // Read back what the graph negotiated: the frame size, whether samples
        // are RGB24 or RGB32, and the row order inside a sample.
        {
            AM_MEDIA_TYPE connected = {};
            hr = sampleGrabber->GetConnectedMediaType(&connected);
            if (SUCCEEDED(hr)) {
                int negotiated_width = 0;
                int negotiated_height = 0;
                if (connected.formattype == FORMAT_VideoInfo && connected.pbFormat) {
                    const VIDEOINFOHEADER* video_info =
                        reinterpret_cast<const VIDEOINFOHEADER*>(connected.pbFormat);
                    const LONG header_height = video_info->bmiHeader.biHeight;
                    negotiated_width = video_info->bmiHeader.biWidth;
                    negotiated_height = (header_height < 0) ? -header_height : header_height;
                    // DirectShow samples are bottom-up when biHeight is positive.
                    source_flip_vertical = (header_height > 0) ? 1 : 0;
                }
                if (connected.subtype == MEDIASUBTYPE_RGB24) {
                    source_stride = 3;
                } else if (connected.subtype == MEDIASUBTYPE_RGB32) {
                    source_stride = 4;
                }

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    latest_width_ = negotiated_width;
                    latest_height_ = negotiated_height;
                }

                LOG_INFO("CameraCapture", "Camera stream negotiated: " +
                    std::to_string(negotiated_width) + "x" + std::to_string(negotiated_height) +
                    (source_stride == 3 ? " RGB24" : (source_stride == 4 ? " RGB32" : " (compressed)")) +
                    (source_flip_vertical ? " bottom-up rows" : " top-down rows"));
                if (source_stride == 0) {
                    LOG_WARN("CameraCapture", "The camera negotiated a compressed pixel format - "
                        "no frames can be decoded, vision requests fall back to a screen grab");
                }

                FreeMediaTypeFormat(connected);
            } else {
                LOG_WARN("CameraCapture", "Could not query the connected media type (HRESULT: " +
                    hresult_to_hex(hr) + ")");
            }
        }

        // Start capture
        IMediaControl* mediaControl = nullptr;
        if (SUCCEEDED(graphBuilder->QueryInterface(IID_IMediaControl, (void**)&mediaControl)) && mediaControl) {
            hr = mediaControl->Run();
            if (FAILED(hr)) {
                LOG_ERROR("CameraCapture", "Failed to start video capture (HRESULT: " + hresult_to_hex(hr) + ")");
            } else {
                LOG_INFO("CameraCapture", "Capturing from camera at " +
                    std::to_string(latest_width_) + "x" + std::to_string(latest_height_));
            }

            while (!stop_requested_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            mediaControl->StopWhenReady();
            mediaControl->Release();
        }

        // Tear the graph down before the callback: the filters hold references to
        // it through ISampleGrabber::SetCallback().
        capBuilder->Release();
        graphBuilder->Release();
        if (nullRenderer) nullRenderer->Release();
        grabberFilter->Release();
        sampleGrabber->Release();
        capFilter->Release();
        cb->Release();
        moniker->Release();
        enumMoniker->Release();
        devEnum->Release();
        CoUninitialize();

        LOG_INFO("CameraCapture", "Capture thread exited");
    });

    LOG_INFO("CameraCapture", "Camera feed started (toggle on)");
    return true;
#else
    LOG_WARN("CameraCapture", "Camera capture is only supported on Windows");
    return false;
#endif
}

void CameraCapture::stop() {
    if (!running_) return;

    stop_requested_ = true;
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    running_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_frame_.clear();
        latest_width_ = 0;
        latest_height_ = 0;
    }

    // A recording left running after the feed goes down would hold an open
    // writer with no frames feeding it, so it is finalised here.
    if (is_recording()) {
        std::string saved_path;
        std::string error;
        stop_recording(saved_path, error);
    }
    LOG_INFO("CameraCapture", "Camera feed stopped (toggle off)");
}

void CameraCapture::capture_loop() {
#ifdef _WIN32
    (void)this;
#else
    LOG_WARN("CameraCapture", "Camera capture not supported on this platform");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
}

std::vector<unsigned char> CameraCapture::capture_frame(int& out_width, int& out_height) {
    std::lock_guard<std::mutex> lock(mutex_);
    out_width = latest_width_;
    out_height = latest_height_;
    if (latest_frame_.empty() || latest_width_ <= 0 || latest_height_ <= 0) {
        return {};
    }
    return latest_frame_;
}

double CameraCapture::source_frame_rate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return source_frame_rate_;
}

// ---------------------------------------------------------------------------
// Photos, videos and where they are written
// ---------------------------------------------------------------------------

std::string CameraCapture::capture_root_dir() {
#ifdef _WIN32
    char pictures[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_MYPICTURES, nullptr, 0, pictures))) {
        char profile[MAX_PATH] = {0};
        if (FAILED(SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, profile))) {
            return std::string();
        }
        return std::string(profile) + "\\Pictures\\Sonny Captures";
    }
    return std::string(pictures) + "\\Sonny Captures";
#else
    return std::string();
#endif
}

std::string CameraCapture::photos_dir() {
    return capture_root_dir() + "\\Photos";
}

std::string CameraCapture::videos_dir() {
    return capture_root_dir() + "\\Videos";
}

bool CameraCapture::ensure_directory(const std::string& directory, std::string& out_error) {
    if (directory.empty()) {
        out_error = "The capture folder could not be located.";
        return false;
    }
    const std::wstring wide = pathutil::utf8_to_wide(directory);
    std::error_code code;
    std::filesystem::create_directories(wide, code);
    if (code && !std::filesystem::is_directory(wide)) {
        out_error = "Could not create " + directory + " (" + code.message() + ").";
        LOG_ERROR("CameraCapture", out_error);
        return false;
    }
    return true;
}

std::string CameraCapture::make_capture_path(const std::string& directory, const std::string& prefix,
                                             const std::string& extension) {
    const std::string stamp = timestamp_suffix();
    std::string candidate = directory + "\\" + prefix + "_" + stamp + "." + extension;
    // Two photos in the same second must not overwrite each other.
    for (int attempt = 1;
         attempt < 100 && std::filesystem::exists(pathutil::utf8_to_wide(candidate));
         ++attempt) {
        candidate = directory + "\\" + prefix + "_" + stamp + "_" + std::to_string(attempt) +
                    "." + extension;
    }
    return candidate;
}

bool CameraCapture::ensure_frame_ready(int timeout_ms, int& out_width, int& out_height,
                                       std::string& out_error) {
    if (!running_) {
        if (!start()) {
            out_error = "The camera could not be started.";
            return false;
        }
        camera_started_here_ = true;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!latest_frame_.empty() && latest_width_ > 0 && latest_height_ > 0) {
                out_width = latest_width_;
                out_height = latest_height_;
                return true;
            }
        }
        if (!running_) {
            out_error = "The camera stopped before it delivered a frame. Is a webcam connected?";
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            out_error = "The camera did not deliver a frame. Is a webcam connected?";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void CameraCapture::release_camera_if_started_here() {
    if (!camera_started_here_) return;
    camera_started_here_ = false;
    if (is_recording()) return;   // the feed has to stay up for the recording
    stop();
}

void CameraCapture::feed_recorder(const unsigned char* rgb, int width, int height) {
    std::lock_guard<std::mutex> lock(recorder_mutex_);
    if (recorder_ && recorder_->is_recording()) {
        recorder_->add_frame(rgb, width, height);
    }
}

bool CameraCapture::save_photo(const std::string& directory, const std::string& extension,
                               std::string& out_path, std::string& out_error) {
    const bool jpeg = (extension == "jpg" || extension == "jpeg");
    if (!ensure_directory(directory, out_error)) return false;

    int width = 0;
    int height = 0;
    if (!ensure_frame_ready(8000, width, height, out_error)) {
        release_camera_if_started_here();
        return false;
    }
    const std::vector<unsigned char> frame = capture_frame(width, height);
    release_camera_if_started_here();
    if (frame.empty()) {
        out_error = "The camera returned an empty frame.";
        return false;
    }

    const std::string path = make_capture_path(directory, "Sonny_Photo", jpeg ? "jpg" : "png");
    if (!Jarvis::imageutil::save_file(frame.data(), width, height, path,
                                      jpeg ? "jpg" : "png", out_error)) {
        return false;
    }

    out_path = path;
    LOG_INFO("CameraCapture", "Photo saved: " + path);
    return true;
}

bool CameraCapture::save_photo(std::string& out_path, std::string& out_error) {
    return save_photo(photos_dir(), "png", out_path, out_error);
}

bool CameraCapture::start_recording(const std::string& path, std::string& out_error) {
    if (is_recording()) {
        out_error = "A recording is already running.";
        return false;
    }

    int width = 0;
    int height = 0;
    if (!ensure_frame_ready(8000, width, height, out_error)) {
        release_camera_if_started_here();
        return false;
    }

    // Built outside the lock so a failed start cannot tear down a recording that
    // another caller started in the meantime.
    auto recorder = std::make_unique<VideoRecorder>();
    if (!recorder->start(path, width, height, source_frame_rate(), kMaxRecordingSeconds)) {
        out_error = recorder->last_error();
        LOG_ERROR("CameraCapture", "Could not start recording: " + out_error);
        release_camera_if_started_here();
        return false;
    }

    std::lock_guard<std::mutex> lock(recorder_mutex_);
    recorder_ = std::move(recorder);
    return true;
}

bool CameraCapture::stop_recording(std::string& out_path, std::string& out_error) {
    std::unique_ptr<VideoRecorder> finished;
    {
        std::lock_guard<std::mutex> lock(recorder_mutex_);
        if (!recorder_ || !recorder_->is_recording()) {
            out_error = "No recording is running.";
            return false;
        }
        if (!recorder_->stop()) {
            out_error = recorder_->last_error();
        }
        out_path = recorder_->output_path();
        finished = std::move(recorder_);
    }
    // Destroyed outside the lock: the destructor flushes the file.
    finished.reset();
    release_camera_if_started_here();
    return out_error.empty();
}

bool CameraCapture::is_recording() const {
    std::lock_guard<std::mutex> lock(recorder_mutex_);
    return recorder_ && recorder_->is_recording();
}

std::string CameraCapture::recording_path() const {
    std::lock_guard<std::mutex> lock(recorder_mutex_);
    if (!recorder_ || !recorder_->is_recording()) return std::string();
    return recorder_->output_path();
}

double CameraCapture::recording_seconds() const {
    std::lock_guard<std::mutex> lock(recorder_mutex_);
    if (!recorder_) return 0.0;
    return recorder_->recorded_seconds();
}

} // namespace Jarvis
