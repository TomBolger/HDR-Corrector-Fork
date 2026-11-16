#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AudioOutputDevice {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

// Active playback devices. Call from a thread with COM initialized.
std::vector<AudioOutputDevice> ListAudioOutputDevices();

class AudioRelay {
public:
    AudioRelay() = default;
    ~AudioRelay();

    AudioRelay(const AudioRelay&) = delete;
    AudioRelay& operator=(const AudioRelay&) = delete;

    // targetProcessId == 0: relay all desktop audio except HDR Corrector itself (original behavior).
    // targetProcessId != 0: relay only audio from that process and its child processes.
    bool Start(std::wstring& error, DWORD targetProcessId = 0);

    // Playback device the relay plays on. Empty = Windows default. Takes effect on next Start.
    void SetOutputDevice(const std::wstring& deviceId);
    void Stop();
    bool IsRunning() const;

private:
    void Worker();

    HANDLE stopEvent_ = nullptr;
    HANDLE startedEvent_ = nullptr;
    std::thread worker_;
    std::atomic_bool running_ = false;
    HRESULT startupResult_ = S_OK;
    DWORD targetProcessId_ = 0;
    std::wstring outputDeviceId_;
    mutable std::mutex stateMutex_;
    std::wstring startupError_;
};
