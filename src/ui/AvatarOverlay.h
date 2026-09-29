#pragma once

#include <windows.h>
#include <d3d11.h>
#include <DirectXMath.h>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <memory>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")

// Simple vertex structure with skinning support
struct SimpleVertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT2 texcoord;
    uint32_t boneIndices[4] = {0, 0, 0, 0};
    float boneWeights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

// Material properties
struct Material {
    DirectX::XMFLOAT4 baseColor = { 0.8f, 0.8f, 0.8f, 1.0f };
    float metallic = 0.0f;
    float roughness = 0.5f;
};

// Mesh data
struct MeshData {
    std::vector<SimpleVertex> vertices;
    std::vector<uint32_t> indices;
    Material material;
    DirectX::XMFLOAT3 center = { 0,0,0 };
    float radius = 1.0f;
};

class AvatarOverlay {
public:
    AvatarOverlay();
    ~AvatarOverlay();

    // Initialize the overlay window and D3D11
    bool initialize(HINSTANCE hInstance, const std::string& glb_path);
    
    // Show/hide the overlay
    void show();
    void hide();
    bool is_visible() const { return visible_; }

    // Start/stop the render loop
    bool start();
    void stop();

    // Set rotation speed
    void set_rotation_speed(float speed) { rotation_speed_ = speed; }

    // Load a .glb model (replaces current)
    bool load_model(const std::string& glb_path);

    // Set position on desktop (over taskbar)
    void set_desktop_position(int x, int y, int width, int height);

private:
    // Window procedure
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT msg, WPARAM wparam, LPARAM lparam);

    // D3D11 initialization
    bool create_device();
    bool create_render_target();
    bool create_shaders();
    bool create_buffers();
    bool create_depth_stencil();

    // Rendering
    void render_loop();
    void render_frame();
    void build_transform_matrices();
    void update_animation(float delta_time);
    void apply_animation_to_meshes();
    void update_node_hierarchy();
    DirectX::XMFLOAT3 interpolate_vector3(const std::vector<DirectX::XMFLOAT3>& values, const std::vector<float>& times, float time);
    DirectX::XMFLOAT4 interpolate_quaternion(const std::vector<DirectX::XMFLOAT4>& values, const std::vector<float>& times, float time);
    DirectX::XMFLOAT4X4 compose_transform(const DirectX::XMFLOAT3& t, const DirectX::XMFLOAT4& r, const DirectX::XMFLOAT3& s);
    
    // GLB loading
    bool load_glb_file(const std::string& path);

    // Position on taskbar
    void find_taskbar_position(int& x, int& y, int& width, int& height);

    // Window handles
    HWND hwnd_ = nullptr;
    HINSTANCE hinstance_ = nullptr;
    std::atomic<bool> running_{ false };
    std::atomic<bool> visible_{ false };
    std::unique_ptr<std::thread> render_thread_;

    // D3D11
    ID3D11Device* d3d_device_ = nullptr;
    ID3D11DeviceContext* d3d_context_ = nullptr;
    IDXGISwapChain* swap_chain_ = nullptr;
    ID3D11RenderTargetView* render_target_view_ = nullptr;
    ID3D11DepthStencilView* depth_stencil_view_ = nullptr;
    ID3D11Texture2D* depth_stencil_buffer_ = nullptr;
    
    // Shaders
    ID3D11VertexShader* vertex_shader_ = nullptr;
    ID3D11PixelShader* pixel_shader_ = nullptr;
    ID3D11InputLayout* input_layout_ = nullptr;
    ID3D11Buffer* vertex_buffer_ = nullptr;
    ID3D11Buffer* index_buffer_ = nullptr;
    ID3D11Buffer* constant_buffer_ = nullptr;
    ID3D11RasterizerState* rasterizer_state_ = nullptr;
    ID3D11BlendState* blend_state_ = nullptr;
    ID3D11DepthStencilState* depth_stencil_state_ = nullptr;

    // Model data
    std::vector<MeshData> meshes_;
    std::vector<MeshData> original_meshes_; // Store original vertices for animation
    std::string current_model_path_;

    // Node hierarchy for animation
    struct SceneNode {
        int index = -1;
        int parent_index = -1;
        int mesh_index = -1; // Which mesh this node has (-1 if none)
        DirectX::XMFLOAT3 translation = {0, 0, 0};
        DirectX::XMFLOAT4 rotation = {0, 0, 0, 1}; // quaternion
        DirectX::XMFLOAT3 scale = {1, 1, 1};
        DirectX::XMFLOAT4X4 local_transform;
        DirectX::XMFLOAT4X4 world_transform;
    };
    std::vector<SceneNode> scene_nodes_;

    // Window dimensions
    int window_width_ = 400;
    int window_height_ = 400;
    int window_x_ = 0;
    int window_y_ = 0;

    // Rotation (disabled by default - set to 0)
    float rotation_angle_ = 0.0f;
    float rotation_speed_ = 0.0f;
    DirectX::XMMATRIX model_overall_transform_;

    // Animation
    struct AnimationNode {
        int node_index = -1;
        std::vector<float> times;
        std::vector<DirectX::XMFLOAT3> translations;
        std::vector<DirectX::XMFLOAT4> rotations; // quaternions
        std::vector<DirectX::XMFLOAT3> scales;
    };
    struct AnimationClip {
        std::string name;
        std::vector<AnimationNode> nodes;
        float duration = 1.0f;
    };
    bool has_animation_ = false;
    float animation_time_ = 0.0f;
    std::vector<AnimationClip> animations_;
    int current_animation_ = 0;

    // Skinning data
    struct Skin {
        std::string name;
        int skin_index = -1;
        std::vector<int> joint_indices; // Node indices for joints
        std::vector<DirectX::XMFLOAT4X4> inverse_bind_matrices;
    };
    std::vector<Skin> skins_;
    std::vector<DirectX::XMFLOAT4X4> bone_matrices_; // Final bone matrices for shader

    // Constant buffer data
    struct ConstantBufferData {
        DirectX::XMMATRIX world;
        DirectX::XMMATRIX view;
        DirectX::XMMATRIX projection;
        DirectX::XMFLOAT4 lightDir;
        DirectX::XMFLOAT4 cameraPos;
        DirectX::XMFLOAT4X4 boneMatrices[64]; // Bone palette for skinning
    };
    ConstantBufferData cb_data_;
};
