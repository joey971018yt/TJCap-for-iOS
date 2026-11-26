#include "ios_vcam/coremedia_codec.h"
#include "ios_vcam/image_convert.h"
#include "ios_vcam/mf_h264_decoder.h"
#include "ios_vcam/qt_session.h"
#include "ios_vcam/shared_ipc.h"
#include "ios_vcam/usb_ios_transport.h"
#include "ios_vcam/wasapi_renderer.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

namespace {

using namespace ios_vcam;

std::atomic<bool> g_stop_flag{false};

#ifdef _WIN32
BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT || ctrl_type == CTRL_CLOSE_EVENT) {
        g_stop_flag.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}
#else
void SignalHandler(int) {
    g_stop_flag.store(true, std::memory_order_release);
}
#endif

struct HostOptions {
    bool mock_mode = false;
    bool clean_status_bar = false;  // --mode clean preserves real iPhone time/battery bar
    bool prefer_hw_decode = true;
    bool enable_audio_route = true;
    bool list_audio_only = false;
    std::string preferred_udid;
    std::string audio_out_match;
};

void PrintUsage() {
    std::cout
        << "iOS USB Universal Virtual Capture Card Host (ios_vcam_host.exe)\n"
        << "Usage: ios_vcam_host.exe [options]\n\n"
        << "Options:\n"
        << "  --mock                 Run self-test pipeline (1080p60 color bars + 48kHz stereo test tone)\n"
        << "  --mode <valeria|clean> Status bar mode: 'valeria' (9:41 clean bar, default) or 'clean' (real bar)\n"
        << "  --udid <serial>        Connect to a specific iPhone/iPad serial/UDID substring\n"
        << "  --audio-out <name>     Route 48kHz audio via WASAPI to endpoint matching <name>\n"
        << "                         (auto-routes to VB-Audio 'CABLE Input' if installed)\n"
        << "  --no-audio-route       Disable automatic WASAPI virtual cable audio routing\n"
        << "  --list-audio           List active Windows audio playback endpoints and exit\n"
        << "  --sw-decode            Force CPU H.264 decoding instead of D3D11VA hardware acceleration\n"
        << "  --help                 Show this help message\n";
}

bool ParseArgs(int argc, char** argv, HostOptions& opts) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return false;
        } else if (arg == "--mock") {
            opts.mock_mode = true;
        } else if (arg == "--mode" && i + 1 < argc) {
            std::string m = argv[++i];
            opts.clean_status_bar = (m == "clean" || m == "native");
        } else if (arg == "--udid" && i + 1 < argc) {
            opts.preferred_udid = argv[++i];
        } else if (arg == "--audio-out" && i + 1 < argc) {
            opts.audio_out_match = argv[++i];
        } else if (arg == "--no-audio-route") {
            opts.enable_audio_route = false;
        } else if (arg == "--list-audio") {
            opts.list_audio_only = true;
        } else if (arg == "--sw-decode") {
            opts.prefer_hw_decode = false;
        }
    }
    return true;
}

int RunMockPipeline(SharedCaptureProducer& producer) {
    std::cout << "[Host] Starting --mock self-test generator (1920x1080@60fps NV12 + 48kHz Stereo LPCM)...\n"
              << "[Host] Open OBS / Discord / Zoom and select 'iOS USB Capture Card (Video)'!\n"
              << "[Host] Press Ctrl+C to stop.\n";

    constexpr uint64_t kFrameIntervalNs = 16666667ULL;  // 60.00 fps
    constexpr size_t kAudioFramesPerVideoFrame = 800;   // 48000 / 60 = 800 stereo frames
    std::vector<int16_t> pcm_chunk(kAudioFramesPerVideoFrame * 2);

    uint64_t frame_idx = 0;
    double phase_l = 0.0;
    double phase_r = 0.0;
    constexpr double kTwoPi = 6.28318530717958647692;

    auto next_tick = std::chrono::steady_clock::now();

    while (!g_stop_flag.load(std::memory_order_relaxed)) {
        uint32_t slot = 0;
        uint8_t* dst_nv12 = producer.AcquireBackbufferSlot(slot);
        if (dst_nv12) {
            GenerateAnimatedColorBarsNV12(dst_nv12, kCanvasWidth, kCanvasHeight, frame_idx);
            producer.PublishBackbufferSlot(slot, frame_idx * kFrameIntervalNs, kCanvasWidth, kCanvasHeight);
        }

        // Gentle -18 dBFS stereo sine chord (440 Hz Left, 554.37 Hz Right)
        for (size_t i = 0; i < kAudioFramesPerVideoFrame; ++i) {
            pcm_chunk[i * 2] = static_cast<int16_t>(std::sin(phase_l) * 4000.0);
            pcm_chunk[i * 2 + 1] = static_cast<int16_t>(std::sin(phase_r) * 4000.0);
            phase_l += kTwoPi * 440.0 / 48000.0;
            phase_r += kTwoPi * 554.365 / 48000.0;
            if (phase_l > kTwoPi) phase_l -= kTwoPi;
            if (phase_r > kTwoPi) phase_r -= kTwoPi;
        }
        producer.PushAudioPcmS16LE(pcm_chunk.data(), kAudioFramesPerVideoFrame);

        ++frame_idx;
        if (frame_idx % 300 == 0) {
            std::cout << "[Mock] Published " << frame_idx << " frames (1920x1080@60fps, 48kHz Stereo)\n";
        }

        next_tick += std::chrono::nanoseconds(kFrameIntervalNs);
        std::this_thread::sleep_until(next_tick);
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    timeBeginPeriod(1);
#else
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);
#endif

    HostOptions opts;
    if (!ParseArgs(argc, argv, opts)) {
#ifdef _WIN32
        timeEndPeriod(1);
#endif
        return EXIT_SUCCESS;
    }

    if (opts.list_audio_only) {
        auto endpoints = WasapiAudioRouter::EnumerateRenderEndpoints();
        std::cout << "Active Windows Audio Render Endpoints (" << endpoints.size() << "):\n";
        for (const auto& ep : endpoints) {
            std::cout << "  - " << ep.friendly_name_utf8 << "\n";
        }
#ifdef _WIN32
        timeEndPeriod(1);
#endif
        return EXIT_SUCCESS;
    }

    SharedCaptureProducer producer;
    if (!producer.Open(kDefaultShmName)) {
        std::cerr << "[Error] Failed to create shared memory IPC (" << kDefaultShmName << ").\n";
#ifdef _WIN32
        timeEndPeriod(1);
#endif
        return EXIT_FAILURE;
    }

    WasapiAudioRouter audio_router;
    if (opts.enable_audio_route) {
        if (audio_router.StartAutoRoute(kDefaultShmName, opts.audio_out_match)) {
            std::cout << "[Audio] Low-latency WASAPI router active -> \""
                      << audio_router.ActiveEndpointName() << "\"\n";
        } else if (!opts.audio_out_match.empty()) {
            std::cerr << "[Audio] Warning: No playback endpoint matched \"" << opts.audio_out_match << "\".\n";
        } else {
            std::cout << "[Audio] DirectShow 'iOS USB Capture Card (Audio)' active. "
                         "(Install VB-Audio Virtual Cable if you also want kernel mic routing.)\n";
        }
    }

    if (opts.mock_mode) {
        int rc = RunMockPipeline(producer);
        audio_router.Stop();
        producer.Close();
#ifdef _WIN32
        timeEndPeriod(1);
#endif
        return rc;
    }

    UsbIosTransport usb;
    std::string usb_err;
    if (!usb.Initialize(&usb_err)) {
        std::cerr << "[USB Error] " << usb_err << "\n"
                  << "Tip: Run 'ios_vcam_host.exe --mock' to test the virtual capture card without an iPhone.\n";
        audio_router.Stop();
        producer.Close();
#ifdef _WIN32
        timeEndPeriod(1);
#endif
        return EXIT_FAILURE;
    }

    std::cout << "[Host] Waiting for iPhone / iPad over USB (Status bar mode: "
              << (opts.clean_status_bar ? "clean native" : "QuickTime Valeria") << ")...\n"
              << "[Host] Press Ctrl+C at any time to stop and restore standard USBMux mode.\n";

    while (!g_stop_flag.load(std::memory_order_relaxed)) {
        producer.TouchHeartbeat(false);

        UsbTransportConfig ucfg;
        ucfg.preferred_udid = opts.preferred_udid;
        usb_err.clear();
        if (!usb.OpenAndClaimQuickTimeInterface(ucfg, &usb_err)) {
            static auto last_log = std::chrono::steady_clock::now() - std::chrono::seconds(10);
            auto now = std::chrono::steady_clock::now();
            if (now - last_log >= std::chrono::seconds(4)) {
                std::cout << "[USB] " << usb_err << "\n";
                last_log = now;
            }
            for (int i = 0; i < 15 && !g_stop_flag.load(std::memory_order_relaxed); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        std::cout << "[USB] Connected: " << usb.ConnectedDeviceDescription() << "\n";

        MfH264Decoder decoder;
        std::string dec_err;
        if (!decoder.Initialize(opts.prefer_hw_decode, &dec_err)) {
            std::cerr << "[Decoder Error] " << dec_err << "\n";
            usb.CloseAndRestoreUsbMux();
            break;
        }
        std::cout << "[Decoder] Windows Media Foundation H.264 MFT initialized ("
                  << (decoder.IsHardwareAccelerated() ? "D3D11VA Hardware GPU" : "CPU Software Fallback")
                  << ", Low-Latency ON)\n";

        uint64_t decoded_frames = 0;
        auto stats_timer = std::chrono::steady_clock::now();
        uint32_t last_src_w = 0, last_src_h = 0;

        QtSessionConfig qcfg;
        qcfg.valeria_clean_status_bar = !opts.clean_status_bar;

        QtStreamSession session(
            qcfg,
            [&](std::span<const uint8_t> out_pkt) { return usb.SendBulk(out_pkt); },
            [&](const EncodedVideoPacket& vpkt) {
                decoder.DecodeAnnexB(
                    vpkt.annexb_data,
                    vpkt.pts_ns,
                    vpkt.width,
                    vpkt.height,
                    [&](const DecodedNV12View& view) {
                        if (view.width != last_src_w || view.height != last_src_h) {
                            last_src_w = view.width;
                            last_src_h = view.height;
                            std::cout << "[Video] Source stream resolution: " << last_src_w << "x" << last_src_h
                                      << " -> Letterboxed to 1920x1080 NV12\n";
                        }
                        uint32_t slot = 0;
                        uint8_t* dst_nv12 = producer.AcquireBackbufferSlot(slot);
                        if (dst_nv12) {
                            ScaleAndLetterboxNV12(
                                view.y_plane,
                                view.y_stride,
                                view.uv_plane,
                                view.uv_stride,
                                view.width,
                                view.height,
                                dst_nv12,
                                kCanvasWidth,
                                kCanvasHeight);
                            producer.PublishBackbufferSlot(slot, view.pts_ns, view.width, view.height);
                        }
                        ++decoded_frames;
                        auto now = std::chrono::steady_clock::now();
                        double elapsed = std::chrono::duration<double>(now - stats_timer).count();
                        if (elapsed >= 5.0) {
                            double fps = static_cast<double>(decoded_frames) / elapsed;
                            std::printf(
                                "[Stream] %.1f fps | Input %ux%u -> 1920x1080 | Audio pkts: %llu\n",
                                fps,
                                last_src_w,
                                last_src_h,
                                static_cast<unsigned long long>(session.AudioPacketsReceived()));
                            std::fflush(stdout);
                            decoded_frames = 0;
                            stats_timer = now;
                        }
                    });
            },
            [&](const DecodedPcmPacket& apkt) {
                size_t stereo_frames = apkt.pcm_s16le.size() / kAudioBytesPerFrame;
                if (stereo_frames > 0) {
                    producer.PushAudioPcmS16LE(
                        reinterpret_cast<const int16_t*>(apkt.pcm_s16le.data()), stereo_frames);
                }
            });

        std::string loop_err;
        usb.RunBulkReadLoop(
            g_stop_flag,
            [&](std::span<const uint8_t> usb_chunk) { return session.FeedUsbBytes(usb_chunk); },
            &loop_err);

        // Send graceful HPA0 + HPD0 stop commands if shutting down while device is still attached
        for (const auto& spkt : session.BuildStopPackets()) {
            usb.SendBulk(spkt);
        }

        decoder.Shutdown();
        usb.CloseAndRestoreUsbMux();

        if (!g_stop_flag.load(std::memory_order_relaxed)) {
            std::cout << "[USB] Device disconnected (" << loop_err << "). Waiting for reconnect...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
        }
    }

    std::cout << "[Host] Shutting down cleanly and restoring iPhone standard USBMux configuration.\n";
    audio_router.Stop();
    producer.Close();
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    return EXIT_SUCCESS;
}
