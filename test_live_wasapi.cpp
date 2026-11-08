#include "audio_capture.h"
#include "audio_analysis.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {
std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running.store(false);
}

#if defined(_WIN32)
BOOL WINAPI consoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        g_running.store(false);
        return TRUE;
    }
    return FALSE;
}
#endif

const char* stateToString(oculi::MusicState state) {
    switch (state) {
        case oculi::MusicState::Dormant:   return "Dormant  (Silence / standby)";
        case oculi::MusicState::Listening: return "Listening (Low/ambient sound)";
        case oculi::MusicState::Groove:    return "Groove    (Rhythm detected)";
        case oculi::MusicState::Impact:    return "Impact    (Beat/transient peak)";
    }
    return "Unknown";
}

std::string makeProgressBar(float val, int width = 14) {
    val = std::min(1.0f, std::max(0.0f, val));
    int filled = static_cast<int>(val * width + 0.5f);
    std::string bar = "[";
    for (int i = 0; i < width; ++i) {
        if (i < filled) bar += "=";
        else if (i == filled && filled > 0) bar += ">";
        else bar += " ";
    }
    bar += "]";
    return bar;
}
} // namespace

int main(int argc, char** argv) {
    int maxSeconds = (argc > 1) ? std::atoi(argv[1]) : 0;
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
#if defined(_WIN32)
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#endif

    std::cout << "======================================================" << std::endl;
    std::cout << "       Oculi Live WASAPI Loopback Capture Test        " << std::endl;
    std::cout << "======================================================" << std::endl;
    std::cout << "Capturing system audio output via WASAPI loopback." << std::endl;
    std::cout << "(Play music through Spotify, YouTube, or your media player now.)" << std::endl;
    std::cout << "Press Ctrl+C at any time to stop cleanly.\n" << std::endl;

    auto analyzer = std::make_shared<oculi::AudioAnalyzer>();
    oculi::AudioCapture capture(analyzer);

    std::cout << "[1/2] Initializing WASAPI loopback on default render device..." << std::endl;
    if (!capture.start()) {
        std::cerr << "[ERROR] Failed to start WASAPI loopback capture: "
                  << capture.getLastError() << std::endl;
        std::cerr << "Ensure that an active audio playback device (speakers/headphones) is connected." << std::endl;
        return 1;
    }

    // Give background capture thread a moment to initialize and query device format
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    if (!capture.isRunning()) {
        std::cerr << "[ERROR] Audio capture thread stopped unexpectedly: "
                  << capture.getLastError() << std::endl;
        return 1;
    }

    std::cout << "[2/2] Capture successfully started!" << std::endl;
    std::cout << "  - Endpoint Sample Rate: " << capture.getSampleRate() << " Hz" << std::endl;
    std::cout << "  - Endpoint Channels:    " << capture.getChannels() << std::endl;
    std::cout << "--------------------------------------------------------------------------------" << std::endl;
    std::cout << std::left
              << std::setw(6)  << "Time"
              << std::setw(23) << "RMS / Volume Level"
              << std::setw(9)  << "Bass"
              << std::setw(9)  << "Mid"
              << std::setw(9)  << "Treble"
              << std::setw(11) << "Beat"
              << "State / Status"
              << std::endl;
    std::cout << "--------------------------------------------------------------------------------" << std::endl;

    int secondsElapsed = 0;
    bool hasReceivedAudio = false;

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!g_running.load()) break;

        secondsElapsed++;
        oculi::AudioFeatures f = analyzer->getFeatures();

        if (f.volume > 0.015f) {
            hasReceivedAudio = true;
        }

        std::cout << std::right << std::setw(4) << secondsElapsed << "s "
                  << std::left
                  << makeProgressBar(f.volume, 14) << " "
                  << std::fixed << std::setprecision(2) << std::setw(4) << f.volume << " "
                  << "B:" << std::setw(4) << f.bass << " "
                  << "M:" << std::setw(4) << f.mid << " "
                  << "T:" << std::setw(4) << f.treble << " "
                  << (f.isBeat ? " [*BEAT*] " : (f.beatIntensity > 0.3f ? " (beat)   " : "        "))
                  << stateToString(f.state);

        if (!hasReceivedAudio && secondsElapsed >= 3 && secondsElapsed % 4 == 0) {
            std::cout << " <-- (No sound detected; play music in Spotify/YouTube)";
        }

        std::cout << std::endl;

        if (maxSeconds > 0 && secondsElapsed >= maxSeconds) {
            std::cout << "\n[!] Specified duration (" << maxSeconds << "s) reached. Stopping capture cleanly..." << std::endl;
            break;
        }
    }

    std::cout << "\nStopping WASAPI capture cleanly..." << std::endl;
    capture.stop();
    std::cout << "[OK] Audio capture stopped. Resources freed." << std::endl;

    if (hasReceivedAudio) {
        std::cout << "[SUCCESS] Actual system audio stream was successfully received and analyzed." << std::endl;
    } else {
        std::cout << "[NOTE] Capture ran, but silence was received. Ensure media was actively playing on the default device." << std::endl;
    }

    return 0;
}
