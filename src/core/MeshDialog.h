#pragma once

#include <windows.h>
#include <string>
#include <functional>
#include <vector>

namespace Jarvis {
namespace Vectors {
struct MeshPeerView;
}
}

class MeshDialog {
public:
    using MeshProvider = std::function<std::string()>;

    MeshDialog() = default;
    ~MeshDialog() = default;

    MeshDialog(const MeshDialog&) = delete;
    MeshDialog& operator=(const MeshDialog&) = delete;

    static INT_PTR show_dialog(HWND hwnd, MeshProvider mesh_provider);
    static bool is_webview2_available();

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static INT_PTR run_dialog(HWND parent, MeshProvider provider);
};