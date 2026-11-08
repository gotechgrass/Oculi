#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <mutex>
#include <vector>

namespace oculi {

enum class MusicState {
    Dormant,    // Silence or sub-threshold audio
    Listening,  // Ambient audio, speech, quiet passage
    Groove,     // Rhythmic music, sustained beat detected
    Impact      // Beat drop, transient peak, or rhythmic onset
};

struct AudioFeatures {
    float volume = 0.0f;        // Normalized RMS volume (0.0 to 1.0)
    float bass = 0.0f;          // Low frequencies (~20 - 250 Hz, 0.0 to 1.0)
    float mid = 0.0f;           // Mid frequencies (~250 - 2500 Hz, 0.0 to 1.0)
    float treble = 0.0f;        // High frequencies (~2500 - 16000 Hz, 0.0 to 1.0)
    float beatIntensity = 0.0f; // Beat transient pulse with decay (0.0 to 1.0)
    bool isBeat = false;        // Instantaneous onset flag for current frame
    MusicState state = MusicState::Dormant;
};

class AudioAnalyzer {
public:
    static constexpr int FFT_SIZE = 1024;

    explicit AudioAnalyzer(float sampleRate = 48000.0f);
    ~AudioAnalyzer() = default;

    // Set or update the audio sample rate
    void setSampleRate(float sampleRate);

    // Push new mono PCM float samples (called from audio capture thread)
    void pushSamples(const float* samples, size_t count);

    // Retrieve thread-safe copy of current audio features for rendering
    AudioFeatures getFeatures();

    // Reset smoothing filters and state
    void reset();

private:
    void initHannWindow();
    void computeRadix2Fft(std::vector<std::complex<float>>& buffer);
    void analyzeWindow(const float* samples);
    void updateSmoothing(float rawRms, float rawBass, float rawMid, float rawTreble, float rawFlux);

    float m_sampleRate;
    std::vector<float> m_hannWindow;
    std::vector<float> m_sampleRing;
    size_t m_ringWritePos = 0;
    size_t m_samplesSinceLastFft = 0;

    std::vector<std::complex<float>> m_fftBuffer;
    std::vector<float> m_prevMagnitudes;
    std::vector<float> m_fluxHistory;

    // Smoothed values (attack/decay ballistic filters)
    float m_smoothedVolume = 0.0f;
    float m_smoothedBass = 0.0f;
    float m_smoothedMid = 0.0f;
    float m_smoothedTreble = 0.0f;
    float m_beatDecay = 0.0f;

    // Beat detection state
    int m_beatCooldown = 0;
    int m_consecutiveBeatFrames = 0;
    int m_quietFrames = 20;
    int m_impactTimer = 0;

    // Thread-safe features
    AudioFeatures m_features;
    mutable std::mutex m_mutex;
};

} // namespace oculi
