#include "audio_capture.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <initguid.h>

// Direct explicit GUID definitions to avoid linker symbol issues across GCC/Clang/MSVC
namespace {
static const IID IID_IMMDeviceEnumerator_local = {
    0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
static const CLSID CLSID_MMDeviceEnumerator_local = {
    0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
static const IID IID_IAudioClient_local = {
    0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
static const IID IID_IAudioCaptureClient_local = {
    0xc8749423, 0x43e0, 0x45e6, {0x9f, 0x08, 0x41, 0x40, 0x44, 0x91, 0x92, 0x76}};
static const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_local = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID KSDATAFORMAT_SUBTYPE_PCM_local = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

template <typename T>
void safeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}
} // namespace

namespace oculi {

AudioCapture::AudioCapture(std::shared_ptr<AudioAnalyzer> analyzer)
    : m_analyzer(std::move(analyzer)) {}

AudioCapture::~AudioCapture() {
    stop();
}

bool AudioCapture::start() {
    if (m_running.load()) return true;

    m_running.store(true);
    m_thread = std::thread(&AudioCapture::captureLoop, this);
    return true;
}

void AudioCapture::stop() {
    if (!m_running.load()) return;

    m_running.store(false);
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void AudioCapture::captureLoop() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitSucceeded = SUCCEEDED(hr);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX* pwfx = nullptr;

    auto cleanup = [&]() {
        if (audioClient) audioClient->Stop();
        safeRelease(captureClient);
        safeRelease(audioClient);
        safeRelease(device);
        safeRelease(enumerator);
        if (pwfx) {
            CoTaskMemFree(pwfx);
            pwfx = nullptr;
        }
        if (coInitSucceeded) {
            CoUninitialize();
        }
    };

    hr = CoCreateInstance(CLSID_MMDeviceEnumerator_local, nullptr, CLSCTX_ALL,
                          IID_IMMDeviceEnumerator_local, (void**)&enumerator);
    if (FAILED(hr)) {
        m_lastError = "Failed to create MMDeviceEnumerator";
        cleanup();
        m_running.store(false);
        return;
    }

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        m_lastError = "No default audio render endpoint found";
        cleanup();
        m_running.store(false);
        return;
    }

    hr = device->Activate(IID_IAudioClient_local, CLSCTX_ALL, nullptr, (void**)&audioClient);
    if (FAILED(hr)) {
        m_lastError = "Failed to activate IAudioClient";
        cleanup();
        m_running.store(false);
        return;
    }

    hr = audioClient->GetMixFormat(&pwfx);
    if (FAILED(hr) || !pwfx) {
        m_lastError = "Failed to retrieve audio mix format";
        cleanup();
        m_running.store(false);
        return;
    }

    m_sampleRate = (int)pwfx->nSamplesPerSec;
    m_channels = (int)pwfx->nChannels;

    if (m_analyzer) {
        m_analyzer->setSampleRate((float)m_sampleRate);
    }

    // Determine sample format (Float32 vs PCM16)
    bool isFloat = false;
    if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        isFloat = true;
    } else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* pEx = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx);
        if (IsEqualGUID(pEx->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_local)) {
            isFloat = true;
        }
    }

    // Request ~50ms buffer for responsive loopback capture
    REFERENCE_TIME hnsRequestedDuration = 500000; // 50ms in 100ns units
    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_LOOPBACK,
                                 hnsRequestedDuration,
                                 0,
                                 pwfx,
                                 nullptr);
    if (FAILED(hr)) {
        m_lastError = "Failed to initialize WASAPI loopback client";
        cleanup();
        m_running.store(false);
        return;
    }

    hr = audioClient->GetService(IID_IAudioCaptureClient_local, (void**)&captureClient);
    if (FAILED(hr)) {
        m_lastError = "Failed to obtain IAudioCaptureClient";
        cleanup();
        m_running.store(false);
        return;
    }

    hr = audioClient->Start();
    if (FAILED(hr)) {
        m_lastError = "Failed to start audio client stream";
        cleanup();
        m_running.store(false);
        return;
    }

    std::vector<float> monoBuffer;
    monoBuffer.reserve(4096);

    while (m_running.load()) {
        UINT32 packetLength = 0;
        hr = captureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        while (packetLength > 0 && m_running.load()) {
            BYTE* pData = nullptr;
            UINT32 numFramesAvailable = 0;
            DWORD flags = 0;

            hr = captureClient->GetBuffer(&pData, &numFramesAvailable, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            if (numFramesAvailable > 0 && m_analyzer) {
                monoBuffer.resize(numFramesAvailable);

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill(monoBuffer.begin(), monoBuffer.end(), 0.0f);
                } else if (pData) {
                    const int channels = m_channels > 0 ? m_channels : 1;

                    if (isFloat && pwfx->wBitsPerSample == 32) {
                        const float* src = reinterpret_cast<const float*>(pData);
                        for (UINT32 f = 0; f < numFramesAvailable; ++f) {
                            float sum = 0.0f;
                            for (int c = 0; c < channels; ++c) {
                                sum += src[f * channels + c];
                            }
                            monoBuffer[f] = sum / (float)channels;
                        }
                    } else if (pwfx->wBitsPerSample == 16) {
                        const int16_t* src = reinterpret_cast<const int16_t*>(pData);
                        for (UINT32 f = 0; f < numFramesAvailable; ++f) {
                            float sum = 0.0f;
                            for (int c = 0; c < channels; ++c) {
                                sum += (float)src[f * channels + c] / 32768.0f;
                            }
                            monoBuffer[f] = sum / (float)channels;
                        }
                    } else {
                        // Fallback for unexpected formats
                        std::fill(monoBuffer.begin(), monoBuffer.end(), 0.0f);
                    }
                }

                m_analyzer->pushSamples(monoBuffer.data(), numFramesAvailable);
            }

            captureClient->ReleaseBuffer(numFramesAvailable);
            hr = captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    cleanup();
}

} // namespace oculi

#else // !_WIN32

// Non-Windows stub
namespace oculi {
AudioCapture::AudioCapture(std::shared_ptr<AudioAnalyzer> analyzer) : m_analyzer(std::move(analyzer)) {}
AudioCapture::~AudioCapture() {}
bool AudioCapture::start() { m_lastError = "WASAPI loopback is Windows-only"; return false; }
void AudioCapture::stop() {}
void AudioCapture::captureLoop() {}
} // namespace oculi

#endif

