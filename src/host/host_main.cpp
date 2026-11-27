#include "ios_vcam/coremedia_codec.h"
#include "ios_vcam/diagnostics.h"
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
    NativeAdaptive,  // Device-native aspect ratio clamped to session max_dim (zero black bars)
    Fixed1440p,      // Fixed 2560x1440 (16:9 2K QHD) letterboxed canvas
    Fixed1080p,      // Fixed 1920x1080 (16:9 FHD) letterboxed canvas
    Fixed720p,       // Fixed 1280x720  (16:9 HD)  letterboxed canvas
};

struct HostOptions {
    bool mock_mode = false;
    bool clean_status_bar = false;
    bool prefer_hw_decode = true;
    bool enable_audio_route = true;
    bool list_audio_only = false;
    ResolutionMode res_mode = ResolutionMode::NativeAdaptive;
    uint32_t explicit_max_dim = 0;  // 0 = determine once from USB port speed before session starts
    std::string preferred_udid;
    std::string audio_out_match;
    std::string stop_event_name;
};

void PrintUsage() {
    std::cout
        << "TJCap for iOS — Core Capture Host (ios_vcam_host.exe)\n"
        << "Usage: ios_vcam_host.exe [options]\n\n"
        << "Options:\n"
        << "  --res <mode>           Resolution & aspect mode (default: native):\n"
        << "                           native     : Device-native aspect ratio (zero black bars)\n"
        << "                           native1080 : 1080p-capped device-native aspect ratio\n"
        << "                           1440p      : Fixed 2560x1440 (2K 16:9) letterboxed canvas\n"
        << "                           1080p      : Fixed 1920x1080 (FHD 16:9) letterboxed canvas\n"
        << "                           720p       : Fixed 1280x720  (HD 16:9) letterboxed canvas\n"
        << "  --max-dim <px>         Lock max transmission long-edge (960 / 1280 / 1920 / 2560 / 3840)\n"
        << "  --mock                 Run self-test pipeline (color bars + 48kHz stereo tone)\n"
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
                opts.res_mode = ResolutionMode::NativeAdaptive;
                if (opts.explicit_max_dim == 0) opts.explicit_max_dim = 1920U;
            } else if (r == "1440p" || r == "2k") {
                opts.res_mode = ResolutionMode::Fixed1440p;
            } else if (r == "1080p" || r == "fhd") {
                opts.res_mode = ResolutionMode::Fixed1080p;
            } else if (r == "720p" || r == "hd") {
                opts.res_mode = ResolutionMode::Fixed720p;
            } else {
                opts.res_mode = ResolutionMode::NativeAdaptive;
            }
        } else if (arg == "--max-dim" && i + 1 < argc) {
            int v = std::atoi(argv[++i]);
            if (v >= 320 && v <= 3840) {
                opts.explicit_max_dim = static_cast<uint32_t>(v);
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

int RunMockPipeline(SharedCaptureProducer& producer, const HostOptions& opts) {
    uint32_t mock_w = 2560U, mock_h = 1440U;
    if (opts.res_mode == ResolutionMode::Fixed720p || (opts.explicit_max_dim > 0 && opts.explicit_max_dim <= 1280U)) {
        mock_w = 1280U;
        mock_h = 720U;
    } else if (opts.res_mode == ResolutionMode::Fixed1080p ||
               (opts.explicit_max_dim > 0 && opts.explicit_max_dim <= 1920U)) {
        mock_w = 1920U;
        mock_h = 1080U;
    }
    producer.SetAdvertisedDimensions(mock_w, mock_h);

    std::cout << "[Host] Starting --mock self-test generator (" << mock_w << "x" << mock_h
              << "@60fps NV12 + 48kHz Stereo LPCM)...\n"
              << "[Host] Open OBS / Discord / Zoom and select 'TJCap for iOS (Video)'!\n"
              << "[Host] Press Ctrl+C to stop.\n";

    constexpr uint64_t kFrameIntervalNs = 16666667ULL;  // 60.00 fps
    constexpr size_t kAudioFramesPerVideoFrame = 800;   // 48000 / 60 = 800 stereo frames
    std::vector<int16_t> pcm_chunk(kAudioFramesPerVideoFrame * 2);

    uint64_t frame_idx = 0;
    double phase_l = 0.0;
    double phase_r = 0.0;
    constexpr double kTwoPi = 6.28318530717958647692;
    constexpr uint64_t kMockSynthBytesPerFrame = 37500ULL;  // Simulated 18.0 Mbps @ 60fps test stream
    uint32_t ema_pipe_us = 450U;

    auto next_tick = std::chrono::steady_clock::now();

    while (!g_stop_flag.load(std::memory_order_relaxed)) {
        auto t0 = std::chrono::steady_clock::now();
        uint32_t slot = 0;
        uint8_t* dst_nv12 = producer.AcquireBackbufferSlot(slot);
        if (dst_nv12) {
            GenerateAnimatedColorBarsNV12(dst_nv12, mock_w, mock_h, frame_idx);
            auto t1 = std::chrono::steady_clock::now();
            uint32_t cur_us = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
            ema_pipe_us = (frame_idx == 0) ? cur_us : ((ema_pipe_us * 3U + cur_us) / 4U);

            producer.AddEncodedVideoBytes(kMockSynthBytesPerFrame);
            FrameTelemetryMeta meta{};
            meta.fps_x100 = 6000U;
            meta.decode_latency_us = ema_pipe_us / 2U;
            meta.pipeline_latency_us = std::max(100U, ema_pipe_us);
            meta.stream_flags = kStreamFlagHwAccelerated | kStreamFlagMockSource | kStreamFlagAdaptiveRes;
            meta.dropped_frames = 0U;

            producer.PublishBackbufferSlot(
                slot, mock_w, mock_h, frame_idx * kFrameIntervalNs, mock_w, mock_h, 18000U, meta);
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
        producer.SetAudioPacketCount(frame_idx + 1ULL);

        ++frame_idx;
        if (frame_idx % 300 == 0) {
            std::cout << "[Mock] Published " << frame_idx << " frames (" << mock_w << "x" << mock_h
                      << "@60fps, Latency: " << (ema_pipe_us / 1000.0) << " ms, 48kHz Stereo)\n";
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
    SetDllDirectoryW(L"");
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
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
        std::cerr << FormatHostDiagLine(
                         DiagCode::E502_SharedMemInitFailed,
                         DiagSeverity::Critical,
                         "無法建立跨行程共享記憶體 (" + std::string(kDefaultShmName) + ")。")
                  << "\n";
#ifdef _WIN32
        timeEndPeriod(1);
#endif
        return EXIT_FAILURE;
    }

    if (opts.res_mode == ResolutionMode::Fixed720p || (opts.explicit_max_dim > 0 && opts.explicit_max_dim <= 1280U)) {
        producer.SetAdvertisedDimensions(1280, 720);
    } else if (opts.res_mode == ResolutionMode::Fixed1080p ||
               (opts.explicit_max_dim > 0 && opts.explicit_max_dim <= 1920U)) {
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
            std::cout << "[Audio] DirectShow 'TJCap for iOS (Audio)' active (48kHz LPCM).\n";
        }
    }

    if (opts.mock_mode) {
        int rc = RunMockPipeline(producer, opts);
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
        DiagCode ucode = ClassifyUsbOrTransportError(usb_err);
        if (ucode == DiagCode::None) ucode = DiagCode::E101_MissingCoreFiles;
        std::cerr << FormatHostDiagLine(ucode, DiagSeverity::Critical, usb_err) << "\n";
        audio_router.Stop();
        producer.Close();
#ifdef _WIN32
        timeEndPeriod(1);
#endif
        return EXIT_FAILURE;
    }

    const char* res_label = "Adaptive Native";
    if (opts.res_mode == ResolutionMode::Fixed1440p) res_label = "Fixed 2K 2560x1440";
    else if (opts.res_mode == ResolutionMode::Fixed1080p) res_label = "Fixed 1080p 1920x1080";
    else if (opts.res_mode == ResolutionMode::Fixed720p) res_label = "Fixed 720p 1280x720";

    std::cout << "[Host] Waiting for iPhone / iPad over USB (Mode: " << res_label;
    if (opts.explicit_max_dim > 0) {
        std::cout << ", Locked Max Edge: " << opts.explicit_max_dim << "px";
    }
    std::cout << ", Status bar: " << (opts.clean_status_bar ? "clean native" : "QuickTime Valeria") << ")...\n";

    DiagCode last_open_diag = DiagCode::None;

    while (!g_stop_flag.load(std::memory_order_relaxed)) {
        producer.TouchHeartbeat(false);

        UsbTransportConfig ucfg;
        ucfg.preferred_udid = opts.preferred_udid;
        usb_err.clear();
        if (!usb.OpenAndClaimQuickTimeInterface(ucfg, &usb_err)) {
            static auto last_log = std::chrono::steady_clock::now() - std::chrono::seconds(10);
            auto now = std::chrono::steady_clock::now();
            DiagCode cur_diag = ClassifyUsbOrTransportError(usb_err);
            if (cur_diag != DiagCode::None && cur_diag != last_open_diag) {
                last_open_diag = cur_diag;
                DiagSeverity sev = (cur_diag == DiagCode::E301_IosNotTrustedOrLocked)
                                       ? DiagSeverity::Warning
                                       : DiagSeverity::Error;
                std::cout << FormatHostDiagLine(cur_diag, sev, usb_err) << "\n";
                last_log = now;
            } else if (now - last_log >= std::chrono::seconds(5)) {
                std::cout << "[USB] " << usb_err << "\n";
                last_log = now;
            }
            for (int i = 0; i < 15 && !g_stop_flag.load(std::memory_order_relaxed); ++i) {
                producer.TouchHeartbeat(false);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }
        last_open_diag = DiagCode::None;

        // Determine maximum resolution ceiling ONCE at session start (never auto-modified during streaming!)
        const UsbLinkSpeed conn_speed = usb.ConnectedLinkSpeed();
        const uint32_t locked_max_dim =
            (opts.explicit_max_dim > 0)
                ? opts.explicit_max_dim
                : MaxSelectableDimForUsbSpeed(conn_speed);

        std::cout << "[USB] Connected: " << usb.ConnectedDeviceDescription()
                  << " | Locked Max Resolution Edge: " << locked_max_dim << "px\n";

        MfH264Decoder decoder;
        std::string dec_err;
        if (!decoder.Initialize(opts.prefer_hw_decode, &dec_err)) {
            if (opts.prefer_hw_decode && decoder.Initialize(false, &dec_err)) {
                std::cout << FormatHostDiagLine(
                                 DiagCode::E402_GpuFallbackToSoftware,
                                 DiagSeverity::Warning,
                                 "GPU D3D11VA 硬體解碼初始化失敗，已自動切換為 CPU 軟體解碼模式。")
                          << "\n";
            } else {
                std::cerr << FormatHostDiagLine(DiagCode::E401_DecoderInitFailed, DiagSeverity::Critical, dec_err)
                          << "\n";
                usb.CloseAndRestoreUsbMux();
                audio_router.Stop();
                producer.Close();
#ifdef _WIN32
                timeEndPeriod(1);
#endif
                return EXIT_FAILURE;
            }
        }
        std::cout << "[Decoder] Windows Media Foundation H.264 MFT initialized ("
                  << (decoder.IsHardwareAccelerated() ? "D3D11VA Hardware GPU" : "CPU Software Fallback")
                  << ")\n";

        std::atomic<uint32_t> session_flags{
            (decoder.IsHardwareAccelerated() ? kStreamFlagHwAccelerated : 0U) |
            ((opts.res_mode == ResolutionMode::NativeAdaptive) ? kStreamFlagAdaptiveRes : 0U) |
            ((static_cast<uint32_t>(conn_speed) & 0xFU) << 8)};

        std::atomic<bool> got_any_frame{false};
        std::atomic<bool> session_done{false};
        std::thread session_watchdog([&]() {
            auto start_tp = std::chrono::steady_clock::now();
            bool warned_no_frames = false;
            while (!session_done.load(std::memory_order_acquire) &&
                   !g_stop_flag.load(std::memory_order_relaxed)) {
                producer.TouchHeartbeat(got_any_frame.load(std::memory_order_acquire));
                if (!warned_no_frames && !got_any_frame.load(std::memory_order_acquire)) {
                    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_tp).count();
                    if (sec >= 8.5) {
                        warned_no_frames = true;
                        std::cout << FormatHostDiagLine(
                                         DiagCode::E303_HandshakeNoFrames,
                                         DiagSeverity::Warning,
                                         "USB 介面已連通但超過 8 秒未收到畫面封包：請確認手機已解鎖亮屏並點擊「信任」。")
                                  << "\n";
                    }
                }
                for (int k = 0; k < 5 && !session_done.load(std::memory_order_acquire) &&
                                !g_stop_flag.load(std::memory_order_relaxed);
                     ++k) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        });

        uint64_t console_frames = 0;
        uint64_t console_video_bytes = 0;
        auto console_timer = std::chrono::steady_clock::now();

        uint64_t fast_frames = 0;
        uint64_t fast_video_bytes = 0;
        auto fast_timer = console_timer;

        uint32_t latest_kbps = 0;
        uint32_t latest_fps_x100 = 6000U;
        uint32_t ema_dec_us = 0;
        uint32_t ema_pipe_us = 0;
        uint32_t decode_drops = 0;
        uint32_t consecutive_dec_failures = 0;
        bool warned_high_latency = false;
        bool latency_seeded = false;
        uint32_t last_src_w = 0, last_src_h = 0;

        QtSessionConfig qcfg;
        qcfg.valeria_clean_status_bar = !opts.clean_status_bar;
        if (opts.res_mode == ResolutionMode::Fixed1440p) {
            qcfg.max_width = std::min<uint32_t>(2560U, locked_max_dim);
            qcfg.max_height = qcfg.max_width;
        } else if (opts.res_mode == ResolutionMode::Fixed1080p) {
            qcfg.max_width = std::min<uint32_t>(1920U, locked_max_dim);
            qcfg.max_height = qcfg.max_width;
        } else if (opts.res_mode == ResolutionMode::Fixed720p) {
            qcfg.max_width = 1280U;
            qcfg.max_height = 1280U;
        } else {
            qcfg.max_width = locked_max_dim;
            qcfg.max_height = locked_max_dim;
        }

        QtStreamSession session(
            qcfg,
            [&](std::span<const uint8_t> out_pkt) { return usb.SendBulk(out_pkt); },
            [&](const EncodedVideoPacket& vpkt) {
                auto pkt_t0 = std::chrono::steady_clock::now();
                producer.AddEncodedVideoBytes(vpkt.annexb_data.size());
                console_video_bytes += vpkt.annexb_data.size();
                fast_video_bytes += vpkt.annexb_data.size();

                bool emitted_frame = false;
                bool dec_ok = decoder.DecodeAnnexB(
                    vpkt.annexb_data,
                    vpkt.pts_ns,
                    vpkt.width,
                    vpkt.height,
                    [&](const DecodedNV12View& view) {
                        emitted_frame = true;
                        got_any_frame.store(true, std::memory_order_release);
                        consecutive_dec_failures = 0;

                        auto dec_t1 = std::chrono::steady_clock::now();
                        uint32_t cur_dec_us = static_cast<uint32_t>(
                            std::chrono::duration_cast<std::chrono::microseconds>(dec_t1 - pkt_t0).count());

                        uint32_t out_w = 0, out_h = 0;
                        if (opts.res_mode == ResolutionMode::NativeAdaptive) {
                            ClampAdaptiveDimensionsToMax(view.width, view.height, locked_max_dim, out_w, out_h);
                        } else if (opts.res_mode == ResolutionMode::Fixed1440p) {
                            out_w = 2560U;
                            out_h = 1440U;
                        } else if (opts.res_mode == ResolutionMode::Fixed720p) {
                            out_w = 1280U;
                            out_h = 720U;
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

                        ++console_frames;
                        ++fast_frames;
                        auto now = std::chrono::steady_clock::now();
                        double fast_dt = std::chrono::duration<double>(now - fast_timer).count();
                        if (fast_dt >= 0.45) {
                            double inst_fps = static_cast<double>(fast_frames) / fast_dt;
                            double inst_mbps = (static_cast<double>(fast_video_bytes) * 8.0) / (fast_dt * 1e6);
                            latest_fps_x100 = static_cast<uint32_t>(inst_fps * 100.0 + 0.5);
                            latest_kbps = static_cast<uint32_t>(inst_mbps * 1000.0 + 0.5);
                            fast_frames = 0;
                            fast_video_bytes = 0;
                            fast_timer = now;
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
                            auto pipe_t1 = std::chrono::steady_clock::now();
                            uint32_t cur_pipe_us = static_cast<uint32_t>(
                                std::chrono::duration_cast<std::chrono::microseconds>(pipe_t1 - pkt_t0).count());

                            if (!latency_seeded) {
                                ema_dec_us = cur_dec_us;
                                ema_pipe_us = cur_pipe_us;
                                latency_seeded = true;
                            } else {
                                ema_dec_us = (ema_dec_us * 3U + cur_dec_us) / 4U;
                                ema_pipe_us = (ema_pipe_us * 3U + cur_pipe_us) / 4U;
                            }

                            if (!warned_high_latency && console_frames > 90 && ema_pipe_us > 17500U) {
                                warned_high_latency = true;
                                char lbuf[160] = {0};
                                std::snprintf(
                                    lbuf,
                                    sizeof(lbuf),
                                    "解碼管線平均耗時 %.1f ms 超過 60fps 預算 (16.7 ms)，建議確認開啟 GPU D3D11VA 硬解或調降解析度上限。",
                                    ema_pipe_us / 1000.0);
                                std::cout << FormatHostDiagLine(
                                                 DiagCode::E403_HighDecodeLatency, DiagSeverity::Warning, lbuf)
                                          << "\n";
                            }

                            FrameTelemetryMeta meta{};
                            meta.fps_x100 = latest_fps_x100;
                            meta.decode_latency_us = ema_dec_us;
                            meta.pipeline_latency_us = ema_pipe_us;
                            meta.stream_flags = session_flags.load(std::memory_order_relaxed);
                            meta.dropped_frames = decode_drops;

                            producer.PublishBackbufferSlot(
                                slot, out_w, out_h, view.pts_ns, view.width, view.height, latest_kbps, meta);
                        }

                        double elapsed = std::chrono::duration<double>(now - console_timer).count();
                        if (elapsed >= 4.0) {
                            double fps = static_cast<double>(console_frames) / elapsed;
                            double mbps = (static_cast<double>(console_video_bytes) * 8.0) / (elapsed * 1e6);
                            std::printf(
                                "[Stream] %.1f fps | Bitrate: %.1f Mbps | Decode: %.2f ms (Total %.2f ms) | Output: %ux%u (Source %ux%u) | Audio pkts: %llu\n",
                                fps,
                                mbps,
                                ema_dec_us / 1000.0,
                                ema_pipe_us / 1000.0,
                                out_w,
                                out_h,
                                last_src_w,
                                last_src_h,
                                static_cast<unsigned long long>(session.AudioPacketsReceived()));
                            std::fflush(stdout);
                            console_frames = 0;
                            console_video_bytes = 0;
                            console_timer = now;
                        }
                    });

                if (!dec_ok) {
                    ++decode_drops;
                    ++consecutive_dec_failures;
                    if (decoder.IsHardwareAccelerated() && consecutive_dec_failures >= 15U) {
                        decoder.Shutdown();
                        std::string fb_err;
                        if (decoder.Initialize(false, &fb_err)) {
                            consecutive_dec_failures = 0;
                            uint32_t prev_f = session_flags.load(std::memory_order_relaxed);
                            session_flags.store(prev_f & ~kStreamFlagHwAccelerated, std::memory_order_release);
                            std::cout << FormatHostDiagLine(
                                             DiagCode::E402_GpuFallbackToSoftware,
                                             DiagSeverity::Warning,
                                             "偵測到顯示卡 D3D11VA 硬體解碼連續異常，已自動熱切換至 CPU 軟體解碼維持直播不中斷！")
                                      << "\n";
                        }
                    }
                } else if (emitted_frame) {
                    consecutive_dec_failures = 0;
                }
            },
            [&](const DecodedPcmPacket& apkt) {
                size_t stereo_frames = apkt.pcm_s16le.size() / kAudioBytesPerFrame;
                if (stereo_frames > 0) {
                    producer.PushAudioPcmS16LE(
                        reinterpret_cast<const int16_t*>(apkt.pcm_s16le.data()), stereo_frames);
                    producer.SetAudioPacketCount(session.AudioPacketsReceived());
                }
            });

        std::string loop_err;
        usb.RunBulkReadLoop(
            g_stop_flag,
            [&](std::span<const uint8_t> usb_chunk) { return session.FeedUsbBytes(usb_chunk); },
            &loop_err);

        session_done.store(true, std::memory_order_release);
        if (session_watchdog.joinable()) {
            session_watchdog.join();
        }

        for (const auto& spkt : session.BuildStopPackets()) {
            usb.SendBulk(spkt);
        }

        decoder.Shutdown();
        usb.CloseAndRestoreUsbMux();

        if (!g_stop_flag.load(std::memory_order_relaxed)) {
            DiagCode drop_code = ClassifyUsbOrTransportError(loop_err);
            if (drop_code != DiagCode::None) {
                std::cout << FormatHostDiagLine(
                                 drop_code,
                                 DiagSeverity::Warning,
                                 "USB 裝置連線中斷 (" + loop_err + ")，正在自動等待重連...")
                          << "\n";
            } else {
                std::cout << "[USB] Device disconnected (" << loop_err << "). Waiting for reconnect...\n";
            }
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
