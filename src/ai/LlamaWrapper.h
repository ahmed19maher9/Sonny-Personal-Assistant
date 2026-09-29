#pragma once

#include <string>
#include <functional>
#include <memory>
#include <vector>
#include <utility>
#include <mutex>
#include <thread>
#include <atomic>

#include "llama-cpp.h"

#ifdef SONNY_HAS_MTMD
#include "mtmd.h"
#endif

class LlamaWrapper {
public:
    using TokenCallback = std::function<void(const std::string&)>;

    LlamaWrapper();
    ~LlamaWrapper();

    bool initialize(const std::string& model_path, const std::string& llama_server_path = "", const std::string& mmproj_path = "", int port = 8081);
    void shutdown();

    std::string generate(const std::string& prompt, int max_tokens = 512, float temperature = 0.7f);
    void generate_stream(const std::string& prompt, TokenCallback callback, int max_tokens = 512, float temperature = 0.7f);

    bool has_vision() const { return has_vision_; }

    // === Model file pre-flight ===
    // llama.cpp reads `general.architecture` from the GGUF header and refuses a
    // model whose architecture its own list does not implement ("unknown model
    // architecture: '<name>'"), which until now surfaced only as a bare
    // "[LLAMA-NATIVE] Failed to load model from: ..." line. This reads the GGUF
    // metadata alone (ggml's gguf parser: no weights, no backend, a few
    // milliseconds even for a 8 GB file) so the reason and the alternatives can
    // be reported before the slow model load is attempted.
    struct ModelFileInfo {
        bool readable = false;      // the file was opened and parsed as GGUF
        bool is_projector = false;  // vision/audio projector (mmproj), not a language model
        bool unusable = false;      // llama.cpp cannot load this file on this build
        std::string architecture;   // GGUF general.architecture, e.g. "llama"
        std::string name;           // GGUF general.name, when present
        std::string problem;        // multi-line explanation, empty when nothing is wrong
    };
    static ModelFileInfo inspect_model_file(const std::string& model_path);

    std::string generate_with_images(const std::string& prompt, const std::vector<std::vector<unsigned char>>& images, int widths, int heights, int max_tokens = 512, float temperature = 0.7f);
    void generate_stream_with_images(const std::string& prompt, const std::vector<std::vector<unsigned char>>& images, int width, int height, TokenCallback callback, int max_tokens = 512, float temperature = 0.7f);

    void set_system_prompt(const std::string& prompt) { system_prompt_ = prompt; }
    // The GPU backend (CUDA / Vulkan) is auto-detected from the display
    // adapter vendor in initialize(); no manual selection is needed.
    void reset_conversation() { conversation_history_.clear(); history_.clear(); }
    bool is_initialized() const { return initialized_; }

    // === LoRA adapter support (vectors personalization) ===
    // A LoRA adapter is a small GGUF file (~100 KB-2 MB) of diff tensors.
    // The vectors shares adapter bytes between peers; each node applies
    // the received adapters to its local model via llama_set_adapters_lora().
    // Multiple adapters can be active simultaneously, each with its own scale.
    bool load_lora_adapter(const std::string& path);
    bool apply_lora(float scale);
    void clear_lora();
    bool has_lora() const { return !lora_adapters_.empty(); }
    int lora_adapter_count() const { return static_cast<int>(lora_adapters_.size()); }

    // Token stats from the most recent generate() / generate_stream() call:
    // total prompt tokens, how many were already in the KV cache (prefix hit),
    // and how many tokens were generated.
    int get_last_prompt_tokens() const { return last_prompt_tokens_; }
    int get_last_cached_tokens() const { return last_cached_tokens_; }
    int get_last_generated_tokens() const { return last_generated_tokens_; }

    // Timing breakdown of the most recent call, in milliseconds:
    // prompt prefill (decoding sent prompt tokens) and token generation.
    long long get_last_prefill_ms() const { return last_prefill_ms_; }
    long long get_last_generate_ms() const { return last_generate_ms_; }

    const std::string& get_system_prompt() const { return system_prompt_; }
    const std::string& get_conversation_history() const { return conversation_history_; }

private:
    bool initialized_;
    bool has_vision_ = false;
    std::string system_prompt_;
    std::string conversation_history_;
    std::string model_path_;
    std::string mmproj_path_;
    int port_;

    llama_model_ptr model_;
    llama_context_ptr context_;
    llama_sampler_ptr sampler_;
    const llama_vocab* vocab_;
#ifdef SONNY_HAS_MTMD
    mtmd::context_ptr mtmd_ctx_{nullptr};
#endif
    int n_ctx_ = 4096;
    int n_batch_ = 512;   // prefill micro-batch: same GPU prefill speed as larger
                          // batches, but ~4x smaller activation-memory peak (less
                          // VRAM pressure on 6 GB GPUs)
    int cache_pos_ = 0;
    std::string cached_system_prompt_;
    std::vector<llama_token> cached_tokens_;
    std::mutex generate_mutex_;
    std::mutex history_mutex_;
    std::vector<std::pair<std::string, std::string>> history_;
    static const size_t kMaxHistoryTurns = 10;
    float current_temperature_ = 0.1f;

    // Last-call token statistics (see getters above)
    int last_prompt_tokens_ = 0;
    int last_cached_tokens_ = 0;
    int last_generated_tokens_ = 0;
    long long last_prefill_ms_ = 0;
    long long last_generate_ms_ = 0;
    bool warned_slow_prefill_ = false;  // logs the VRAM/throttle diagnostic only once

    std::string build_prompt(const std::string& user_message) const;
    std::vector<llama_token> tokenize(const std::string& text, bool add_special = true) const;
    std::string detokenize(const llama_token* tokens, int n_tokens) const;
    bool sample_and_decode(llama_context* ctx, llama_sampler* smpl, llama_token& out_token, std::string& piece) const;
    void warmup();
    std::string generate_impl(const std::string& prompt, int max_tokens, float temperature, TokenCallback callback);
    std::string generate_impl_with_images(const std::string& prompt, const std::vector<std::vector<unsigned char>>& images, int width, int height, int max_tokens, float temperature, TokenCallback callback);

    // LoRA adapter state (vectors personalization).
    std::vector<llama_adapter_lora_ptr> lora_adapters_;
    std::vector<float> lora_scales_;
    mutable std::mutex lora_mutex_;
};