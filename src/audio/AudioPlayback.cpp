#include "AudioPlayback.h"
#include <iostream>
#include <fstream>
#include <windows.h>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

AudioPlayback::AudioPlayback() 
    : initialized_(false), playing_(false), sample_rate_(24000), audio_handle_(nullptr), m_stop_requested_(false), device_ready_(false), device_sample_rate_(0) {
}

AudioPlayback::~AudioPlayback() {
    close_device();
}

bool AudioPlayback::initialize(int sample_rate) {
    sample_rate_ = sample_rate;
    initialized_ = true;
    return true;
}

bool AudioPlayback::ensure_device(int sample_rate) {
    if (device_ready_ && audio_handle_ && device_sample_rate_ == sample_rate) {
        return true;
    }
    close_device();

    WAVEFORMATEX waveFormat;
    waveFormat.wFormatTag = WAVE_FORMAT_PCM;
    waveFormat.nChannels = 1;
    waveFormat.nSamplesPerSec = (DWORD)sample_rate;
    waveFormat.wBitsPerSample = 16;
    waveFormat.nBlockAlign = (WORD)((waveFormat.nChannels * waveFormat.wBitsPerSample) / 8);
    waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;
    waveFormat.cbSize = 0;

    if (waveOutOpen(&audio_handle_, WAVE_MAPPER, &waveFormat, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        std::cerr << "Failed to open wave output device at " << sample_rate << " Hz" << std::endl;
        audio_handle_ = nullptr;
        return false;
    }
    device_ready_ = true;
    device_sample_rate_ = sample_rate;
    return true;
}

void AudioPlayback::close_device() {
    if (audio_handle_) {
        // Reset and close the device
        waveOutReset(audio_handle_);
        waveOutClose(audio_handle_);
        audio_handle_ = nullptr;
    }
    device_ready_ = false;
    device_sample_rate_ = 0;
    active_pcm_buffer_.clear();
    active_pcm_buffer_.shrink_to_fit();
}

bool AudioPlayback::play(const std::vector<float>& audio_data) {
    m_stop_requested_ = false;

    if (!initialized_) {
        return false;
    }

    if (audio_data.empty()) {
        std::cerr << "AudioPlayback: Empty audio data" << std::endl;
        return false;
    }

    if (!ensure_device(sample_rate_)) {
        return false;
    }

    try {
        active_pcm_buffer_.resize(audio_data.size());
        for (size_t i = 0; i < audio_data.size(); i++) {
            // Clamp to prevent overflow
            float sample = audio_data[i];
            if (sample > 1.0f) sample = 1.0f;
            if (sample < -1.0f) sample = -1.0f;
            active_pcm_buffer_[i] = static_cast<short>(sample * 32767.0f);
        }

        WAVEHDR waveHeader;
        memset(&waveHeader, 0, sizeof(WAVEHDR));
        waveHeader.lpData = reinterpret_cast<LPSTR>(active_pcm_buffer_.data());
        waveHeader.dwBufferLength = static_cast<DWORD>(active_pcm_buffer_.size() * sizeof(short));
        waveHeader.dwBytesRecorded = 0;
        waveHeader.dwUser = 0;
        waveHeader.dwFlags = 0;
        waveHeader.dwLoops = 0;
        waveHeader.lpNext = nullptr;
        waveHeader.reserved = 0;

        if (waveOutPrepareHeader(audio_handle_, &waveHeader, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            std::cerr << "Failed to prepare wave header" << std::endl;
            active_pcm_buffer_.clear();
            active_pcm_buffer_.shrink_to_fit();
            return false;
        }

        playing_ = true;
        
        if (waveOutWrite(audio_handle_, &waveHeader, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            std::cerr << "Failed to write audio data" << std::endl;
            waveOutUnprepareHeader(audio_handle_, &waveHeader, sizeof(WAVEHDR));
            playing_ = false;
            active_pcm_buffer_.clear();
            active_pcm_buffer_.shrink_to_fit();
            return false;
        }

        while (!(waveHeader.dwFlags & WHDR_DONE) && !m_stop_requested_) {
            Sleep(5);
        }

        if (m_stop_requested_) {
            waveOutReset(audio_handle_);
        }

        waveOutUnprepareHeader(audio_handle_, &waveHeader, sizeof(WAVEHDR));
        playing_ = false;
        active_pcm_buffer_.clear();
        active_pcm_buffer_.shrink_to_fit();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "AudioPlayback exception: " << e.what() << std::endl;
        playing_ = false;
        active_pcm_buffer_.clear();
        active_pcm_buffer_.shrink_to_fit();
        return false;
    } catch (...) {
        std::cerr << "AudioPlayback unknown exception" << std::endl;
        playing_ = false;
        active_pcm_buffer_.clear();
        active_pcm_buffer_.shrink_to_fit();
        return false;
    }
}

bool AudioPlayback::play_sync(const std::vector<float>& audio_data) {
    return play(audio_data);
}

void AudioPlayback::stop_playback() {
    m_stop_requested_ = true;
    if (audio_handle_) {
        waveOutReset(audio_handle_);
    }
}

void AudioPlayback::stop() {
    stop_playback();
    playing_ = false;
    close_device();
}

bool AudioPlayback::save_to_wav(const std::vector<float>& audio_data, const std::string& filename, int sample_rate) {
    std::vector<short> pcm_data(audio_data.size());
    for (size_t i = 0; i < audio_data.size(); i++) {
        pcm_data[i] = static_cast<short>(audio_data[i] * 32767.0f);
    }

    std::ofstream file(filename, std::ios::binary);
    if (!file) {
        return false;
    }

    const int data_size = static_cast<int>(pcm_data.size() * sizeof(short));
    const int file_size = 36 + data_size;

    file.write("RIFF", 4);
    file.write((const char*)&file_size, 4);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    
    const int fmt_chunk_size = 16;
    const short audio_format = 1;
    const short num_channels = 1;
    const short bits_per_sample = 16;
    const int byte_rate = sample_rate * num_channels * bits_per_sample / 8;
    const short block_align = num_channels * bits_per_sample / 8;

    file.write((const char*)&fmt_chunk_size, 4);
    file.write((const char*)&audio_format, 2);
    file.write((const char*)&num_channels, 2);
    file.write((const char*)&sample_rate, 4);
    file.write((const char*)&byte_rate, 4);
    file.write((const char*)&block_align, 2);
    file.write((const char*)&bits_per_sample, 2);
    file.write("data", 4);
    file.write((const char*)&data_size, 4);
    file.write((const char*)pcm_data.data(), data_size);

    file.close();
    return true;
}

bool AudioPlayback::play_file(const std::string& filename) {
    std::cerr << "AudioPlayback: Playing file: " << filename << std::endl;

    BOOL result = PlaySoundA(filename.c_str(), NULL, SND_FILENAME | SND_SYNC | SND_NODEFAULT);
    if (!result) {
        std::cerr << "Failed to play WAV file: " << filename << std::endl;
        return false;
    }

    return true;
}
