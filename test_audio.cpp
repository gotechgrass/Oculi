#include "audio_analysis.h"
#include "audio_capture.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

void testAudioAnalyzerFftAndBands() {
    std::cout << "[Test] Running AudioAnalyzer FFT & Band Separation..." << std::endl;

    oculi::AudioAnalyzer analyzer(48000.0f);

    // 1. Test Silence -> Expect Dormant state
    std::vector<float> silence(2048, 0.0f);
    analyzer.pushSamples(silence.data(), silence.size());
    auto features = analyzer.getFeatures();
    assert(features.volume < 0.05f);
    assert(features.state == oculi::MusicState::Dormant);
    std::cout << "  - Silence test passed (State = Dormant, Volume = " << features.volume << ")" << std::endl;

    // 2. Test Low-Frequency Sine Wave (100 Hz = Bass)
    std::vector<float> bassWave(2048);
    for (size_t i = 0; i < bassWave.size(); ++i) {
        bassWave[i] = 0.8f * std::sin(2.0f * 3.14159265f * 100.0f * (float)i / 48000.0f);
    }
    analyzer.pushSamples(bassWave.data(), bassWave.size());
    features = analyzer.getFeatures();
    assert(features.bass > features.treble);
    assert(features.volume > 0.05f);
    std::cout << "  - Bass tone test passed (Bass = " << features.bass 
              << ", Mid = " << features.mid 
              << ", Treble = " << features.treble << ")" << std::endl;

    // 3. Test High-Frequency Sine Wave (5000 Hz = Treble)
    analyzer.reset();
    std::vector<float> trebleWave(2048);
    for (size_t i = 0; i < trebleWave.size(); ++i) {
        trebleWave[i] = 0.8f * std::sin(2.0f * 3.14159265f * 5000.0f * (float)i / 48000.0f);
    }
    analyzer.pushSamples(trebleWave.data(), trebleWave.size());
    features = analyzer.getFeatures();
    assert(features.treble > features.bass);
    std::cout << "  - Treble tone test passed (Treble = " << features.treble 
              << ", Bass = " << features.bass << ")" << std::endl;

    // 4. Test Beat Transient -> Expect Impact or Groove
    analyzer.reset();
    // Simulate a sharp impulse
    std::vector<float> impulse(2048, 0.0f);
    for (int i = 0; i < 256; ++i) {
        impulse[i] = (i % 2 == 0 ? 0.9f : -0.9f);
    }
    analyzer.pushSamples(impulse.data(), impulse.size());
    features = analyzer.getFeatures();
    assert(features.volume > 0.05f);
    std::cout << "  - Impulse/Transient test passed (State = " 
              << (features.state == oculi::MusicState::Impact ? "Impact" : 
                 (features.state == oculi::MusicState::Groove ? "Groove" : "Listening"))
              << ", BeatIntensity = " << features.beatIntensity << ")" << std::endl;

    std::cout << "All AudioAnalyzer tests passed successfully!" << std::endl;
}

void testLiveAudioCapture() {
    std::cout << "[Test] Running Live WASAPI Loopback Capture Test..." << std::endl;
    auto analyzer = std::make_shared<oculi::AudioAnalyzer>();
    oculi::AudioCapture capture(analyzer);
    if (!capture.start()) {
        std::cout << "  - AudioCapture could not start: " << capture.getLastError() << std::endl;
        return;
    }
    std::cout << "  - Capture started successfully." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    std::cout << "  - Detected endpoint: " << capture.getSampleRate() << " Hz, " 
              << capture.getChannels() << " channels" << std::endl;
    capture.stop();
    std::cout << "  - Capture stopped cleanly." << std::endl;
}

int main() {
    std::cout << "=== Oculi Audio Engine Tests ===" << std::endl;
    testAudioAnalyzerFftAndBands();
    testLiveAudioCapture();
    std::cout << "=== All tests completed ===" << std::endl;
    return 0;
}

