// MeshDialog.cpp - WebView2-based mesh topology visualization
//
// Loads mesh.html from the resources/web directory and exposes a REST
// endpoint (/api/mesh) that returns the aggregated mesh view from
// VectorsSync. The HTML page uses vis-network to render the topology
// as an interactive graph.
//
// WebView2 is loaded dynamically at runtime via LoadLibrary, so the
// dependency is optional: if the runtime is missing the dialog falls
// back to a static message.

#include "MeshDialog.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <combaseapi.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <webview2.h>
#include <WebView2EnvironmentOptions.h>

using Microsoft::WRL::ComPtr;

namespace {

// Title bar text of the mesh window.
constexpr const wchar_t* kWindowTitle = L"Sonny";

// Returns the path to the mesh.html file in the resources/web directory
std::string get_mesh_html_path() {
    char module_dir[MAX_PATH] = {0};
    if (GetModuleFileNameA(nullptr, module_dir, MAX_PATH) == 0) {
        return "web\\mesh.html";
    }
    std::string path(module_dir);
    size_t last_slash = path.find_last_of("\\/");
    if (last_slash != std::string::npos) {
        path = path.substr(0, last_slash);
    }
    return path + "\\web\\mesh.html";
}

}  // namespace

struct MeshDialog::Impl {
    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    MeshProvider mesh_provider_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webview_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> navigation_completed_{false};

    Impl(HWND parent, MeshProvider provider) : parent_(parent), mesh_provider_(std::move(provider)) {}
    ~Impl() = default;

    void initialize_webview2();
    void on_webview_created(ICoreWebView2Controller* controller);
    void on_navigation_completed(ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args);
    void inject_mesh_data();
    void setup_virtual_host_mapping();
    void resize_webview();
    static LRESULT CALLBACK static_window_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, Impl* self);
    LRESULT handle_message(UINT message, WPARAM wParam, LPARAM lParam);
};

void MeshDialog::Impl::initialize_webview2() {
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, options.Get(),
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    return result;
                }
                env->CreateCoreWebView2Controller(hwnd_,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || !controller) {
                                return result;
                            }
                            on_webview_created(controller);
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

void MeshDialog::Impl::on_webview_created(ICoreWebView2Controller* controller) {
    controller_ = controller;
    controller_->get_CoreWebView2(&webview_);

    // Configure WebView2 settings
    ComPtr<ICoreWebView2Settings> settings;
    webview_->get_Settings(&settings);
    settings->put_IsScriptEnabled(TRUE);
    settings->put_AreDefaultScriptDialogsEnabled(TRUE);
    settings->put_IsWebMessageEnabled(TRUE);

    // Add host object for /api/mesh endpoint
    setup_virtual_host_mapping();

    // Handle navigation completion
    EventRegistrationToken token;
    webview_->add_NavigationCompleted(
        Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [this](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                on_navigation_completed(sender, args);
                return S_OK;
            }).Get(), &token);

    // Navigate via virtual host so local assets load from http:// origin
    webview_->Navigate(L"http://sonny-mesh.local/mesh.html");

    initialized_ = true;
    resize_webview();
}

void MeshDialog::Impl::on_navigation_completed(ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) {
    BOOL success = FALSE;
    args->get_IsSuccess(&success);
    if (success) {
        navigation_completed_ = true;
        inject_mesh_data();
    }
}

void MeshDialog::Impl::setup_virtual_host_mapping() {
    // Map "sonny-mesh.local" to the web/ directory so mesh.html and its
    // local assets (vis-network.min.js) are served from an http:// origin,
    // avoiding file:// security restrictions in WebView2.
    ComPtr<ICoreWebView2_3> webview2;
    webview_.As(&webview2);
    if (webview2) {
        std::string html_path = get_mesh_html_path();
        size_t last_sep = html_path.find_last_of("\\/");
        std::wstring web_dir;
        if (last_sep != std::string::npos) {
            std::string dir = html_path.substr(0, last_sep + 1);
            web_dir = std::wstring(dir.begin(), dir.end());
        } else {
            web_dir = L"web\\";
        }
        webview2->SetVirtualHostNameToFolderMapping(
            L"sonny-mesh.local",
            web_dir.c_str(),
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    }
}

void MeshDialog::Impl::inject_mesh_data() {
    if (!webview_) return;

    std::string json = mesh_provider_();
    if (json.empty()) json = "[]";

    // Inject mesh data into the page via JavaScript
    std::string script = "window.meshData = " + json + "; if (typeof renderMesh === 'function') renderMesh(window.meshData);";
    std::wstring wide_script(script.begin(), script.end());
    webview_->ExecuteScript(wide_script.c_str(), nullptr);
}

void MeshDialog::Impl::resize_webview() {
    if (!controller_) return;
    RECT bounds;
    GetClientRect(hwnd_, &bounds);
    controller_->put_Bounds(bounds);
}

LRESULT MeshDialog::Impl::handle_message(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_SIZE:
            resize_webview();
            break;
        case WM_DESTROY:
            if (controller_) {
                controller_->Close();
                controller_ = nullptr;
            }
            webview_ = nullptr;
            PostQuitMessage(0);
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            break;
    }
    return DefWindowProc(hwnd_, message, wParam, lParam);
}

LRESULT CALLBACK MeshDialog::Impl::static_window_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, Impl* self) {
    if (self) {
        return self->handle_message(message, wParam, lParam);
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK MeshDialog::window_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        Impl* self = static_cast<Impl*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (self) {
        return self->static_window_proc(hwnd, message, wParam, lParam, self);
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

bool MeshDialog::is_webview2_available() {
    HMODULE hModule = LoadLibraryA("WebView2Loader.dll");
    if (hModule) {
        FreeLibrary(hModule);
        return true;
    }
    return false;
}

INT_PTR MeshDialog::run_dialog(HWND parent, MeshProvider provider) {
    if (!is_webview2_available()) {
        MessageBox(nullptr,
            "WebView2 runtime is not installed. Please install it from:\n"
            "https://go.microsoft.com/fwlink/p/?LinkId=2124703",
            "Sonny Mesh - WebView2 Required", MB_OK | MB_ICONWARNING);
        return 0;
    }

    // Initialize COM for this thread
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        MessageBox(nullptr, "Failed to initialize COM", "Error", MB_OK | MB_ICONERROR);
        return 0;
    }

    // Register window class
    WNDCLASSW wc = {};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"SonnyMeshDialogClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    // Create the dialog window
    auto impl = std::make_unique<Impl>(parent, std::move(provider));
    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW | WS_EX_DLGMODALFRAME,
        L"SonnyMeshDialogClass",
        kWindowTitle,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700,
        parent, nullptr, GetModuleHandle(nullptr), impl.get());

    if (!hwnd) {
        CoUninitialize();
        return 0;
    }

    // Transfer ownership to the window
    impl.release();

    // Initialize WebView2
    Impl* impl_ptr = reinterpret_cast<Impl*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (impl_ptr) {
        impl_ptr->initialize_webview2();
    }

    // Re-assert the caption once the controller is attached, so the title bar
    // shows the product name rather than whatever the host window was left with.
    SetWindowTextW(hwnd, kWindowTitle);

    // Message loop
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CoUninitialize();
    return 0;
}

INT_PTR MeshDialog::show_dialog(HWND hwnd, MeshProvider mesh_provider) {
    return run_dialog(hwnd, std::move(mesh_provider));
}