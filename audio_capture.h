#pragma once

#include "audio_analysis.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace oculi {

class AudioCapture {
public:
    explicit AudioCapture(std::shared_ptr<AudioAnalyzer> analyzer);
    ~AudioCapture();

    // Start WASAPI loopback capture on a background thread
    bool start();

    // Stop audio capture and release COM resources
    void stop();

    // Check if capture loop is actively running
    bool isRunning() const { return m_running.load(); }

    // Get capture format details
    int getSampleRate() const { return m_sampleRate; }
    int getChannels() const { return m_channels; }
    std::string getLastError() const { return m_lastError; }

private:
    void captureLoop();

    std::shared_ptr<AudioAnalyzer> m_analyzer;
    std::thread m_thread;
    std::atomic<bool> m_running{false};

    int m_sampleRate = 48000;
    int m_channels = 2;
    std::string m_lastError;
};

} // namespace oculi

