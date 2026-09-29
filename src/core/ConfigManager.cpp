#include "ConfigManager.h"
#include <iostream>
#include <windows.h>
#include <shlobj.h>

bool ConfigManager::save_config(const AppConfig& config, const std::string& filepath) {
    std::ofstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open config file for writing: " << filepath << std::endl;
        return false;
    }
    
    file << "[Settings]\n";
    file << "llama_model_path=" << config.llama_model_path << "\n";
    file << "kokoro_voice=" << config.kokoro_voice << "\n";
    file << "language=" << config.language << "\n";
    file << "tts_speed=" << config.tts_speed << "\n";
    file << "avatar_type=" << config.avatar_type << "\n";
    file << "system_prompt=" << config.system_prompt << "\n";
    file << "use_browser_stt=" << (config.use_browser_stt ? "true" : "false") << "\n";
    file << "use_browser_tts=" << (config.use_browser_tts ? "true" : "false") << "\n";
    file << "browser_voice=" << config.browser_voice << "\n";
    file << "show_avatar=" << (config.show_avatar ? "true" : "false") << "\n";
    file << "rag_embedding_model=" << config.rag_embedding_model << "\n";
    file << "wake_word_enabled=" << (config.wake_word_enabled ? "true" : "false") << "\n";
    file << "debug_mode=" << (config.debug_mode ? "true" : "false") << "\n";
    file << "enable_cava_visualizer=" << (config.enable_cava_visualizer ? "true" : "false") << "\n";
    file << "console_listening_mode=" << (config.console_listening_mode ? "true" : "false") << "\n";
    file << "mic_gain=" << config.mic_gain << "\n";
    file << "rag_max_context_chars=" << config.rag_max_context_chars << "\n";
    file << "camera_feed_enabled=" << (config.camera_feed_enabled ? "true" : "false") << "\n";
    file << "camera_device_name=" << config.camera_device_name << "\n";
    file << "vectors_enabled=" << (config.vectors_enabled ? "true" : "false") << "\n";
    file << "vectors_mode=" << config.vectors_mode << "\n";
    file << "vectors_room=" << config.vectors_room << "\n";
    file << "vectors_room_secret=" << config.vectors_room_secret << "\n";
    file << "vectors_rendezvous_url=" << config.vectors_rendezvous_url << "\n";
    file << "vectors_share_tool_metrics=" << (config.vectors_share_tool_metrics ? "true" : "false") << "\n";
    file << "vectors_k_anonymity=" << config.vectors_k_anonymity << "\n";
    file << "vectors_dp_sigma=" << config.vectors_dp_sigma << "\n";
    
    file.close();
    return true;
}

bool ConfigManager::load_config(AppConfig& config, const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Config file not found, using defaults: " << filepath << std::endl;
        return false;
    }
    
    std::string line;
    std::string current_section;
    
    while (std::getline(file, line)) {
        // Trim whitespace
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        
        // Section header
        if (line[0] == '[' && line.back() == ']') {
            current_section = line.substr(1, line.length() - 2);
            continue;
        }
        
        // Key-value pair
        size_t pos = line.find('=');
        if (pos != std::string::npos && current_section == "Settings") {
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            
            // Trim key and value
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            
            if (key == "llama_model_path") config.llama_model_path = value;
            else if (key == "kokoro_voice") config.kokoro_voice = value;
            else if (key == "language") config.language = value;
            else if (key == "tts_speed") config.tts_speed = std::stof(value);
            else if (key == "avatar_type") config.avatar_type = value;
            else if (key == "system_prompt") config.system_prompt = value;
            else if (key == "use_browser_stt") config.use_browser_stt = (value == "true");
            else if (key == "use_browser_tts") config.use_browser_tts = (value == "true");
            else if (key == "browser_voice") config.browser_voice = value;
            else if (key == "show_avatar") config.show_avatar = (value == "true");
            else if (key == "rag_embedding_model") config.rag_embedding_model = value;
            else if (key == "wake_word_enabled") config.wake_word_enabled = (value == "true");
            else if (key == "debug_mode") config.debug_mode = (value == "true");
            else if (key == "enable_cava_visualizer") config.enable_cava_visualizer = (value == "true");
            else if (key == "console_listening_mode") config.console_listening_mode = (value == "true");
            else if (key == "mic_gain") config.mic_gain = std::stof(value);
            else if (key == "rag_max_context_chars") config.rag_max_context_chars = std::stoi(value);
            else if (key == "camera_feed_enabled") config.camera_feed_enabled = (value == "true");
            else if (key == "camera_device_name") config.camera_device_name = value;
            else if (key == "vectors_enabled") config.vectors_enabled = (value == "true");
            else if (key == "vectors_mode") config.vectors_mode = value;
            else if (key == "vectors_room") config.vectors_room = value;
            else if (key == "vectors_room_secret") config.vectors_room_secret = value;
            else if (key == "vectors_rendezvous_url") config.vectors_rendezvous_url = value;
            else if (key == "vectors_share_tool_metrics") config.vectors_share_tool_metrics = (value == "true");
            else if (key == "vectors_k_anonymity") config.vectors_k_anonymity = std::stoi(value);
            else if (key == "vectors_dp_sigma") config.vectors_dp_sigma = std::stof(value);
        }
    }
    
    file.close();
    return true;
}

bool ConfigManager::is_first_run() {
    std::string path = get_config_path();
    std::ifstream file(path);
    return !file.good();
}

std::string ConfigManager::get_config_path() {
    // Use AppData directory for config file to avoid permission issues
    char appdata_path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata_path))) {
        std::string config_dir = std::string(appdata_path) + "\\Sonny";
        // Create directory if it doesn't exist
        CreateDirectoryA(config_dir.c_str(), NULL);
        return config_dir + "\\config.ini";
    }
    // Fallback to current directory if AppData is unavailable
    return "config.ini";
}
