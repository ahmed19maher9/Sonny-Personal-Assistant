#include "AvatarOverlay.h"
#include "Logger.h"
#include <iostream>
#include <fstream>
#include <dwmapi.h>
#include <d3dcompiler.h>
#include <vector>
#include <algorithm>
#include <cmath>

// Global texture resources (since we cannot modify the header)
static ID3D11ShaderResourceView* g_avatarTexture = nullptr;
static ID3D11SamplerState* g_avatarSampler = nullptr;

// Include nlohmann/json before tiny_gltf
#include "../resources/avatars/json.hpp"

// Define TINYGLTF_IMPLEMENTATION to include implementation
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_INCLUDE_JSON  // Use external nlohmann/json
#define TINYGLTF_NO_STB_IMAGE     // Prevent duplicate stb_image symbols with mtmd.lib
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include "../resources/avatars/tiny_gltf.h"

// Include stb_image separately with STB_IMAGE_STATIC so all STBIDEF functions
// are declared 'static' (internal linkage), preventing duplicate-symbol errors
// with mtmd.lib(mtmd-helper.obj) which also bundles stb_image.
// TINYGLTF_NO_STB_IMAGE above prevents tiny_gltf from pulling in its own copy.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../resources/avatars/stb_image.h"
#undef STB_IMAGE_IMPLEMENTATION
#undef STB_IMAGE_STATIC

// Image decoder handed to tinygltf through TinyGLTF::SetImageLoader().
//
// TINYGLTF_NO_STB_IMAGE stops tinygltf from compiling its own stb_image copy
// (mtmd.lib already bundles one), but it also removes tinygltf's default
// LoadImageData() implementation. Without a replacement, any .glb that stores
// its textures in a bufferView (i.e. every embedded-texture glb, like
// sonny.glb / iron-man.glb) fails to parse with:
//   "No LoadImageData callback specified."
// Here the compressed PNG/JPEG/... bytes are decoded with the statically
// linked stb_image included above and handed back to tinygltf.
static bool load_image_data_with_stb_image(tinygltf::Image* image, const int image_idx,
                                           std::string* err, std::string* warn,
                                           int req_width, int req_height,
                                           const unsigned char* bytes, int size,
                                           void* user_data) {
    (void)warn;
    (void)user_data;

    if (!image || !bytes || size <= 0) {
        if (err) {
            *err += "Invalid image data for image[" + std::to_string(image_idx) + "].\n";
        }
        return false;
    }

    // Decode straight to RGBA: the texture upload below only handles
    // DXGI_FORMAT_R8G8B8A8_UNORM (4 bytes per pixel). req_comp = 4 also turns
    // grayscale/2-channel sources into a usable colour texture.
    int width = 0;
    int height = 0;
    int components = 0;
    unsigned char* pixels = stbi_load_from_memory(bytes, size, &width, &height, &components, 4);
    if (!pixels) {
        const char* reason = stbi_failure_reason();
        if (err) {
            *err += "Failed to decode image[" + std::to_string(image_idx) + "] name = \"" +
                    image->name + "\" (" + (reason ? reason : "unknown stb_image error") + ").\n";
        }
        return false;
    }

    if (width < 1 || height < 1) {
        stbi_image_free(pixels);
        if (err) {
            *err += "Invalid image dimensions (" + std::to_string(width) + "x" +
                    std::to_string(height) + ") for image[" + std::to_string(image_idx) + "].\n";
        }
        return false;
    }

    // tinygltf passes the dimensions from the glTF JSON when it has them; a
    // mismatch means the asset and the payload disagree, so treat it as an
    // error like tinygltf's own loader does.
    if ((req_width > 0 && req_width != width) || (req_height > 0 && req_height != height)) {
        stbi_image_free(pixels);
        if (err) {
            *err += "Image size mismatch for image[" + std::to_string(image_idx) + "]: expected " +
                    std::to_string(req_width) + "x" + std::to_string(req_height) + ", decoded " +
                    std::to_string(width) + "x" + std::to_string(height) + ".\n";
        }
        return false;
    }

    image->width = width;
    image->height = height;
    image->component = 4;
    image->bits = 8;
    image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image->as_is = false;
    image->image.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);

    stbi_image_free(pixels);
    return true;
}

// Vertex shader as a compiled string (vs_5_0)
static const char* vertex_shader_code = R"(
cbuffer ConstantBuffer : register(b0) {
    float4x4 world;
    float4x4 view;
    float4x4 projection;
    float4 lightDir;
    float4 cameraPos;
    float4x4 boneMatrices[64];
};

struct VS_INPUT {
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 tex : TEXCOORD;
    uint4 boneIndices : BLENDINDICES;
    float4 boneWeights : BLENDWEIGHTS;
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float3 normal : NORMAL;
    float2 tex : TEXCOORD;
    float3 viewDir : TEXCOORD1;
    float3 lightDir : TEXCOORD2;
};

PS_INPUT main(VS_INPUT input) {
    PS_INPUT output;
    
    // Skinning
    float4 skinnedPos = float4(0, 0, 0, 0);
    float4 skinnedNormal = float4(0, 0, 0, 0);
    
    for(int i = 0; i < 4; ++i) {
        if(input.boneWeights[i] > 0.0) {
            float4x4 boneMat = boneMatrices[input.boneIndices[i]];
            skinnedPos += mul(float4(input.pos, 1.0f), boneMat) * input.boneWeights[i];
            skinnedNormal += mul(float4(input.normal, 0.0f), boneMat) * input.boneWeights[i];
        }
    }
    
    // Apply world transform (includes rotation)
    float4 worldPos = mul(skinnedPos, world);
    float4 worldNormal = mul(skinnedNormal, world);

    output.pos = mul(mul(worldPos, view), projection);
    output.normal = normalize(worldNormal.xyz);
    output.tex = input.tex;
    output.viewDir = normalize(cameraPos.xyz - worldPos.xyz);
    output.lightDir = normalize(lightDir.xyz);
    return output;
}
)";

// Pixel shader
static const char* pixel_shader_code = R"(
Texture2D txDiffuse : register(t0);
SamplerState samLinear : register(s0);

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float3 normal : NORMAL;
    float2 tex : TEXCOORD;
    float3 viewDir : TEXCOORD1;
    float3 lightDir : TEXCOORD2;
};

float4 main(PS_INPUT input) : SV_TARGET {
    float3 n = normalize(input.normal);
    float3 v = normalize(input.viewDir);
    float3 l = normalize(input.lightDir);
    
    float4 texColor = txDiffuse.Sample(samLinear, input.tex);
    
    // Ambient - Reduced slightly to dial back the over-whiteness
    float3 ambient = float3(0.95f, 0.95f, 0.95f) * texColor.rgb;
    
    // Diffuse
    float diff = max(dot(n, l), 0.0f);
    float3 diffuse = diff * float3(0.7f, 0.7f, 0.7f) * texColor.rgb;
    
    // Specular (Blinn-Phong) for glossiness
    // Using a high power (128.0) makes the highlights sharp and focused, creating a glossy finish
    float3 h = normalize(l + v);
    float spec = pow(max(dot(n, h), 0.0f), 128.0f);
    float3 specular = spec * float3(0.5f, 0.5f, 0.5f);
    
    float3 result = ambient + diffuse + specular;
    
    return float4(result, texColor.a);
}
)";

AvatarOverlay::AvatarOverlay() {}
AvatarOverlay::~AvatarOverlay() { stop(); }

bool AvatarOverlay::initialize(HINSTANCE hInstance, const std::string& glb_path) {
    hinstance_ = hInstance;

    // Find taskbar position
    int tb_x = 0, tb_y = 0, tb_w = 0, tb_h = 0;
    find_taskbar_position(tb_x, tb_y, tb_w, tb_h);

    // Size the overlay (e.g., 200x300 standing above taskbar right side)
    window_width_ = 220;
    window_height_ = 340;
    window_x_ = tb_x + tb_w - window_width_ - 10;
    window_y_ = tb_y - window_height_;

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"AvatarOverlayClass";
    
    if (!RegisterClassExW(&wc)) {
        LOG_ERROR("AVATAR", "Failed to register window class");
        return false;
    }

    // Create layered window (transparent)
    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"AvatarOverlayClass",
        L"Avatar",
        WS_POPUP,
        window_x_, window_y_, window_width_, window_height_,
        nullptr, nullptr, hInstance, this
    );

    if (!hwnd_) {
        LOG_ERROR("AVATAR", "Failed to create window");
        return false;
    }

    // Enable true per-pixel alpha transparency using DWM composition. 
    // Setting MARGINS to -1 creates a "sheet of glass" window that respects our D3D alpha channel.
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(hwnd_, &margins);

    // Ensure the window uses our D3D alpha channel for transparency and remains click-through
    SetWindowLong(hwnd_, GWL_EXSTYLE, GetWindowLong(hwnd_, GWL_EXSTYLE) | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW);
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);

    // Create D3D device
    if (!create_device()) {
        LOG_ERROR("AVATAR", "Failed to create D3D device");
        return false;
    }

    // Create shaders
    if (!create_shaders()) {
        LOG_ERROR("AVATAR", "Failed to create shaders");
        return false;
    }

    // Create depth stencil
    if (!create_depth_stencil()) {
        LOG_ERROR("AVATAR", "Failed to create depth stencil");
        return false;
    }

    // Load model
    if (!glb_path.empty()) {
        if (!load_model(glb_path)) {
            LOG_ERROR("AVATAR", "Failed to load model: " + glb_path);
            return false;
        }
    }

    // Create vertex/index buffers
    if (!create_buffers()) {
        LOG_ERROR("AVATAR", "Failed to create buffers");
        return false;
    }

    LOG_DEBUG_COMPONENT("AVATAR", "Overlay initialized successfully");
    return true;
}

void AvatarOverlay::show() {
    if (!hwnd_) return;
    visible_ = true;
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
}

void AvatarOverlay::hide() {
    if (!hwnd_) return;
    visible_ = false;
    ShowWindow(hwnd_, SW_HIDE);
}

bool AvatarOverlay::start() {
    if (running_) return true;
    running_ = true;
    render_thread_ = std::make_unique<std::thread>(&AvatarOverlay::render_loop, this);
    return true;
}

void AvatarOverlay::stop() {
    running_ = false;
    visible_ = false;
    
    if (render_thread_ && render_thread_->joinable()) {
        render_thread_->join();
    }
    render_thread_.reset();

    // Cleanup textures
    if (g_avatarTexture) {
        g_avatarTexture->Release();
        g_avatarTexture = nullptr;
    }
    if (g_avatarSampler) {
        g_avatarSampler->Release();
        g_avatarSampler = nullptr;
    }

    // Cleanup D3D
    if (blend_state_) blend_state_->Release();
    if (depth_stencil_state_) depth_stencil_state_->Release();
    if (rasterizer_state_) rasterizer_state_->Release();
    if (constant_buffer_) constant_buffer_->Release();
    if (index_buffer_) index_buffer_->Release();
    if (vertex_buffer_) vertex_buffer_->Release();
    if (input_layout_) input_layout_->Release();
    if (pixel_shader_) pixel_shader_->Release();
    if (vertex_shader_) vertex_shader_->Release();
    if (depth_stencil_view_) depth_stencil_view_->Release();
    if (depth_stencil_buffer_) depth_stencil_buffer_->Release();
    if (render_target_view_) render_target_view_->Release();
    if (swap_chain_) swap_chain_->Release();
    if (d3d_context_) d3d_context_->Release();
    if (d3d_device_) d3d_device_->Release();
    
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool AvatarOverlay::load_model(const std::string& glb_path) {
    current_model_path_ = glb_path;
    return load_glb_file(glb_path);
}

void AvatarOverlay::set_desktop_position(int x, int y, int width, int height) {
    window_x_ = x;
    window_y_ = y;
    window_width_ = width;
    window_height_ = height;
    if (hwnd_) {
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    }
}

//=============================================================================
// PRIVATE METHODS
//=============================================================================

LRESULT CALLBACK AvatarOverlay::wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    AvatarOverlay* overlay = nullptr;
    if (msg == WM_CREATE) {
        CREATESTRUCT* cs = (CREATESTRUCT*)lparam;
        overlay = (AvatarOverlay*)cs->lpCreateParams;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)overlay);
    } else {
        overlay = (AvatarOverlay*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    }
    
    if (overlay) {
        return overlay->handle_message(msg, wparam, lparam);
    }
    return DefWindowProc(hwnd, msg, wparam, lparam);
}

LRESULT AvatarOverlay::handle_message(UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_DESTROY:
            running_ = false;
            return 0;
        case WM_SIZE:
            window_width_ = LOWORD(lparam);
            window_height_ = HIWORD(lparam);
            if (render_target_view_) {
                render_target_view_->Release();
                render_target_view_ = nullptr;
                swap_chain_->ResizeBuffers(0, window_width_, window_height_, DXGI_FORMAT_UNKNOWN, 0);
                create_render_target();
                create_depth_stencil();
            }
            return 0;
        case WM_ERASEBKGND:
            return 1; // Prevent flicker
    }
    return DefWindowProc(hwnd_, msg, wparam, lparam);
}

bool AvatarOverlay::create_device() {
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferDesc.Width = window_width_;
    scd.BufferDesc.Height = window_height_;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd_;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.Windowed = TRUE;
    scd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    scd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    D3D_FEATURE_LEVEL feature_levels[] = { D3D_FEATURE_LEVEL_11_0 };
    
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        feature_levels, 1, D3D11_SDK_VERSION,
        &scd, &swap_chain_, &d3d_device_, nullptr, &d3d_context_
    );

    if (FAILED(hr)) {
        LOG_ERROR("AVATAR", "D3D11CreateDeviceAndSwapChain failed: 0x" + std::to_string(hr));
        return false;
    }

    return create_render_target();
}

bool AvatarOverlay::create_render_target() {
    ID3D11Texture2D* back_buffer = nullptr;
    HRESULT hr = swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back_buffer);
    if (FAILED(hr)) return false;

    hr = d3d_device_->CreateRenderTargetView(back_buffer, nullptr, &render_target_view_);
    back_buffer->Release();
    
    if (FAILED(hr)) return false;

    // Set viewport
    D3D11_VIEWPORT viewport;
    viewport.TopLeftX = 0;
    viewport.TopLeftY = 0;
    viewport.Width = (float)window_width_;
    viewport.Height = (float)window_height_;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    d3d_context_->RSSetViewports(1, &viewport);

    return true;
}

bool AvatarOverlay::create_depth_stencil() {
    if (depth_stencil_view_) {
        depth_stencil_view_->Release();
        depth_stencil_view_ = nullptr;
    }
    if (depth_stencil_buffer_) {
        depth_stencil_buffer_->Release();
        depth_stencil_buffer_ = nullptr;
    }

    D3D11_TEXTURE2D_DESC depth_desc = {};
    depth_desc.Width = window_width_;
    depth_desc.Height = window_height_;
    depth_desc.MipLevels = 1;
    depth_desc.ArraySize = 1;
    depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.SampleDesc.Quality = 0;
    depth_desc.Usage = D3D11_USAGE_DEFAULT;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    HRESULT hr = d3d_device_->CreateTexture2D(&depth_desc, nullptr, &depth_stencil_buffer_);
    if (FAILED(hr)) return false;

    hr = d3d_device_->CreateDepthStencilView(depth_stencil_buffer_, nullptr, &depth_stencil_view_);
    return SUCCEEDED(hr);
}

bool AvatarOverlay::create_shaders() {
    // Compile vertex shader
    ID3DBlob* vs_blob = nullptr;
    ID3DBlob* error_blob = nullptr;
    
    HRESULT hr = D3DCompile(vertex_shader_code, strlen(vertex_shader_code), 
        nullptr, nullptr, nullptr, "main", "vs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs_blob, &error_blob);
    
    if (FAILED(hr)) {
        if (error_blob) {
            LOG_ERROR("AVATAR", "VS compile error: " + std::string((const char*)error_blob->GetBufferPointer()));
            error_blob->Release();
        }
        return false;
    }

    hr = d3d_device_->CreateVertexShader(vs_blob->GetBufferPointer(), 
        vs_blob->GetBufferSize(), nullptr, &vertex_shader_);
    if (FAILED(hr)) {
        vs_blob->Release();
        return false;
    }

    // Input layout
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHTS", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };

    hr = d3d_device_->CreateInputLayout(layout, 5, vs_blob->GetBufferPointer(),
        vs_blob->GetBufferSize(), &input_layout_);
    vs_blob->Release();
    if (FAILED(hr)) return false;

    // Compile pixel shader
    ID3DBlob* ps_blob = nullptr;
    hr = D3DCompile(pixel_shader_code, strlen(pixel_shader_code),
        nullptr, nullptr, nullptr, "main", "ps_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps_blob, &error_blob);

    if (FAILED(hr)) {
        if (error_blob) {
            LOG_ERROR("AVATAR", "PS compile error: " + std::string((const char*)error_blob->GetBufferPointer()));
            error_blob->Release();
        }
        return false;
    }

    hr = d3d_device_->CreatePixelShader(ps_blob->GetBufferPointer(),
        ps_blob->GetBufferSize(), nullptr, &pixel_shader_);
    ps_blob->Release();
    if (FAILED(hr)) return false;

    // Create constant buffer
    D3D11_BUFFER_DESC cb_desc = {};
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cb_desc.ByteWidth = sizeof(ConstantBufferData);

    hr = d3d_device_->CreateBuffer(&cb_desc, nullptr, &constant_buffer_);
    if (FAILED(hr)) return false;

    // Rasterizer state
    D3D11_RASTERIZER_DESC rs_desc = {};
    rs_desc.FillMode = D3D11_FILL_SOLID;
    rs_desc.CullMode = D3D11_CULL_NONE;
    rs_desc.FrontCounterClockwise = FALSE;
    rs_desc.DepthClipEnable = TRUE;
    rs_desc.ScissorEnable = FALSE;
    rs_desc.MultisampleEnable = FALSE;
    rs_desc.AntialiasedLineEnable = FALSE;
    hr = d3d_device_->CreateRasterizerState(&rs_desc, &rasterizer_state_);
    if (FAILED(hr)) return false;

    // Blend state (transparency)
    D3D11_BLEND_DESC blend_desc = {};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = d3d_device_->CreateBlendState(&blend_desc, &blend_state_);
    if (FAILED(hr)) return false;

    // Depth stencil state
    D3D11_DEPTH_STENCIL_DESC ds_desc = {};
    ds_desc.DepthEnable = TRUE;
    ds_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds_desc.DepthFunc = D3D11_COMPARISON_LESS;
    ds_desc.StencilEnable = FALSE;
    hr = d3d_device_->CreateDepthStencilState(&ds_desc, &depth_stencil_state_);
    if (FAILED(hr)) return false;

    // Create sampler state
    D3D11_SAMPLER_DESC samp_desc = {};
    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samp_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samp_desc.MinLOD = 0;
    samp_desc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = d3d_device_->CreateSamplerState(&samp_desc, &g_avatarSampler);
    if (FAILED(hr)) {
        return false;
    }

    return true;
}

bool AvatarOverlay::create_buffers() {
    // Count total vertices and indices
    size_t total_verts = 0;
    size_t total_indices = 0;
    for (const auto& mesh : meshes_) {
        total_verts += mesh.vertices.size();
        total_indices += mesh.indices.size();
    }

    if (total_verts == 0 || total_indices == 0) return true; // No mesh data yet

    // Create combined vertex buffer
    std::vector<SimpleVertex> all_vertices;
    all_vertices.reserve(total_verts);
    
    std::vector<uint32_t> all_indices;
    all_indices.reserve(total_indices);
    
    uint32_t base_vertex = 0;
    for (const auto& mesh : meshes_) {
        for (const auto& v : mesh.vertices) {
            all_vertices.push_back(v);
        }
        for (auto idx : mesh.indices) {
            all_indices.push_back(idx + base_vertex);
        }
        base_vertex += (uint32_t)mesh.vertices.size();
    }

    // Create vertex buffer
    D3D11_BUFFER_DESC vb_desc = {};
    vb_desc.Usage = D3D11_USAGE_DEFAULT;
    vb_desc.ByteWidth = (UINT)(all_vertices.size() * sizeof(SimpleVertex));
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA init_data = {};
    init_data.pSysMem = all_vertices.data();

    HRESULT hr = d3d_device_->CreateBuffer(&vb_desc, &init_data, &vertex_buffer_);
    if (FAILED(hr)) return false;

    // Create index buffer
    D3D11_BUFFER_DESC ib_desc = {};
    ib_desc.Usage = D3D11_USAGE_DEFAULT;
    ib_desc.ByteWidth = (UINT)(all_indices.size() * sizeof(uint32_t));
    ib_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;

    init_data.pSysMem = all_indices.data();
    hr = d3d_device_->CreateBuffer(&ib_desc, &init_data, &index_buffer_);
    if (FAILED(hr)) return false;

    return true;
}

void AvatarOverlay::build_transform_matrices() {
    using namespace DirectX;

    // Compute bounding sphere of all meshes
    XMFLOAT3 center = { 0,0,0 };
    XMFLOAT3 min_pt = { 0,0,0 };
    float radius = 1.0f;
    if (!meshes_.empty()) {
        XMFLOAT3 max_pt = { -1e9, -1e9, -1e9 };
        min_pt = { 1e9, 1e9, 1e9 };
        for (const auto& mesh : meshes_) {
            for (const auto& v : mesh.vertices) {
                min_pt.x = std::min(min_pt.x, v.position.x);
                min_pt.y = std::min(min_pt.y, v.position.y);
                min_pt.z = std::min(min_pt.z, v.position.z);
                max_pt.x = std::max(max_pt.x, v.position.x);
                max_pt.y = std::max(max_pt.y, v.position.y);
                max_pt.z = std::max(max_pt.z, v.position.z);
            }
        }
        center.x = (min_pt.x + max_pt.x) * 0.5f;
        center.y = (min_pt.y + max_pt.y) * 0.5f;
        center.z = (min_pt.z + max_pt.z) * 0.5f;
        radius = std::max({
            max_pt.x - min_pt.x,
            max_pt.y - min_pt.y,
            max_pt.z - min_pt.z
        }) * 0.5f;
        if (radius < 0.01f) radius = 1.0f;
    }

    // Calculate camera distance
    float cam_distance = radius * 2.45f;
    
    // Calculate the Y offset needed to make Y=0 appear at the bottom of the view
    // For a camera at (0, 0, -cam_distance) looking at origin, the bottom of view at Z=0 is at Y = -cam_distance * tan(half_fov)
    float half_fov_y = XMConvertToRadians(45.0f * 0.5f);
    float bottom_offset = cam_distance * tanf(half_fov_y);

    // Translate model so feet are at Y=0, then center horizontally and rotate
    model_overall_transform_ = XMMatrixTranslation(-center.x, -min_pt.y, -center.z) * XMMatrixRotationY(XM_PI);

    // Position camera such that Y=0 (feet) appears at bottom of view
    // Raise the camera to make the model appear lower (stand on taskbar)
    XMVECTOR eye = XMVectorSet(0.0f, bottom_offset * 2.0f, -cam_distance, 0.0f);
    XMVECTOR at = XMVectorSet(0.0f, bottom_offset * 2.0f, 0.0f, 0.0f);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMMATRIX view = XMMatrixLookAtLH(eye, at, up);

    // Projection
    float aspect = (float)window_width_ / (float)window_height_;
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(45.0f), aspect, 0.1f, 1000.0f);

    cb_data_.world = XMMatrixTranspose(model_overall_transform_); // Apply model transform to world matrix
    cb_data_.view = XMMatrixTranspose(view);
    cb_data_.projection = XMMatrixTranspose(proj);
    cb_data_.lightDir = XMFLOAT4(0.0f, 1.0f, -1.0f, 0.0f); // Centered light from above-front
    
    XMStoreFloat4(&cb_data_.cameraPos, eye);
}

void AvatarOverlay::render_loop() {
    // Message loop + render
    MSG msg = {};
    auto last_time = std::chrono::high_resolution_clock::now();
    
    static int debug_counter = 0;
    
    while (running_ && visible_) {
        // Process window messages
        while (PeekMessage(&msg, hwnd_, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (running_ && visible_) {
            auto current_time = std::chrono::high_resolution_clock::now();
            float delta_time = std::chrono::duration<float>(current_time - last_time).count();
            last_time = current_time;
            
            // Debug logging disabled to reduce console spam
            // if (debug_counter++ % 60 == 0) {
            //     std::cout << "[AVATAR] Render loop: running=" << running_ << " visible=" << visible_ << " delta=" << delta_time << std::endl;
            // }
            
            // Update animation
            update_animation(delta_time);
            
            render_frame();
            
            // No continuous rotation - avatar faces forward toward the screen
        }

        // Sleep a bit to reduce CPU usage
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

void AvatarOverlay::render_frame() {
    if (!d3d_context_ || !render_target_view_ || !depth_stencil_view_) return;

    // Clear to transparent black
    float clear_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    d3d_context_->ClearRenderTargetView(render_target_view_, clear_color);
    d3d_context_->ClearDepthStencilView(depth_stencil_view_, D3D11_CLEAR_DEPTH, 1.0f, 0);

    // Set render target
    d3d_context_->OMSetRenderTargets(1, &render_target_view_, depth_stencil_view_);

    // Set states
    d3d_context_->RSSetState(rasterizer_state_);
    float blend_factor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    d3d_context_->OMSetBlendState(blend_state_, blend_factor, 0xffffffff);
    d3d_context_->OMSetDepthStencilState(depth_stencil_state_, 0);

    // Set shaders and input layout
    d3d_context_->VSSetShader(vertex_shader_, nullptr, 0);
    d3d_context_->PSSetShader(pixel_shader_, nullptr, 0);
    d3d_context_->IASetInputLayout(input_layout_);
    d3d_context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Set texture and sampler
    if (g_avatarTexture) {
        d3d_context_->PSSetShaderResources(0, 1, &g_avatarTexture);
    }
    if (g_avatarSampler) {
        d3d_context_->PSSetSamplers(0, 1, &g_avatarSampler);
    }

    // Update constant buffer
    build_transform_matrices();
    
    // Copy bone matrices to constant buffer
    for (size_t i = 0; i < bone_matrices_.size() && i < 64; i++) {
        cb_data_.boneMatrices[i] = bone_matrices_[i];
    }
    
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(d3d_context_->Map(constant_buffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &cb_data_, sizeof(cb_data_));
        d3d_context_->Unmap(constant_buffer_, 0);
    }
    d3d_context_->VSSetConstantBuffers(0, 1, &constant_buffer_);

    // Set vertex and index buffers
    UINT stride = sizeof(SimpleVertex);
    UINT offset = 0;
    d3d_context_->IASetVertexBuffers(0, 1, &vertex_buffer_, &stride, &offset);
    d3d_context_->IASetIndexBuffer(index_buffer_, DXGI_FORMAT_R32_UINT, 0);

    // Draw all indices
    size_t total_indices = 0;
    for (const auto& mesh : meshes_) {
        total_indices += mesh.indices.size();
    }
    if (total_indices > 0) {
        d3d_context_->DrawIndexed((UINT)total_indices, 0, 0);
    }

    // Present
    swap_chain_->Present(1, 0);
}

bool AvatarOverlay::load_glb_file(const std::string& path) {
    using namespace tinygltf;

    TinyGLTF loader;
    Model model;
    std::string err, warn;

    // Embedded textures (image.bufferView) can only be decoded through an image
    // loader; see load_image_data_with_stb_image() above.
    loader.SetImageLoader(&load_image_data_with_stb_image, nullptr);

    // Load GLB binary file
    bool ret = loader.LoadBinaryFromFile(&model, &err, &warn, path);
    if (!warn.empty()) {
        LOG_DEBUG_COMPONENT("AVATAR", "GLTF warning: " + warn);
    }
    if (!err.empty()) {
        LOG_ERROR("AVATAR", "GLTF error: " + err);
    }
    if (!ret) {
        return false;
    }

    // Create texture if model has any
    int base_color_tex_index = 0; // Default to first

    // Look for the actual Base Color texture defined in the model materials
    if (!model.materials.empty()) {
        int material_index = model.materials[0].pbrMetallicRoughness.baseColorTexture.index;
        if (material_index >= 0) {
            base_color_tex_index = material_index;
        }
    }

    if (base_color_tex_index >= 0 && base_color_tex_index < (int)model.textures.size() && !model.images.empty()) {
        const auto& gltf_tex = model.textures[base_color_tex_index];
        if (gltf_tex.source >= 0 && gltf_tex.source < (int)model.images.size()) {
            const auto& gltf_img = model.images[gltf_tex.source];
            LOG_DEBUG_COMPONENT("AVATAR", "Loading texture: " + std::to_string(gltf_img.width) + "x" + std::to_string(gltf_img.height) + " components: " + std::to_string(gltf_img.component) + " size: " + std::to_string(gltf_img.image.size()));
            
            if (!gltf_img.image.empty() && gltf_img.component >= 3) {
                std::vector<unsigned char> rgba_data;
                const unsigned char* pixel_source = gltf_img.image.data();

                // If the texture is RGB (3 components), we must pad it to RGBA (4 components)
                // because DXGI_FORMAT_R8G8B8A8_UNORM requires 4 bytes per pixel.
                if (gltf_img.component == 3) {
                    rgba_data.resize(gltf_img.width * gltf_img.height * 4);
                    for (size_t i = 0; i < (size_t)gltf_img.width * gltf_img.height; ++i) {
                        rgba_data[i * 4 + 0] = gltf_img.image[i * 3 + 0];
                        rgba_data[i * 4 + 1] = gltf_img.image[i * 3 + 1];
                        rgba_data[i * 4 + 2] = gltf_img.image[i * 3 + 2];
                        rgba_data[i * 4 + 3] = 255; // Full opacity
                    }
                    pixel_source = rgba_data.data();
                    LOG_DEBUG_COMPONENT("AVATAR", "Converted RGB to RGBA");
                }

                D3D11_TEXTURE2D_DESC desc = {};
                desc.Width = gltf_img.width;
                desc.Height = gltf_img.height;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_IMMUTABLE;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

                D3D11_SUBRESOURCE_DATA initData = {};
                initData.pSysMem = pixel_source;
                initData.SysMemPitch = gltf_img.width * 4;

                ID3D11Texture2D* pTex = nullptr;
                HRESULT hr = d3d_device_->CreateTexture2D(&desc, &initData, &pTex);
                if (SUCCEEDED(hr)) {
                    if (g_avatarTexture) g_avatarTexture->Release();
                    hr = d3d_device_->CreateShaderResourceView(pTex, nullptr, &g_avatarTexture);
                    pTex->Release();
                    if (SUCCEEDED(hr)) {
                        LOG_DEBUG_COMPONENT("AVATAR", "Texture created successfully");
                    } else {
                        LOG_ERROR("AVATAR", "Failed to create SRV: 0x" + std::to_string(hr));
                    }
                } else {
                    LOG_ERROR("AVATAR", "Failed to create Texture2D: 0x" + std::to_string(hr));
                }
            } else {
                LOG_ERROR("AVATAR", "Image data empty or invalid component count");
            }
        } else {
            LOG_ERROR("AVATAR", "Invalid texture source index");
        }
    } else {
        LOG_DEBUG_COMPONENT("AVATAR", "No textures or images in model");
    }

    meshes_.clear();
    animations_.clear();
    skins_.clear();
    scene_nodes_.clear();
    original_meshes_.clear();
    has_animation_ = false;

    LOG_DEBUG_COMPONENT("AVATAR", "Starting to load scene nodes...");

    // Load scene nodes
    for (const auto& gltf_node : model.nodes) {
        SceneNode node;
        node.index = (int)scene_nodes_.size();
        
        // Initialize with default values
        node.translation = {0.0f, 0.0f, 0.0f};
        node.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
        node.scale = {1.0f, 1.0f, 1.0f};
        node.mesh_index = gltf_node.mesh;
        
        // Safely load translation if available
        if (gltf_node.translation.size() >= 3) {
            node.translation = {(float)gltf_node.translation[0], (float)gltf_node.translation[1], (float)gltf_node.translation[2]};
        }
        
        // Safely load rotation if available
        if (gltf_node.rotation.size() >= 4) {
            node.rotation = {(float)gltf_node.rotation[0], (float)gltf_node.rotation[1], (float)gltf_node.rotation[2], (float)gltf_node.rotation[3]};
        }
        
        // Safely load scale if available
        if (gltf_node.scale.size() >= 3) {
            node.scale = {(float)gltf_node.scale[0], (float)gltf_node.scale[1], (float)gltf_node.scale[2]};
        }
        
        scene_nodes_.push_back(node);
    }
    
    LOG_DEBUG_COMPONENT("AVATAR", "Loaded " + std::to_string(scene_nodes_.size()) + " scene nodes");

    // Build node hierarchy
    for (size_t i = 0; i < model.nodes.size(); i++) {
        for (int child : model.nodes[i].children) {
            if (child >= 0 && child < (int)scene_nodes_.size()) {
                scene_nodes_[child].parent_index = (int)i;
            }
        }
    }
    
    LOG_DEBUG_COMPONENT("AVATAR", "Built node hierarchy: " + std::to_string(scene_nodes_.size()) + " nodes");

    LOG_DEBUG_COMPONENT("AVATAR", "Starting to load skins...");

    // Load skins
    for (const auto& gltf_skin : model.skins) {
        Skin skin;
        skin.name = gltf_skin.name;
        
        // Load inverse bind matrices
        if (gltf_skin.inverseBindMatrices >= 0 && gltf_skin.inverseBindMatrices < (int)model.accessors.size()) {
            const auto& accessor = model.accessors[gltf_skin.inverseBindMatrices];
            
            if (accessor.bufferView >= 0 && accessor.bufferView < (int)model.bufferViews.size()) {
                const auto& buffer_view = model.bufferViews[accessor.bufferView];
                
                if (buffer_view.buffer >= 0 && buffer_view.buffer < (int)model.buffers.size()) {
                    const auto& buffer = model.buffers[buffer_view.buffer];
                    
                    size_t data_offset = buffer_view.byteOffset + accessor.byteOffset;
                    if (data_offset < buffer.data.size()) {
                        const float* matrix_data = (const float*)(buffer.data.data() + data_offset);
                        
                        for (size_t i = 0; i < accessor.count; i++) {
                            DirectX::XMFLOAT4X4 mat;
                            for (int j = 0; j < 16; j++) {
                                ((float*)&mat)[j] = matrix_data[i * 16 + j];
                            }
                            skin.inverse_bind_matrices.push_back(mat);
                        }
                    } else {
                        LOG_DEBUG_COMPONENT("AVATAR", "Invalid data offset for inverse bind matrices");
                    }
                } else {
                    LOG_DEBUG_COMPONENT("AVATAR", "Invalid buffer index for inverse bind matrices");
                }
            } else {
                LOG_DEBUG_COMPONENT("AVATAR", "Invalid bufferView index for inverse bind matrices");
            }
        }
        
        // Load joint indices
        for (int joint_index : gltf_skin.joints) {
            skin.joint_indices.push_back(joint_index);
        }
        
        skins_.push_back(skin);
        LOG_DEBUG_COMPONENT("AVATAR", "Loaded skin: " + skin.name + " joints: " + std::to_string(skin.joint_indices.size()));
    }

    LOG_DEBUG_COMPONENT("AVATAR", "Loaded model: " + path);
    LOG_DEBUG_COMPONENT("AVATAR", "  Meshes: " + std::to_string(model.meshes.size()));
    LOG_DEBUG_COMPONENT("AVATAR", "  Animations: " + std::to_string(model.animations.size()));
    LOG_DEBUG_COMPONENT("AVATAR", "  Accessors: " + std::to_string(model.accessors.size()));
    LOG_DEBUG_COMPONENT("AVATAR", "  BufferViews: " + std::to_string(model.bufferViews.size()));
    LOG_DEBUG_COMPONENT("AVATAR", "  Buffers: " + std::to_string(model.buffers.size()));
    LOG_DEBUG_COMPONENT("AVATAR", "  Nodes: " + std::to_string(model.nodes.size()));

    // Load animations
    if (!model.animations.empty()) {
        has_animation_ = true;
        for (const auto& gltf_anim : model.animations) {
            AnimationClip clip;
            clip.name = gltf_anim.name;
            float max_time = 0.0f;
            
            for (const auto& channel : gltf_anim.channels) {
                const auto& sampler = gltf_anim.samplers[channel.sampler];
                
                // Read time accessor
                const auto& time_accessor = model.accessors[sampler.input];
                const auto& time_bv = model.bufferViews[time_accessor.bufferView];
                const auto& time_buf = model.buffers[time_bv.buffer];
                const float* time_data = (const float*)(time_buf.data.data() + time_bv.byteOffset + time_accessor.byteOffset);
                size_t time_count = time_accessor.count;
                
                // Read value accessor
                const auto& val_accessor = model.accessors[sampler.output];
                const auto& val_bv = model.bufferViews[val_accessor.bufferView];
                const auto& val_buf = model.buffers[val_bv.buffer];
                const float* val_data = (const float*)(val_buf.data.data() + val_bv.byteOffset + val_accessor.byteOffset);
                
                AnimationNode node;
                node.node_index = channel.target_node;
                
                for (size_t i = 0; i < time_count; i++) {
                    if (time_data[i] > max_time) max_time = time_data[i];
                    node.times.push_back(time_data[i]);
                }
                
                if (channel.target_path == "translation") {
                    for (size_t i = 0; i < time_count; i++) {
                        node.translations.push_back({val_data[i*3+0], val_data[i*3+1], val_data[i*3+2]});
                    }
                } else if (channel.target_path == "rotation") {
                    for (size_t i = 0; i < time_count; i++) {
                        node.rotations.push_back({val_data[i*4+0], val_data[i*4+1], val_data[i*4+2], val_data[i*4+3]});
                    }
                } else if (channel.target_path == "scale") {
                    for (size_t i = 0; i < time_count; i++) {
                        node.scales.push_back({val_data[i*3+0], val_data[i*3+1], val_data[i*3+2]});
                    }
                }
                
                clip.nodes.push_back(node);
            }
            
            clip.duration = max_time;
            animations_.push_back(clip);
            LOG_DEBUG_COMPONENT("AVATAR", "  Animation: '" + clip.name + "' duration: " + std::to_string(clip.duration) + "s, nodes: " + std::to_string(clip.nodes.size()));
        }
    }

    // Process each mesh
    for (const auto& gltf_mesh : model.meshes) {
        for (const auto& primitive : gltf_mesh.primitives) {
            MeshData mesh_data;
            
            // Pre-determine vertex count to ensure all attributes load correctly
            size_t vertex_count = 0;
            auto pos_count_iter = primitive.attributes.find("POSITION");
            if (pos_count_iter != primitive.attributes.end()) {
                vertex_count = model.accessors[pos_count_iter->second].count;
            }

            if (vertex_count == 0) continue;
            mesh_data.vertices.resize(vertex_count);
            for(auto& v : mesh_data.vertices) {
                v.boneWeights[0] = 1.0f; // Default weight to prevent collapse if unskinned
            }

            // Get material
            if (primitive.material >= 0 && primitive.material < (int)model.materials.size()) {
                const auto& mat = model.materials[primitive.material];
                const auto& pbr = mat.pbrMetallicRoughness;
                mesh_data.material.baseColor = {
                    (float)pbr.baseColorFactor[0],
                    (float)pbr.baseColorFactor[1],
                    (float)pbr.baseColorFactor[2],
                    (float)pbr.baseColorFactor[3]
                };
                mesh_data.material.metallic = (float)pbr.metallicFactor;
                mesh_data.material.roughness = (float)pbr.roughnessFactor;
            }

            // Get indices
            if (primitive.indices >= 0) {
                const auto& accessor = model.accessors[primitive.indices];
                const auto& buffer_view = model.bufferViews[accessor.bufferView];
                const auto& buffer = model.buffers[buffer_view.buffer];
                
                const unsigned char* data = buffer.data.data() + buffer_view.byteOffset + accessor.byteOffset;
                size_t count = accessor.count;
                
                mesh_data.indices.resize(count);
                
                // Determine index type
                int component_type = accessor.componentType;
                for (size_t i = 0; i < count; i++) {
                    if (component_type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                        mesh_data.indices[i] = ((const unsigned short*)data)[i];
                    } else if (component_type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                        mesh_data.indices[i] = ((const unsigned int*)data)[i];
                    } else if (component_type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                        mesh_data.indices[i] = data[i];
                    }
                }
            }

            // Get positions
            auto pos_iter = primitive.attributes.find("POSITION");
            if (pos_iter != primitive.attributes.end()) {
                const auto& accessor = model.accessors[pos_iter->second];
                const auto& buffer_view = model.bufferViews[accessor.bufferView];
                const auto& buffer = model.buffers[buffer_view.buffer];
                
                const unsigned char* data = buffer.data.data() + buffer_view.byteOffset + accessor.byteOffset;
                const float* pos_data = (const float*)data;
                size_t count = accessor.count;
                
                for (size_t i = 0; i < count; i++) {
                    mesh_data.vertices[i].position.x = pos_data[i * 3 + 0];
                    mesh_data.vertices[i].position.y = pos_data[i * 3 + 1];
                    mesh_data.vertices[i].position.z = pos_data[i * 3 + 2];
                }
            }

            // Get normals
            auto norm_iter = primitive.attributes.find("NORMAL");
            if (norm_iter != primitive.attributes.end()) {
                const auto& accessor = model.accessors[norm_iter->second];
                const auto& buffer_view = model.bufferViews[accessor.bufferView];
                const auto& buffer = model.buffers[buffer_view.buffer];
                
                const unsigned char* data = buffer.data.data() + buffer_view.byteOffset + accessor.byteOffset;
                const float* norm_data = (const float*)data;
                size_t count = accessor.count;
                
                for (size_t i = 0; i < count; i++) {
                    mesh_data.vertices[i].normal.x = norm_data[i * 3 + 0];
                    mesh_data.vertices[i].normal.y = norm_data[i * 3 + 1];
                    mesh_data.vertices[i].normal.z = norm_data[i * 3 + 2];
                }
            }

            // Get texcoords
            auto tex_iter = primitive.attributes.find("TEXCOORD_0");
            if (tex_iter != primitive.attributes.end()) {
                const auto& accessor = model.accessors[tex_iter->second];
                const auto& buffer_view = model.bufferViews[accessor.bufferView];
                const auto& buffer = model.buffers[buffer_view.buffer];
                
                const unsigned char* data = buffer.data.data() + buffer_view.byteOffset + accessor.byteOffset;
                const float* tex_data = (const float*)data;
                size_t count = accessor.count;
                
                for (size_t i = 0; i < count; i++) {
                    mesh_data.vertices[i].texcoord.x = tex_data[i * 2 + 0];
                    mesh_data.vertices[i].texcoord.y = tex_data[i * 2 + 1];
                }
            }

            // Get Joint Indices
            auto joint_iter = primitive.attributes.find("JOINTS_0");
            if (joint_iter != primitive.attributes.end()) {
                const auto& accessor = model.accessors[joint_iter->second];
                
                if (accessor.bufferView >= 0 && accessor.bufferView < (int)model.bufferViews.size()) {
                    const auto& buffer_view = model.bufferViews[accessor.bufferView];
                    
                    if (buffer_view.buffer >= 0 && buffer_view.buffer < (int)model.buffers.size()) {
                        const auto& buffer = model.buffers[buffer_view.buffer];
                        
                        size_t data_offset = buffer_view.byteOffset + accessor.byteOffset;
                        if (data_offset < buffer.data.size()) {
                            const unsigned char* data = buffer.data.data() + data_offset;
                            size_t count = accessor.count;
                            
                            // Joint data can be unsigned byte or unsigned short
                            if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                                const uint8_t* joint_data = (const uint8_t*)data;
                                for (size_t i = 0; i < count && i < mesh_data.vertices.size(); i++) {
                                    for (int j = 0; j < 4; j++) {
                                        mesh_data.vertices[i].boneIndices[j] = joint_data[i * 4 + j];
                                    }
                                }
                            } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                                const uint16_t* joint_data = (const uint16_t*)data;
                                for (size_t i = 0; i < count && i < mesh_data.vertices.size(); i++) {
                                    for (int j = 0; j < 4; j++) {
                                        mesh_data.vertices[i].boneIndices[j] = joint_data[i * 4 + j];
                                    }
                                }
                            }
                        } else {
                            LOG_DEBUG_COMPONENT("AVATAR", "Invalid data offset for JOINTS_0");
                        }
                    } else {
                        LOG_DEBUG_COMPONENT("AVATAR", "Invalid buffer index for JOINTS_0");
                    }
                } else {
                    LOG_DEBUG_COMPONENT("AVATAR", "Invalid bufferView index for JOINTS_0");
                }
            } else {
                LOG_DEBUG_COMPONENT("AVATAR", "No JOINTS_0 attribute found in primitive");
            }

            // Get Weights
            auto weight_iter = primitive.attributes.find("WEIGHTS_0");
            if (weight_iter != primitive.attributes.end()) {
                const auto& accessor = model.accessors[weight_iter->second];
                
                if (accessor.bufferView >= 0 && accessor.bufferView < (int)model.bufferViews.size()) {
                    const auto& buffer_view = model.bufferViews[accessor.bufferView];
                    
                    if (buffer_view.buffer >= 0 && buffer_view.buffer < (int)model.buffers.size()) {
                        const auto& buffer = model.buffers[buffer_view.buffer];
                        
                        size_t data_offset = buffer_view.byteOffset + accessor.byteOffset;
                        if (data_offset < buffer.data.size()) {
                            const unsigned char* data = buffer.data.data() + data_offset;
                            const float* weight_data = (const float*)data;
                            size_t count = accessor.count;
                            
                            for (size_t i = 0; i < count && i < mesh_data.vertices.size(); i++) {
                                for (int j = 0; j < 4; j++) {
                                    mesh_data.vertices[i].boneWeights[j] = weight_data[i * 4 + j];
                                }
                            }
                        } else {
                            LOG_DEBUG_COMPONENT("AVATAR", "Invalid data offset for WEIGHTS_0");
                        }
                    } else {
                        LOG_DEBUG_COMPONENT("AVATAR", "Invalid buffer index for WEIGHTS_0");
                    }
                } else {
                    LOG_DEBUG_COMPONENT("AVATAR", "Invalid bufferView index for WEIGHTS_0");
                }
            } else {
                LOG_DEBUG_COMPONENT("AVATAR", "No WEIGHTS_0 attribute found in primitive");
            }

            // Compute normals if missing
            if (pos_iter != primitive.attributes.end() && norm_iter == primitive.attributes.end()) {
                // Compute flat normals
                for (size_t i = 0; i < mesh_data.indices.size(); i += 3) {
                    if (i + 2 >= mesh_data.indices.size()) break;
                    uint32_t i0 = mesh_data.indices[i];
                    uint32_t i1 = mesh_data.indices[i + 1];
                    uint32_t i2 = mesh_data.indices[i + 2];
                    
                    if (i0 >= mesh_data.vertices.size() || 
                        i1 >= mesh_data.vertices.size() || 
                        i2 >= mesh_data.vertices.size()) continue;
                    
                    DirectX::XMFLOAT3 p0 = mesh_data.vertices[i0].position;
                    DirectX::XMFLOAT3 p1 = mesh_data.vertices[i1].position;
                    DirectX::XMFLOAT3 p2 = mesh_data.vertices[i2].position;
                    
                    DirectX::XMVECTOR v0 = DirectX::XMLoadFloat3(&p0);
                    DirectX::XMVECTOR v1 = DirectX::XMLoadFloat3(&p1);
                    DirectX::XMVECTOR v2 = DirectX::XMLoadFloat3(&p2);
                    
                    DirectX::XMVECTOR edge1 = DirectX::XMVectorSubtract(v1, v0);
                    DirectX::XMVECTOR edge2 = DirectX::XMVectorSubtract(v2, v0);
                    DirectX::XMVECTOR normal = DirectX::XMVector3Cross(edge1, edge2);
                    normal = DirectX::XMVector3Normalize(normal);
                    
                    DirectX::XMFLOAT3 n;
                    DirectX::XMStoreFloat3(&n, normal);
                    
                    mesh_data.vertices[i0].normal = n;
                    mesh_data.vertices[i1].normal = n;
                    mesh_data.vertices[i2].normal = n;
                }
            }

            if (!mesh_data.vertices.empty()) {
                meshes_.push_back(std::move(mesh_data));
            }
        }
    }

    LOG_DEBUG_COMPONENT("AVATAR", "Loaded " + std::to_string(meshes_.size()) + " meshes");
    
    // Store original meshes for animation
    original_meshes_ = meshes_;
    
    // Pre-calculate transform matrices so first animation frame is valid
    build_transform_matrices();

    // Initialize bone matrices
    if (!skins_.empty()) {
        bone_matrices_.resize(64);
        for (size_t i = 0; i < bone_matrices_.size(); i++) {
            DirectX::XMStoreFloat4x4(&bone_matrices_[i], DirectX::XMMatrixIdentity());
        }
    }
    
    return !meshes_.empty();
}

void AvatarOverlay::find_taskbar_position(int& x, int& y, int& width, int& height) {
    // Get the taskbar window (shell's tray window)
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    RECT rect;
    
    if (taskbar && GetWindowRect(taskbar, &rect)) {
        // Calculate which edge the taskbar is on
        int screen_width = GetSystemMetrics(SM_CXSCREEN);
        int screen_height = GetSystemMetrics(SM_CYSCREEN);
        
        int tb_width = rect.right - rect.left;
        int tb_height = rect.bottom - rect.top;
        
        if (tb_height > tb_width) {
            // Taskbar on left or right side
            x = rect.left; // left edge of taskbar
            y = rect.top;
            width = tb_width;
            height = tb_height;
        } else {
            // Taskbar on bottom or top
            x = rect.left;
            y = rect.top;
            width = tb_width;
            height = tb_height;
        }
    } else {
        // Fallback: assume taskbar is at the bottom
        int screen_width = GetSystemMetrics(SM_CXSCREEN);
        int screen_height = GetSystemMetrics(SM_CYSCREEN);
        int taskbar_height = GetSystemMetrics(SM_CYICON) + 8; // ~48px typical
        
        x = 0;
        y = screen_height - taskbar_height;
        width = screen_width;
        height = taskbar_height;
    }
}

void AvatarOverlay::update_animation(float delta_time) {
    // Debug logging disabled to reduce console spam
    // static int debug_counter = 0;
    // if (debug_counter++ % 60 == 0) {
    //     std::cout << "[AVATAR] Animation update called: time=" << animation_time_ << " delta=" << delta_time << " has_anim=" << has_animation_ << " num_anims=" << animations_.size() << " num_nodes=" << scene_nodes_.size() << std::endl;
    // }
    
    if (!has_animation_ || animations_.empty() || scene_nodes_.empty()) {
        return;
    }
    
    animation_time_ += delta_time;
    
    // Loop animation
    if (animations_[current_animation_].duration > 0) {
        animation_time_ = fmod(animation_time_, animations_[current_animation_].duration);
    }
    
    // Apply animation to nodes
    const auto& clip = animations_[current_animation_];
    
    // Reset nodes to base transforms
    for (auto& node : scene_nodes_) {
        node.local_transform = compose_transform(node.translation, node.rotation, node.scale);
    }
    
    // Apply animation transforms
    for (const auto& anim_node : clip.nodes) {
        if (anim_node.node_index >= 0 && anim_node.node_index < (int)scene_nodes_.size()) {
            auto& node = scene_nodes_[anim_node.node_index];
            
            // Interpolate translation
            if (!anim_node.translations.empty()) {
                node.translation = interpolate_vector3(anim_node.translations, anim_node.times, animation_time_);
            }
            
            // Interpolate rotation
            if (!anim_node.rotations.empty()) {
                node.rotation = interpolate_quaternion(anim_node.rotations, anim_node.times, animation_time_);
            }
            
            // Interpolate scale
            if (!anim_node.scales.empty()) {
                node.scale = interpolate_vector3(anim_node.scales, anim_node.times, animation_time_);
            }
            
            // Update local transform
            node.local_transform = compose_transform(node.translation, node.rotation, node.scale);
        }
    }
    
    // Update node hierarchy
    update_node_hierarchy();
    
    // Apply animation to meshes
    apply_animation_to_meshes();
}

void AvatarOverlay::apply_animation_to_meshes() {
    // Debug logging disabled to reduce console spam
    // static int debug_counter = 0;
    // if (debug_counter++ % 60 == 0) {
    //     std::cout << "[AVATAR] Apply animation: has_anim=" << has_animation_ << " num_anims=" << animations_.size() << " num_nodes=" << scene_nodes_.size() << " num_skins=" << skins_.size() << std::endl;
    // }
    
    if (!has_animation_ || animations_.empty() || scene_nodes_.empty()) return;
    
    // Reset meshes to original positions
    for (size_t i = 0; i < meshes_.size() && i < original_meshes_.size(); i++) {
        meshes_[i].vertices = original_meshes_[i].vertices;
    }
    
    // Update bone matrices for shader
    if (!skins_.empty()) {
        const auto& skin = skins_[0];
        
        // Debug logging disabled to reduce console spam
        // if (debug_counter % 60 == 0) {
        //     std::cout << "[AVATAR] Processing skin: " << skin.name << " joints: " << skin.joint_indices.size() << " inverse_mats: " << skin.inverse_bind_matrices.size() << std::endl;
        // }
        
        for (size_t i = 0; i < skin.joint_indices.size() && i < bone_matrices_.size(); i++) {
            int joint_index = skin.joint_indices[i];
            if (joint_index >= 0 && joint_index < (int)scene_nodes_.size()) {
                // Get node world transform
                DirectX::XMMATRIX node_world = DirectX::XMLoadFloat4x4(&scene_nodes_[joint_index].world_transform); // This is the joint's world transform
                
                // Get inverse bind matrix
                DirectX::XMMATRIX inverse_bind = DirectX::XMLoadFloat4x4(&skin.inverse_bind_matrices[i]);
                
                // Calculate final bone matrix: inverse_bind * node_world
                // (model_overall_transform is now applied to world matrix instead)
                DirectX::XMMATRIX bone_matrix = inverse_bind * node_world;
                
                // Store in bone matrices array
                DirectX::XMStoreFloat4x4(&bone_matrices_[i], DirectX::XMMatrixTranspose(bone_matrix));
            }
        }
    }
}

void AvatarOverlay::update_node_hierarchy() {
    if (scene_nodes_.empty()) return;

    // Perform multiple passes to ensure parents are always updated before children,
    // regardless of their order in the vector.
    for (int pass = 0; pass < 8; ++pass) {
        for (size_t i = 0; i < scene_nodes_.size(); i++) {
            if (scene_nodes_[i].parent_index < 0) {
                scene_nodes_[i].world_transform = scene_nodes_[i].local_transform;
            } else {
                int parent_idx = scene_nodes_[i].parent_index;
                DirectX::XMMATRIX local = DirectX::XMLoadFloat4x4(&scene_nodes_[i].local_transform);
                DirectX::XMMATRIX parent_world = DirectX::XMLoadFloat4x4(&scene_nodes_[parent_idx].world_transform);
                DirectX::XMMATRIX world = DirectX::XMMatrixMultiply(local, parent_world);
                DirectX::XMStoreFloat4x4(&scene_nodes_[i].world_transform, world);
            }
        }
    }
}

DirectX::XMFLOAT3 AvatarOverlay::interpolate_vector3(const std::vector<DirectX::XMFLOAT3>& values, const std::vector<float>& times, float time) {
    if (values.empty()) return {0, 0, 0};
    if (values.size() == 1) return values[0];
    if (time <= times[0]) return values[0];
    if (time >= times.back()) return values.back();
    
    // Find the two keyframes that bracket the current time
    size_t idx = 0;
    for (size_t i = 0; i < times.size() - 1; i++) {
        if (time >= times[i] && time < times[i + 1]) {
            idx = i;
            break;
        }
    }
    
    // Linear interpolation
    float t = (time - times[idx]) / (times[idx + 1] - times[idx]);
    
    DirectX::XMFLOAT3 result;
    result.x = values[idx].x + t * (values[idx + 1].x - values[idx].x);
    result.y = values[idx].y + t * (values[idx + 1].y - values[idx].y);
    result.z = values[idx].z + t * (values[idx + 1].z - values[idx].z);
    
    return result;
}

DirectX::XMFLOAT4 AvatarOverlay::interpolate_quaternion(const std::vector<DirectX::XMFLOAT4>& values, const std::vector<float>& times, float time) {
    if (values.empty()) return {0, 0, 0, 1};
    if (values.size() == 1) return values[0];
    if (time <= times[0]) return values[0];
    if (time >= times.back()) return values.back();
    
    // Find the two keyframes that bracket the current time
    size_t idx = 0;
    for (size_t i = 0; i < times.size() - 1; i++) {
        if (time >= times[i] && time < times[i + 1]) {
            idx = i;
            break;
        }
    }
    
    // Spherical linear interpolation
    float t = (time - times[idx]) / (times[idx + 1] - times[idx]);
    
    DirectX::XMVECTOR q1 = DirectX::XMLoadFloat4(&values[idx]);
    DirectX::XMVECTOR q2 = DirectX::XMLoadFloat4(&values[idx + 1]);
    DirectX::XMVECTOR result = DirectX::XMQuaternionSlerp(q1, q2, t);
    
    DirectX::XMFLOAT4 result_float;
    DirectX::XMStoreFloat4(&result_float, result);
    
    return result_float;
}

DirectX::XMFLOAT4X4 AvatarOverlay::compose_transform(const DirectX::XMFLOAT3& t, const DirectX::XMFLOAT4& r, const DirectX::XMFLOAT3& s) {
    DirectX::XMVECTOR translation = DirectX::XMLoadFloat3(&t);
    DirectX::XMVECTOR rotation = DirectX::XMLoadFloat4(&r);
    DirectX::XMVECTOR scale = DirectX::XMLoadFloat3(&s);
    
    DirectX::XMMATRIX scale_mat = DirectX::XMMatrixScalingFromVector(scale);
    DirectX::XMMATRIX rotation_mat = DirectX::XMMatrixRotationQuaternion(rotation);
    DirectX::XMMATRIX translation_mat = DirectX::XMMatrixTranslationFromVector(translation);
    
    DirectX::XMMATRIX result = DirectX::XMMatrixMultiply(scale_mat, rotation_mat);
    result = DirectX::XMMatrixMultiply(result, translation_mat);
    
    DirectX::XMFLOAT4X4 result_float;
    DirectX::XMStoreFloat4x4(&result_float, result);
    
    return result_float;
}