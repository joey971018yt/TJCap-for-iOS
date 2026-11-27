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
#include <numeric>
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

enum class ResolutionMode {
    Native2KPlus,  // HPD1 3840x3840 -> exact device native pixels & aspect ratio (zero black bars)
    Native1080,    // HPD1 1920x1920 -> 1080p-tier exact device aspect ratio (zero black bars)
    Fixed1440p,    // HPD1 2560x2560 -> letterboxed into 2560x1440 (16:9 2K QHD)
    Fixed1080p,    // HPD1 1920x1920 -> letterboxed into 1920x1080 (16:9 FHD)
};

struct HostOptions {
    bool mock_mode = false;
    bool clean_status_bar = false;
    bool prefer_hw_decode = true;
    bool enable_audio_route = true;
    bool list_audio_only = false;
    ResolutionMode res_mode = ResolutionMode::Native2KPlus;
    std::string preferred_udid;
    std::string audio_out_match;
    std::string stop_event_name;
};

void PrintUsage() {
    std::cout
        << "iOS USB Universal Virtual Capture Card Host (ios_vcam_host.exe)\n"
        << "Usage: ios_vcam_host.exe [options]\n\n"
        << "Options:\n"
        << "  --res <mode>           Resolution & aspect mode (default: native):\n"
        << "                           native     : 2K+ device-native resolution & aspect ratio (zero black bars)\n"
        << "                           native1080 : 1080p-class device-native aspect ratio (zero black bars)\n"
        << "                           1440p      : Fixed 2560x1440 (2K 16:9) letterboxed canvas\n"
        << "                           1080p      : Fixed 1920x1080 (FHD 16:9) letterboxed canvas\n"
        << "  --mock                 Run self-test pipeline (2560x1440@60fps color bars + 48kHz stereo tone)\n"
        << "  --mode <valeria|clean> Status bar mode: 'valeria' (9:41 clean bar, default) or 'clean' (real bar)\n"
        << "  --udid <serial>        Connect to a specific iPhone/iPad serial/UDID substring\n"
        << "  --audio-out <name>     Route 48kHz audio via WASAPI to endpoint matching <name>\n"
        << "  --no-audio-route       Disable automatic WASAPI virtual cable audio routing\n"
        << "  --list-audio           List active Windows audio playback endpoints and exit\n"
        << "  --sw-decode            Force CPU H.264 decoding instead of D3D11VA hardware acceleration\n"
        << "  --stop-event <name>    Win32 named event for graceful GUI stop signaling\n"
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
        } else if (arg == "--res" && i + 1 < argc) {
            std::string r = argv[++i];
            if (r == "native1080") {
                opts.res_mode = ResolutionMode::Native1080;
            } else if (r == "1440p" || r == "2k") {
                opts.res_mode = ResolutionMode::Fixed1440p;
            } else if (r == "1080p" || r == "fhd") {
                opts.res_mode = ResolutionMode::Fixed1080p;
            } else {
                opts.res_mode = ResolutionMode::Native2KPlus;
            }
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
        } else if (arg == "--stop-event" && i + 1 < argc) {
            opts.stop_event_name = argv[++i];
        }
    }
    return true;
}

int RunMockPipeline(SharedCaptureProducer& producer, ResolutionMode res_mode) {
    uint32_t mock_w = (res_mode == ResolutionMode::Fixed1080p || res_mode == ResolutionMode::Native1080)
                          ? 1920U
                          : 2560U;
    uint32_t mock_h = (mock_w == 2560U) ? 1440U : 1080U;
    producer.SetAdvertisedDimensions(mock_w, mock_h);

    std::cout << "[Host] Starting --mock self-test generator (" << mock_w << "x" << mock_h
              << "@60fps NV12 + 48kHz Stereo LPCM)...\n"
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
            GenerateAnimatedColorBarsNV12(dst_nv12, mock_w, mock_h, frame_idx);
            producer.PublishBackbufferSlot(
                slot, mock_w, mock_h, frame_idx * kFrameIntervalNs, mock_w, mock_h, 0);
        }

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
            std::cout << "[Mock] Published " << frame_idx << " frames (" << mock_w << "x" << mock_h
                      << "@60fps, 48kHz Stereo)\n";
        }

        next_tick += std::chrono::nanoseconds(kFrameIntervalNs);
        std::this_thread::sleep_until(next_tick);
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

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

#ifdef _WIN32
    struct ScopedStopEventWatcher {
        HANDLE stop_evt = nullptr;
        HANDLE exit_evt = nullptr;
        std::thread thr;
        ~ScopedStopEventWatcher() {
            if (exit_evt) SetEvent(exit_evt);
            if (thr.joinable()) thr.join();
            if (exit_evt) CloseHandle(exit_evt);
            if (stop_evt) CloseHandle(stop_evt);
        }
    } stop_watcher;

    if (!opts.stop_event_name.empty()) {
        std::wstring wname(opts.stop_event_name.begin(), opts.stop_event_name.end());
        stop_watcher.stop_evt = OpenEventW(SYNCHRONIZE, FALSE, wname.c_str());
        if (stop_watcher.stop_evt) {
            stop_watcher.exit_evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            stop_watcher.thr = std::thread([hStop = stop_watcher.stop_evt, hExit = stop_watcher.exit_evt]() {
                HANDLE handles[2] = {hStop, hExit};
                DWORD rc = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                if (rc == WAIT_OBJECT_0) {
                    g_stop_flag.store(true, std::memory_order_release);
                }
            });
        }
    }
#endif

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

    if (opts.res_mode == ResolutionMode::Fixed1080p) {
        producer.SetAdvertisedDimensions(1920, 1080);
    } else {
        producer.SetAdvertisedDimensions(2560, 1440);
    }

    WasapiAudioRouter audio_router;
    if (opts.enable_audio_route) {
        if (audio_router.StartAutoRoute(kDefaultShmName, opts.audio_out_match)) {
            std::cout << "[Audio] Low-latency WASAPI router active -> \""
                      << audio_router.ActiveEndpointName() << "\"\n";
        } else if (!opts.audio_out_match.empty()) {
            std::cerr << "[Audio] Warning: No playback endpoint matched \"" << opts.audio_out_match << "\".\n";
        } else {
            std::cout << "[Audio] DirectShow 'iOS USB Capture Card (Audio)' active (48kHz LPCM).\n";
        }
    }

    if (opts.mock_mode) {
        int rc = RunMockPipeline(producer, opts.res_mode);
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
        std::cerr << "[USB Error] " << usb_err << "\n";
        audio_router.Stop();
        producer.Close();
#ifdef _WIN32
        timeEndPeriod(1);
#endif
        return EXIT_FAILURE;
    }

    const char* res_label = "2K+ Native Adaptive";
    if (opts.res_mode == ResolutionMode::Native1080) res_label = "1080p Native Adaptive";
    else if (opts.res_mode == ResolutionMode::Fixed1440p) res_label = "Fixed 2K 2560x1440";
    else if (opts.res_mode == ResolutionMode::Fixed1080p) res_label = "Fixed 1080p 1920x1080";

    std::cout << "[Host] Waiting for iPhone / iPad over USB (Resolution: " << res_label
              << ", Status bar: " << (opts.clean_status_bar ? "clean native" : "QuickTime Valeria") << ")...\n";

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
                  << ", 2K+ Adaptive Ready)\n";

        uint64_t decoded_frames = 0;
        uint64_t window_video_bytes = 0;
        uint32_t latest_kbps = 0;
        auto stats_timer = std::chrono::steady_clock::now();
        uint32_t last_src_w = 0, last_src_h = 0;

        QtSessionConfig qcfg;
        qcfg.valeria_clean_status_bar = !opts.clean_status_bar;
        if (opts.res_mode == ResolutionMode::Native2KPlus) {
            qcfg.max_width = 3840;
            qcfg.max_height = 3840;
        } else if (opts.res_mode == ResolutionMode::Fixed1440p) {
            qcfg.max_width = 2560;
            qcfg.max_height = 2560;
        } else {
            qcfg.max_width = 1920;
            qcfg.max_height = 1920;
        }

        QtStreamSession session(
            qcfg,
            [&](std::span<const uint8_t> out_pkt) { return usb.SendBulk(out_pkt); },
            [&](const EncodedVideoPacket& vpkt) {
                window_video_bytes += vpkt.annexb_data.size();

                decoder.DecodeAnnexB(
                    vpkt.annexb_data,
                    vpkt.pts_ns,
                    vpkt.width,
                    vpkt.height,
                    [&](const DecodedNV12View& view) {
                        uint32_t out_w = 0, out_h = 0;
                        if (opts.res_mode == ResolutionMode::Native2KPlus ||
                            opts.res_mode == ResolutionMode::Native1080) {
                            AlignAdaptiveDimensions(view.width, view.height, out_w, out_h);
                        } else if (opts.res_mode == ResolutionMode::Fixed1440p) {
                            out_w = 2560U;
                            out_h = 1440U;
                        } else {
                            out_w = 1920U;
                            out_h = 1080U;
                        }

                        if (view.width != last_src_w || view.height != last_src_h) {
                            last_src_w = view.width;
                            last_src_h = view.height;
                            uint32_t g = std::gcd(last_src_w, last_src_h);
                            double aspect = (last_src_h > 0)
                                                ? (static_cast<double>(last_src_w) / static_cast<double>(last_src_h))
                                                : 1.777;
                            std::printf(
                                "[Video] Device native resolution: %ux%u (ratio %.2f:1, %u:%u) -> Output %ux%u\n",
                                last_src_w,
                                last_src_h,
                                aspect,
                                g ? (last_src_w / g) : last_src_w,
                                g ? (last_src_h / g) : last_src_h,
                                out_w,
                                out_h);
                            std::fflush(stdout);
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
                                out_w,
                                out_h);
                            producer.PublishBackbufferSlot(
                                slot, out_w, out_h, view.pts_ns, view.width, view.height, latest_kbps);
                        }

                        ++decoded_frames;
                        auto now = std::chrono::steady_clock::now();
                        double elapsed = std::chrono::duration<double>(now - stats_timer).count();
                        if (elapsed >= 4.0) {
                            double fps = static_cast<double>(decoded_frames) / elapsed;
                            double mbps = (static_cast<double>(window_video_bytes) * 8.0) / (elapsed * 1e6);
                            latest_kbps = static_cast<uint32_t>(mbps * 1000.0);
                            std::printf(
                                "[Stream] %.1f fps | Bitrate: %.1f Mbps | Output: %ux%u (Source %ux%u) | Audio pkts: %llu\n",
                                fps,
                                mbps,
                                out_w,
                                out_h,
                                last_src_w,
                                last_src_h,
                                static_cast<unsigned long long>(session.AudioPacketsReceived()));
                            std::fflush(stdout);
                            decoded_frames = 0;
                            window_video_bytes = 0;
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
