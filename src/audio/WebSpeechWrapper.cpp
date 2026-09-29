#include "WebSpeechWrapper.h"

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#include "Logger.h"
#include <iostream>
#include <fstream>
#include <sstream>

WebSpeechWrapper::WebSpeechWrapper() 
    : initialized_(false), running_(false), server_port_(8080), server_thread_running_(false) {
}

WebSpeechWrapper::~WebSpeechWrapper() {
    stop();
}

bool WebSpeechWrapper::initialize(const std::string& html_path) {
    html_path_ = html_path;
    
    LOG_DEBUG_COMPONENT("WebSpeech", "Initializing with HTML path: " + html_path);
    
    // Check if HTML file exists
    std::ifstream file(html_path);
    if (!file.good()) {
        LOG_DEBUG_COMPONENT("WebSpeech", "Failed to find HTML file: " + html_path);
        return false;
    }
    file.close();
    LOG_DEBUG_COMPONENT("WebSpeech", "HTML file found successfully");
    
    // Initialize Winsock on Windows
#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        LOG_DEBUG_COMPONENT("WebSpeech", "Failed to initialize Winsock");
        return false;
    }
#endif
    
    initialized_ = true;
    return true;
}

bool WebSpeechWrapper::start() {
    if (!initialized_ || running_) {
        return false;
    }
    
    running_ = true;
    server_thread_running_ = true;
    
    // Start HTTP server thread
    server_thread_ = std::thread(&WebSpeechWrapper::run_http_server, this);
    
    LOG_DEBUG_COMPONENT("WebSpeech", "Web Speech API server started on port " + std::to_string(server_port_));
    return true;
}

void WebSpeechWrapper::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    server_thread_running_ = false;
    
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    
#ifdef _WIN32
    WSACleanup();
#endif
}

bool WebSpeechWrapper::speak(const std::string& text, const std::string& voice_name) {
    if (!running_) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(tts_mutex_);
    tts_queue_.push(text);
    tts_voice_queue_.push(voice_name);
    
    return true;
}

std::string WebSpeechWrapper::get_transcription() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    
    if (transcription_queue_.empty()) {
        return "";
    }
    
    std::string result = transcription_queue_.front();
    transcription_queue_.pop();
    return result;
}

std::vector<VoiceInfo> WebSpeechWrapper::get_available_voices() {
    // This would need to be populated by querying the web page
    // For now, return a placeholder
    std::vector<VoiceInfo> voices;
    voices.push_back({"Default", "en-US", true});
    return voices;
}

std::vector<VoiceInfo> WebSpeechWrapper::get_web_voices() {
    std::vector<VoiceInfo> voices;
    
    std::lock_guard<std::mutex> lock(voices_mutex_);
    
    // Convert queue to vector
    while (!voices_queue_.empty()) {
        voices.push_back(voices_queue_.front());
        voices_queue_.pop();
    }
    
    return voices;
}

void WebSpeechWrapper::run_http_server() {
#ifdef _WIN32
    SOCKET server_socket, client_socket;
    sockaddr_in server_addr, client_addr;
    int addr_len = sizeof(client_addr);
    
    server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == INVALID_SOCKET) {
        LOG_DEBUG_COMPONENT("WebSpeech", "Failed to create socket");
        return;
    }
    
    // Allow address reuse to avoid "address already in use" errors
    int reuse = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port_);
    
    // Try to bind, if it fails, try to kill the stale process on the port
    if (bind(server_socket, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        DWORD bind_error = GetLastError();
        LOG_DEBUG_COMPONENT("WebSpeech", "Failed to bind socket on port " + std::to_string(server_port_) + " (error: " + std::to_string(bind_error) + ")");
        
        // Try to kill the process using our port via Windows IP Helper API
        // This is a best-effort attempt to clear stale processes
        LOG_DEBUG_COMPONENT("WebSpeech", "Attempting to free port " + std::to_string(server_port_) + "...");
        
        // Use GetExtendedTcpTable to find the process using our port
        ULONG tcp_size = 0;
        GetExtendedTcpTable(nullptr, &tcp_size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
        
        std::vector<BYTE> tcp_buffer(tcp_size);
        PMIB_TCPTABLE_OWNER_PID tcp_table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(tcp_buffer.data());
        
        DWORD result = GetExtendedTcpTable(tcp_table, &tcp_size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
        if (result == NO_ERROR) {
            for (DWORD i = 0; i < tcp_table->dwNumEntries; i++) {
                MIB_TCPROW_OWNER_PID& row = tcp_table->table[i];
                // Check if this entry is listening on our port
                if (row.dwState == MIB_TCP_STATE_LISTEN && 
                    ntohs((u_short)row.dwLocalPort) == server_port_) {
                    DWORD pid = row.dwOwningPid;
                    LOG_DEBUG_COMPONENT("WebSpeech", "Found stale process PID " + std::to_string(pid) + " on port " + std::to_string(server_port_) + ", terminating...");
                    
                    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                    if (hProcess) {
                        TerminateProcess(hProcess, 1);
                        CloseHandle(hProcess);
                        LOG_DEBUG_COMPONENT("WebSpeech", "Terminated stale process PID " + std::to_string(pid));
                        
                        // Wait a moment for the port to be released
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        
                        // Try to bind again
                        closesocket(server_socket);
                        server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                        if (server_socket != INVALID_SOCKET) {
                            setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, 
                                       (const char*)&reuse, sizeof(reuse));
                            if (bind(server_socket, (sockaddr*)&server_addr, 
                                     sizeof(server_addr)) == SOCKET_ERROR) {
                                LOG_DEBUG_COMPONENT("WebSpeech", "Still failed to bind after killing stale process");
                                closesocket(server_socket);
                                return;
                            }
                            LOG_DEBUG_COMPONENT("WebSpeech", "Successfully bound to port " + std::to_string(server_port_) + " after killing stale process");
                        } else {
                            return;
                        }
                    } else {
                        LOG_DEBUG_COMPONENT("WebSpeech", "Failed to open process PID " + std::to_string(pid));
                        closesocket(server_socket);
                        return;
                    }
                    break;
                }
            }
        } else {
            LOG_DEBUG_COMPONENT("WebSpeech", "GetExtendedTcpTable failed: " + std::to_string(result));
            closesocket(server_socket);
            return;
        }
    }
    
    if (listen(server_socket, SOMAXCONN) == SOCKET_ERROR) {
        LOG_DEBUG_COMPONENT("WebSpeech", "Failed to listen on socket");
        closesocket(server_socket);
        return;
    }
    
    LOG_DEBUG_COMPONENT("WebSpeech", "HTTP server listening on port " + std::to_string(server_port_));
    
    while (server_thread_running_) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_socket, &read_fds);
        
        timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;  // 100ms instead of 1s for lower latency
        
        int select_result = select(0, &read_fds, NULL, NULL, &timeout);
        if (select_result > 0) {
            client_socket = accept(server_socket, (sockaddr*)&client_addr, &addr_len);
            if (client_socket != INVALID_SOCKET) {
                std::string request;
                char buffer[8192];  // Increased from 4096 to 8192 for fewer recv calls
                int total_received = 0;
                
                // Receive data in a loop until we have the full request
                while (true) {
                    int bytes_received = recv(client_socket, buffer, sizeof(buffer), 0);
                    if (bytes_received <= 0) {
                        break;
                    }
                    
                    request.append(buffer, bytes_received);
                    total_received += bytes_received;
                    
                    // Check if we have received the full headers
                    size_t header_end = request.find("\r\n\r\n");
                    if (header_end != std::string::npos) {
                        // Parse Content-Length
                        size_t content_length_pos = request.find("Content-Length:");
                        if (content_length_pos != std::string::npos) {
                            size_t colon_pos = request.find(":", content_length_pos);
                            size_t end_pos = request.find("\r\n", colon_pos);
                            if (end_pos != std::string::npos) {
                                std::string length_str = request.substr(colon_pos + 1, end_pos - colon_pos - 1);
                                // Trim whitespace
                                size_t start = length_str.find_first_not_of(" \t");
                                size_t end = length_str.find_last_not_of(" \t");
                                if (start != std::string::npos && end != std::string::npos) {
                                    length_str = length_str.substr(start, end - start + 1);
                                    int content_length = std::stoi(length_str);
                                    size_t expected_total = header_end + 4 + content_length;
                                    
                                    if (total_received >= expected_total) {
                                        // We have the full request
                                        break;
                                    }
                                }
                            }
                        } else {
                            // No Content-Length header, assume we have the full request
                            break;
                        }
                    }
                }
                
                if (total_received > 0) {
                    std::string response;
                    
                    if (handle_request(request, response)) {
                        send(client_socket, response.c_str(), static_cast<int>(response.length()), 0);
                    }
                }
                
                closesocket(client_socket);
            }
        }
    }
    
    closesocket(server_socket);
#endif
}

bool WebSpeechWrapper::handle_request(const std::string& request, std::string& response) {
    // Parse the HTTP request
    std::istringstream iss(request);
    std::string method, path, version;
    iss >> method >> path >> version;
    
    // Strip query parameters from path
    size_t query_pos = path.find('?');
    if (query_pos != std::string::npos) {
        path = path.substr(0, query_pos);
    }
    
    LOG_DEBUG_COMPONENT("WebSpeech", "Received request: " + method + " " + path);
    
    // Serve the HTML file
    if (path == "/" || path == "/index.html") {
        std::ifstream file(html_path_);
        if (file.good()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            response = get_http_response(buffer.str());
            file.close();
            return true;
        } else {
            LOG_DEBUG_COMPONENT("WebSpeech", "Failed to open HTML file: " + html_path_);
            response = get_http_response("404 Not Found - HTML file not found", "text/plain", 404);
            return true;
        }
    }
    
    // API endpoint for speech recognition results
    if (path == "/api/speech" && method == "POST") {
        // Parse JSON body (simplified)
        size_t body_start = request.find("\r\n\r\n");
        if (body_start != std::string::npos) {
            std::string body = request.substr(body_start + 4);
            LOG_DEBUG_COMPONENT("WebSpeech", "POST /api/speech body: " + body);
            
            // Extract type and data from JSON using fast string operations instead of regex
            std::string type_val;
            std::string data_val;
            
            size_t type_pos = body.find("\"type\"");
            if (type_pos != std::string::npos) {
                size_t colon = body.find(':', type_pos);
                if (colon != std::string::npos) {
                    size_t quote_start = body.find('"', colon);
                    if (quote_start != std::string::npos) {
                        size_t quote_end = body.find('"', quote_start + 1);
                        if (quote_end != std::string::npos) {
                            type_val = body.substr(quote_start + 1, quote_end - quote_start - 1);
                        }
                    }
                }
            }
            
            size_t data_pos = body.find("\"data\"");
            if (data_pos != std::string::npos) {
                size_t colon = body.find(':', data_pos);
                if (colon != std::string::npos) {
                    size_t quote_start = body.find('"', colon);
                    if (quote_start != std::string::npos) {
                        // Find the matching closing quote for the string
                        // Handle both plain strings and JSON strings
                        size_t data_end = quote_start + 1;
                        int bracket_count = 0;
                        bool in_string = true; // Start in string since we're after the opening quote
                        
                        for (size_t i = quote_start + 1; i < body.size(); i++) {
                            char c = body[i];
                            if (c == '"' && body[i-1] != '\\') {
                                // Check if this is the closing quote
                                if (bracket_count == 0 && in_string) {
                                    data_val = body.substr(quote_start + 1, i - quote_start - 1);
                                    break;
                                }
                                in_string = !in_string;
                            }
                            if (!in_string) {
                                if (c == '[') bracket_count++;
                                else if (c == ']') {
                                    bracket_count--;
                                    if (bracket_count == 0) {
                                        // Find the closing quote after the ]
                                        size_t closing_quote = body.find('"', i);
                                        if (closing_quote != std::string::npos) {
                                            data_val = body.substr(quote_start + 1, closing_quote - quote_start - 1);
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            
            if (!type_val.empty()) {
                LOG_DEBUG_COMPONENT("WebSpeech", "Parsed - type: " + type_val + ", data: " + data_val);
                
                if (type_val == "stt") {
                    std::lock_guard<std::mutex> lock(queue_mutex_);
                    transcription_queue_.push(data_val);
                    LOG_DEBUG_COMPONENT("WebSpeech", "Received transcription: " + data_val);
                } else if (type_val == "voices") {
                    // Parse voices data - data_val now contains the JSON array string with escaped quotes
                    std::lock_guard<std::mutex> lock(voices_mutex_);
                    // Clear existing voices
                    while (!voices_queue_.empty()) {
                        voices_queue_.pop();
                    }
                    
                    // Unescape the JSON string (replace \" with ")
                    std::string unescaped_data = data_val;
                    size_t pos = 0;
                    while ((pos = unescaped_data.find("\\\"", pos)) != std::string::npos) {
                        unescaped_data.replace(pos, 2, "\"");
                        pos++;
                    }
                    
                    // Parse voice entries from unescaped data_val
                    pos = 0;
                    while (pos < unescaped_data.size()) {
                        size_t name_pos = unescaped_data.find("\"name\"", pos);
                        if (name_pos == std::string::npos) break;
                        
                        size_t name_colon = unescaped_data.find(':', name_pos);
                        if (name_colon == std::string::npos) break;
                        
                        size_t name_quote_start = unescaped_data.find('"', name_colon);
                        if (name_quote_start == std::string::npos) break;
                        
                        size_t name_quote_end = unescaped_data.find('"', name_quote_start + 1);
                        if (name_quote_end == std::string::npos) break;
                        
                        VoiceInfo voice;
                        voice.name = unescaped_data.substr(name_quote_start + 1, name_quote_end - name_quote_start - 1);
                        voice.default_voice = false;
                        voice.lang = "en-US";
                        
                        // Try to find lang for this voice
                        size_t lang_pos = unescaped_data.find("\"lang\"", name_quote_end);
                        if (lang_pos != std::string::npos) {
                            size_t lang_colon = unescaped_data.find(':', lang_pos);
                            if (lang_colon != std::string::npos) {
                                size_t lang_quote_start = unescaped_data.find('"', lang_colon);
                                if (lang_quote_start != std::string::npos) {
                                    size_t lang_quote_end = unescaped_data.find('"', lang_quote_start + 1);
                                    if (lang_quote_end != std::string::npos) {
                                        voice.lang = unescaped_data.substr(lang_quote_start + 1, lang_quote_end - lang_quote_start - 1);
                                    }
                                }
                            }
                        }
                        
                        voices_queue_.push(voice);
                        pos = name_quote_end + 1;
                    }
                    
                    LOG_DEBUG_COMPONENT("WebSpeech", "Received " + std::to_string(voices_queue_.size()) + " voices from web page");
                }
            }
        }
        
        response = get_cors_response("{\"status\":\"ok\"}");
        return true;
    }
    
    // API endpoint for TTS requests
    if (path == "/api/tts" && method == "GET") {
        std::string text, voice;
        
        {
            std::lock_guard<std::mutex> lock(tts_mutex_);
            if (!tts_queue_.empty()) {
                text = tts_queue_.front();
                tts_queue_.pop();
                voice = tts_voice_queue_.front();
                tts_voice_queue_.pop();
            }
        }
        
        if (!text.empty()) {
            response = get_cors_response("{\"text\":\"" + text + "\",\"voice\":\"" + voice + "\"}");
        } else {
            response = get_cors_response("{}");
        }
        return true;
    }
    
    // API endpoint for getting available voices
    if (path == "/api/voices" && method == "GET") {
        std::string voices_json = "[";
        {
            std::lock_guard<std::mutex> lock(voices_mutex_);
            // Return voices that have been sent from the web page
            std::queue<VoiceInfo> temp_queue = voices_queue_;
            bool first = true;
            while (!temp_queue.empty()) {
                if (!first) {
                    voices_json += ",";
                }
                first = false;
                
                VoiceInfo voice = temp_queue.front();
                temp_queue.pop();
                
                voices_json += "{\"name\":\"" + voice.name + "\",\"lang\":\"" + voice.lang + "\",\"default\":" + (voice.default_voice ? "true" : "false") + "}";
            }
        }
        voices_json += "]";
        response = get_cors_response(voices_json);
        return true;
    }
    
    // 404 for other paths
    LOG_DEBUG_COMPONENT("WebSpeech", "404: Path not found: " + path);
    response = get_http_response("404 Not Found", "text/plain", 404);
    return true;
}

std::string WebSpeechWrapper::get_http_response(const std::string& content, const std::string& content_type, int status_code) const {
    std::stringstream response;
    response << "HTTP/1.1 " << status_code;
    if (status_code == 200) {
        response << " OK";
    } else if (status_code == 404) {
        response << " Not Found";
    } else {
        response << " " << status_code;
    }
    response << "\r\n";
    response << "Content-Type: " << content_type << "\r\n";
    response << "Content-Length: " << content.length() << "\r\n";
    response << "Access-Control-Allow-Origin: *\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << content;
    return response.str();
}

std::string WebSpeechWrapper::get_cors_response(const std::string& content) const {
    return get_http_response(content, "application/json");
}
