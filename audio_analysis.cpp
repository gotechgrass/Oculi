#include "audio_analysis.h"

#include <numeric>

namespace oculi {

namespace {
constexpr float PI = 3.14159265358979323846f;

// Ballistic envelope filter helper
inline float applyBallistic(float current, float target, float attack, float decay) {
    float rate = (target > current) ? attack : decay;
    return current + (target - current) * rate;
}

inline float clamp01(float v) {
    return std::min(1.0f, std::max(0.0f, v));
}
} // namespace

AudioAnalyzer::AudioAnalyzer(float sampleRate)
    : m_sampleRate(sampleRate) {
    initHannWindow();
    m_sampleRing.assign(FFT_SIZE * 2, 0.0f);
    m_fftBuffer.resize(FFT_SIZE);
    m_prevMagnitudes.assign(FFT_SIZE / 2, 0.0f);
    m_fluxHistory.reserve(32);
}

void AudioAnalyzer::setSampleRate(float sampleRate) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (sampleRate > 8000.0f && sampleRate < 192000.0f) {
        m_sampleRate = sampleRate;
    }
}

void AudioAnalyzer::initHannWindow() {
    m_hannWindow.resize(FFT_SIZE);
    for (int i = 0; i < FFT_SIZE; ++i) {
        m_hannWindow[i] = 0.5f * (1.0f - std::cos(2.0f * PI * (float)i / (float)(FFT_SIZE - 1)));
    }
}

void AudioAnalyzer::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::fill(m_sampleRing.begin(), m_sampleRing.end(), 0.0f);
    std::fill(m_prevMagnitudes.begin(), m_prevMagnitudes.end(), 0.0f);
    m_fluxHistory.clear();
    m_smoothedVolume = 0.0f;
    m_smoothedBass = 0.0f;
    m_smoothedMid = 0.0f;
    m_smoothedTreble = 0.0f;
    m_beatDecay = 0.0f;
    m_beatCooldown = 0;
    m_consecutiveBeatFrames = 0;
    m_quietFrames = 20;
    m_impactTimer = 0;
    m_ringWritePos = 0;
    m_samplesSinceLastFft = 0;
    m_features = AudioFeatures{};
}

void AudioAnalyzer::pushSamples(const float* samples, size_t count) {
    if (!samples || count == 0) return;

    const size_t ringSize = m_sampleRing.size();
    const size_t hopSize = FFT_SIZE / 4; // 256 samples hop (~5.3ms at 48kHz)

    for (size_t i = 0; i < count; ++i) {
        m_sampleRing[m_ringWritePos] = samples[i];
        m_ringWritePos = (m_ringWritePos + 1) % ringSize;
        m_samplesSinceLastFft++;

        // Whenever we accumulate a hop size, perform an FFT analysis
        if (m_samplesSinceLastFft >= hopSize) {
            m_samplesSinceLastFft = 0;

            // Extract the most recent FFT_SIZE samples in chronological order
            std::vector<float> window(FFT_SIZE);
            size_t readPos = (m_ringWritePos + ringSize - FFT_SIZE) % ringSize;
            for (int k = 0; k < FFT_SIZE; ++k) {
                window[k] = m_sampleRing[(readPos + k) % ringSize];
            }

            analyzeWindow(window.data());
        }
    }
}

void AudioAnalyzer::computeRadix2Fft(std::vector<std::complex<float>>& buffer) {
    const size_t n = buffer.size();
    if (n <= 1) return;

    // Bit-reversal permutation
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(buffer[i], buffer[j]);
        }
    }

    // Cooley-Tukey butterflies
    for (size_t len = 2; len <= n; len <<= 1) {
        float angle = -2.0f * PI / (float)len;
        std::complex<float> wlen(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t j = 0; j < len / 2; ++j) {
                std::complex<float> u = buffer[i + j];
                std::complex<float> v = buffer[i + j + len / 2] * w;
                buffer[i + j] = u + v;
                buffer[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

void AudioAnalyzer::analyzeWindow(const float* samples) {
    double sumSquares = 0.0;
    for (int i = 0; i < FFT_SIZE; ++i) {
        float s = samples[i];
        sumSquares += (double)(s * s);
        m_fftBuffer[i] = std::complex<float>(s * m_hannWindow[i], 0.0f);
    }
    float rawRms = (float)std::sqrt(sumSquares / (double)FFT_SIZE);

    computeRadix2Fft(m_fftBuffer);

    // Compute magnitude spectrum for bins 0 to FFT_SIZE/2 - 1
    const int numBins = FFT_SIZE / 2;
    std::vector<float> mag(numBins);
    for (int i = 0; i < numBins; ++i) {
        mag[i] = std::abs(m_fftBuffer[i]) / (float)numBins;
    }

    // Frequency resolution: binWidth = sampleRate / FFT_SIZE
    const float binWidth = m_sampleRate / (float)FFT_SIZE;

    auto freqToBin = [binWidth, numBins](float freq) -> int {
        int b = (int)std::round(freq / binWidth);
        return std::max(1, std::min(numBins - 1, b));
    };

    // Define frequency bands
    const int bassMin = freqToBin(20.0f);
    const int bassMax = freqToBin(250.0f);

    const int midMin = bassMax + 1;
    const int midMax = freqToBin(2500.0f);

    const int trebleMin = midMax + 1;
    const int trebleMax = freqToBin(16000.0f);

    // Extract average band magnitudes
    auto averageBand = [&mag](int startBin, int endBin) -> float {
        if (startBin >= endBin) return 0.0f;
        float sum = 0.0f;
        for (int i = startBin; i <= endBin; ++i) sum += mag[i];
        return sum / (float)(endBin - startBin + 1);
    };

    float rawBass = averageBand(bassMin, bassMax);
    float rawMid = averageBand(midMin, midMax);
    float rawTreble = averageBand(trebleMin, trebleMax);

    // Spectral flux in bass region for beat onset detection
    float rawFlux = 0.0f;
    for (int i = bassMin; i <= bassMax; ++i) {
        float diff = mag[i] - m_prevMagnitudes[i];
        if (diff > 0.0f) rawFlux += diff;
        m_prevMagnitudes[i] = mag[i];
    }

    updateSmoothing(rawRms, rawBass, rawMid, rawTreble, rawFlux);
}

void AudioAnalyzer::updateSmoothing(float rawRms, float rawBass, float rawMid, float rawTreble, float rawFlux) {
    // Normalization gains calibrated for typical system audio levels
    float normRms = clamp01(rawRms * 4.0f);
    float normBass = clamp01(rawBass * 20.0f);
    float normMid = clamp01(rawMid * 40.0f);
    float normTreble = clamp01(rawTreble * 80.0f);

    // Ballistic smoothing: fast attack (responsive to drum hits), smooth decay
    m_smoothedVolume = applyBallistic(m_smoothedVolume, normRms, 0.75f, 0.15f);
    m_smoothedBass = applyBallistic(m_smoothedBass, normBass, 0.85f, 0.12f);
    m_smoothedMid = applyBallistic(m_smoothedMid, normMid, 0.70f, 0.18f);
    m_smoothedTreble = applyBallistic(m_smoothedTreble, normTreble, 0.70f, 0.20f);

    // Moving average of flux for dynamic beat thresholding
    m_fluxHistory.push_back(rawFlux);
    if (m_fluxHistory.size() > 24) {
        m_fluxHistory.erase(m_fluxHistory.begin());
    }
    float avgFlux = 0.0f;
    for (float f : m_fluxHistory) avgFlux += f;
    avgFlux /= (float)m_fluxHistory.size();

    bool isBeat = false;
    m_beatCooldown = std::max(0, m_beatCooldown - 1);

    if (rawFlux > avgFlux * 1.50f && rawFlux > 0.015f && m_beatCooldown == 0 && normRms > 0.03f) {
        isBeat = true;
        m_beatCooldown = 7; // ~37ms cooldown at 256 hop
        m_beatDecay = 1.0f;
        m_consecutiveBeatFrames++;
    } else {
        m_beatDecay *= 0.82f; // Smooth exponential decay of beat pulse
    }

    // Four-state music machine: Dormant, Listening, Groove, Impact
    MusicState newState = m_features.state;

    if (m_smoothedVolume < 0.015f) {
        m_quietFrames++;
        if (m_quietFrames > 15 || m_features.state == MusicState::Dormant) {
            newState = MusicState::Dormant;
            m_consecutiveBeatFrames = 0;
        } else {
            newState = MusicState::Listening;
        }
    } else {
        m_quietFrames = 0;
        if (m_impactTimer > 0) {
            m_impactTimer--;
            newState = MusicState::Impact;
        } else if (isBeat && (m_smoothedBass > 0.45f || m_beatDecay > 0.85f)) {
            m_impactTimer = 3; // Latch impact for 3 analysis cycles (~15ms)
            newState = MusicState::Impact;
        } else if (m_consecutiveBeatFrames >= 2 || (m_smoothedBass > 0.25f && m_smoothedVolume > 0.10f)) {
            newState = MusicState::Groove;
        } else {
            newState = MusicState::Listening;
        }
    }

    // Thread-safe update of published features
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_features.volume = m_smoothedVolume;
        m_features.bass = m_smoothedBass;
        m_features.mid = m_smoothedMid;
        m_features.treble = m_smoothedTreble;
        m_features.beatIntensity = m_beatDecay;
        m_features.isBeat = isBeat;
        m_features.state = newState;
    }
}

AudioFeatures AudioAnalyzer::getFeatures() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_features;
}

} // namespace oculi
