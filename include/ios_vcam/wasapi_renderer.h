#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace ios_vcam {

class SharedCaptureConsumer;

struct AudioEndpointInfo {
    std::wstring id;
    std::string friendly_name_utf8;
};

class WasapiAudioRouter {
public:
    WasapiAudioRouter();
    ~WasapiAudioRouter();

    WasapiAudioRouter(const WasapiAudioRouter&) = delete;
    WasapiAudioRouter& operator=(const WasapiAudioRouter&) = delete;

    static std::vector<AudioEndpointInfo> EnumerateRenderEndpoints();

    // Starts low-latency WASAPI routing from shared memory to target_name_substring
    // (if empty, auto-detects "CABLE Input" / "VB-Audio Virtual Cable").
    bool StartAutoRoute(const std::string& shm_name, const std::string& target_name_substring = "");
    void Stop();

    bool IsRunning() const { return running_.load(std::memory_order_acquire); }
    std::string ActiveEndpointName() const { return active_endpoint_name_; }

private:
    void WorkerLoop(std::string shm_name, std::wstring device_id);

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::thread worker_;
    std::string active_endpoint_name_;
};

}  // namespace ios_vcam
