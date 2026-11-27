#include "ios_vcam/app_updater.h"
#include "ios_vcam/coremedia_codec.h"
#include "ios_vcam/diagnostics.h"
#include "ios_vcam/image_convert.h"
#include "ios_vcam/qt_session.h"
#include "ios_vcam/screenshot_util.h"
#include "ios_vcam/shared_ipc.h"
#include "ios_vcam/usb_ios_transport.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

using namespace ios_vcam;

#define ASSERT_TRUE(cond, msg)                                                           \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::cerr << "[FAIL] " << __FUNCTION__ << ":" << __LINE__ << " - " << (msg)  \
                      << std::endl;                                                      \
            return false;                                                                \
        }                                                                                \
    } while (0)

std::vector<uint8_t> HexToBytes(const char* hex) {
    std::vector<uint8_t> out;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int hi = -1;
    for (const char* p = hex; *p != '\0'; ++p) {
        int v = nibble(*p);
        if (v < 0) continue;
        if (hi < 0) {
            hi = v;
        } else {
            out.push_back(static_cast<uint8_t>((hi << 4) | v));
            hi = -1;
        }
    }
    return out;
}

bool TestExactQuickTimeHexVectors() {
    auto expected_ping = HexToBytes("10000000 676E6970 00000000 01000000");
    auto actual_ping = CoreMediaCodec::BuildPingPacket();
    ASSERT_TRUE(actual_ping == expected_ping, "PING packet hex mismatch");

    auto expected_valeria_dict = HexToBytes(
        "28000000 74636964 20000000 7679656B 0F000000 6B727473 56616C65 726961 09000000 766C7562 01");
    StringDict d;
    d.entries.push_back({"Valeria", true});
    auto actual_valeria_dict = CoreMediaCodec::SerializeStringDict(d);
    ASSERT_TRUE(actual_valeria_dict == expected_valeria_dict, "Valeria dictionary TLV hex mismatch");

    auto parsed = CoreMediaCodec::ParseStringDict(actual_valeria_dict);
    ASSERT_TRUE(parsed.has_value() && parsed->entries.size() == 1, "Failed to parse Valeria dict");
    ASSERT_TRUE(parsed->entries[0].key == "Valeria" && std::get<bool>(parsed->entries[0].value) == true,
                "Parsed Valeria entry mismatch");

    auto expected_afmt_rply = HexToBytes(
        "3E000000 796C7072 809D2213 01000000 00000000 2A000000 74636964 "
        "22000000 7679656B 0D000000 6B727473 4572726F 720D0000 0076626D 6E030000 0000");
    uint64_t corr_id = CoreMediaCodec::ReadU64LE(HexToBytes("809D2213 01000000"), 0);
    auto actual_afmt_rply = CoreMediaCodec::BuildRplyAfmtOk(corr_id);
    ASSERT_TRUE(actual_afmt_rply == expected_afmt_rply, "AFMT RPLY packet hex mismatch");

    auto expected_need = HexToBytes("14000000 6E797361 A08D5313 01000000 6465656E");
    uint64_t dev_vclock = CoreMediaCodec::ReadU64LE(HexToBytes("A08D5313 01000000"), 0);
    auto actual_need = CoreMediaCodec::BuildAsynNeedPacket(dev_vclock);
    ASSERT_TRUE(actual_need == expected_need, "ASYN NEED packet hex mismatch");

    return true;
}

bool TestSpsResolutionAndAvccConversion() {
    auto sps_1080p = HexToBytes("67640028AC2CA501E0089F970110000003001000000303C8F1831960");
    auto dims = CoreMediaCodec::ParseH264SpsDimensions(sps_1080p);
    ASSERT_TRUE(dims.has_value(), "Failed to parse 1920x1080 SPS");
    ASSERT_TRUE(dims->width == 1920 && dims->height == 1080, "Expected 1920x1080 dimensions from SPS");

    std::vector<uint8_t> pps = {0x68, 0xEE, 0x3C, 0xB0};
    std::vector<uint8_t> avcc = {0x01, 0x64, 0x00, 0x28, 0xFF, 0xE1};
    avcc.push_back(static_cast<uint8_t>((sps_1080p.size() >> 8) & 0xFF));
    avcc.push_back(static_cast<uint8_t>(sps_1080p.size() & 0xFF));
    avcc.insert(avcc.end(), sps_1080p.begin(), sps_1080p.end());
    avcc.push_back(0x01);
    avcc.push_back(0x00);
    avcc.push_back(static_cast<uint8_t>(pps.size()));
    avcc.insert(avcc.end(), pps.begin(), pps.end());

    auto cfg = CoreMediaCodec::ParseAvcC(avcc);
    ASSERT_TRUE(cfg.has_value(), "Failed to parse avcC");
    ASSERT_TRUE(cfg->nal_length_size == 4 && cfg->width == 1920 && cfg->height == 1080,
                "avcC parsed metadata mismatch");

    std::vector<uint8_t> avcc_idr = {0x00, 0x00, 0x00, 0x05, 0x65, 0x88, 0x84, 0x21, 0xA0};
    bool is_idr = false;
    auto annexb = CoreMediaCodec::ConvertAvccToAnnexB(avcc_idr, 4, &(*cfg), &is_idr);
    ASSERT_TRUE(is_idr, "IDR NALU should be flagged as keyframe");
    ASSERT_TRUE(annexb.size() == 4 + sps_1080p.size() + 4 + pps.size() + 4 + 5,
                "Annex-B output length mismatch");
    ASSERT_TRUE(annexb[0] == 0 && annexb[1] == 0 && annexb[2] == 0 && annexb[3] == 1 && annexb[4] == 0x67,
                "Annex-B should start with SPS start code");

    return true;
}

bool TestFragmentedHandshakeAndMediaSession() {
    std::vector<std::vector<uint8_t>> tx_packets;
    std::vector<EncodedVideoPacket> rx_videos;
    std::vector<DecodedPcmPacket> rx_audios;

    QtSessionConfig cfg;
    cfg.valeria_clean_status_bar = true;
    cfg.max_width = 3840;
    cfg.max_height = 3840;
    QtStreamSession session(
        cfg,
        [&](std::span<const uint8_t> pkt) {
            tx_packets.emplace_back(pkt.begin(), pkt.end());
            return true;
        },
        [&](const EncodedVideoPacket& v) { rx_videos.push_back(v); },
        [&](const DecodedPcmPacket& a) { rx_audios.push_back(a); });

    std::vector<uint8_t> wire_stream;
    auto append = [&](const std::vector<uint8_t>& b) {
        wire_stream.insert(wire_stream.end(), b.begin(), b.end());
    };

    append(HexToBytes("10000000 676E6970 00000000 01000000"));
    append(HexToBytes("24000000 636E7973 01000000 00000000 61707763 E03D5713 01000000 E0740000 5A130040"));
    append(HexToBytes(
        "44000000 636E7973 B00CE26C A67F0000 746D6661 809D2213 01000000 "
        "00000000 0070E740 6D63706C 0C000000 04000000 01000000 04000000 02000000 10000000 00000000"));

    std::vector<uint8_t> cvrp;
    CoreMediaCodec::WriteU32LE(cvrp, 36U);
    CoreMediaCodec::WriteU32LE(cvrp, magic::kSync);
    CoreMediaCodec::WriteU64LE(cvrp, magic::kEmptyCFType);
    CoreMediaCodec::WriteU32LE(cvrp, magic::kCvrp);
    CoreMediaCodec::WriteU64LE(cvrp, 0x01135659D0ULL);
    CoreMediaCodec::WriteU64LE(cvrp, 0x0113538DA0ULL);
    append(cvrp);

    append(HexToBytes("1C000000 636E7973 5002D16C A67F0000 6B6F6C63 70495813 01000000"));
    append(HexToBytes("1C000000 636E7973 8079C17C A67F0000 656D6974 503D2213 01000000"));
    append(HexToBytes("1C000000 636E7973 8079C17C A67F0000 77656B73 60B9FD02 01000000"));

    std::vector<uint8_t> sdat_atom;
    std::vector<uint8_t> avcc_nalu = {0x00, 0x00, 0x00, 0x04, 0x65, 0xAA, 0xBB, 0xCC};
    CoreMediaCodec::WriteU32LE(sdat_atom, static_cast<uint32_t>(8 + avcc_nalu.size()));
    CoreMediaCodec::WriteU32LE(sdat_atom, magic::kSdat);
    sdat_atom.insert(sdat_atom.end(), avcc_nalu.begin(), avcc_nalu.end());

    std::vector<uint8_t> sbuf_video;
    CoreMediaCodec::WriteU32LE(sbuf_video, static_cast<uint32_t>(8 + sdat_atom.size()));
    CoreMediaCodec::WriteU32LE(sbuf_video, magic::kSbuf);
    sbuf_video.insert(sbuf_video.end(), sdat_atom.begin(), sdat_atom.end());

    std::vector<uint8_t> feed_pkt;
    CoreMediaCodec::WriteU32LE(feed_pkt, static_cast<uint32_t>(20 + sbuf_video.size()));
    CoreMediaCodec::WriteU32LE(feed_pkt, magic::kAsyn);
    CoreMediaCodec::WriteU64LE(feed_pkt, 0x11223344ULL);
    CoreMediaCodec::WriteU32LE(feed_pkt, magic::kFeed);
    feed_pkt.insert(feed_pkt.end(), sbuf_video.begin(), sbuf_video.end());
    append(feed_pkt);

    std::vector<uint8_t> pcm_bytes(480 * 4, 0x12);
    std::vector<uint8_t> audio_sdat;
    CoreMediaCodec::WriteU32LE(audio_sdat, static_cast<uint32_t>(8 + pcm_bytes.size()));
    CoreMediaCodec::WriteU32LE(audio_sdat, magic::kSdat);
    audio_sdat.insert(audio_sdat.end(), pcm_bytes.begin(), pcm_bytes.end());

    std::vector<uint8_t> sbuf_audio;
    CoreMediaCodec::WriteU32LE(sbuf_audio, static_cast<uint32_t>(8 + audio_sdat.size()));
    CoreMediaCodec::WriteU32LE(sbuf_audio, magic::kSbuf);
    sbuf_audio.insert(sbuf_audio.end(), audio_sdat.begin(), audio_sdat.end());

    std::vector<uint8_t> eat_pkt;
    CoreMediaCodec::WriteU32LE(eat_pkt, static_cast<uint32_t>(20 + sbuf_audio.size()));
    CoreMediaCodec::WriteU32LE(eat_pkt, magic::kAsyn);
    CoreMediaCodec::WriteU64LE(eat_pkt, 0x11223344ULL);
    CoreMediaCodec::WriteU32LE(eat_pkt, magic::kEat);
    eat_pkt.insert(eat_pkt.end(), sbuf_audio.begin(), sbuf_audio.end());
    append(eat_pkt);

    for (size_t off = 0; off < wire_stream.size(); off += 7) {
        size_t len = std::min<size_t>(7, wire_stream.size() - off);
        ASSERT_TRUE(session.FeedUsbBytes(std::span<const uint8_t>(wire_stream.data() + off, len)),
                    "Session failed during fragmented USB stream feed");
    }

    ASSERT_TRUE(session.IsStreaming(), "Session should enter streaming state after CVRP");
    ASSERT_TRUE(rx_videos.size() == 1 && rx_videos[0].is_keyframe, "Expected 1 decoded Annex-B keyframe");
    ASSERT_TRUE(rx_audios.size() == 1 && rx_audios[0].pcm_s16le.size() == 1920, "Expected 1920B PCM packet");
    ASSERT_TRUE(tx_packets.size() >= 10, "Expected handshake RPLY + HPD1/HPA1 + NEED packets");

    return true;
}

bool Test2KPlusAdaptiveAspectAndMultiReaderIpc() {
    // 1. Verify AlignAdaptiveDimensions preserves exact device aspect ratio with 4x2 alignment
    uint32_t aw = 0, ah = 0;
    AlignAdaptiveDimensions(2622, 1206, aw, ah);  // iPhone 16 Pro (2622 -> 2624, 1206 -> 1206)
    ASSERT_TRUE((aw % 4) == 0 && (ah % 2) == 0 && aw == 2624 && ah == 1206,
                "Expected 4x2-aligned 2K+ iPhone 16 Pro dimensions");

    // 2. Portrait 540x960 NV12 -> Landscape 1920x1080 NV12 pillarbox verification
    const uint32_t sw = 540, sh = 960;
    std::vector<uint8_t> src_nv12(sw * sh * 3 / 2, 200);
    std::memset(src_nv12.data() + sw * sh, 100, sw * sh / 2);

    std::vector<uint8_t> dst_1080p(1920 * 1080 * 3 / 2, 0);
    ScaleAndLetterboxNV12(
        src_nv12.data(), sw, src_nv12.data() + sw * sh, sw, sw, sh,
        dst_1080p.data(), 1920, 1080);

    ASSERT_TRUE(dst_1080p[540 * 1920 + 10] == 16, "Left pillarbox should be BT.709 black (Y=16)");
    ASSERT_TRUE(dst_1080p[540 * 1920 + 960] == 200, "Center active area should preserve source luma");

    // 3. Shared memory v2: 2K+ Native iPhone Pro Max (2868x1320, 19.5:9) + SPMC audio ring buffer
#ifdef _WIN32
    int pid = _getpid();
#else
    int pid = static_cast<int>(getpid());
#endif
    std::string test_shm = "IosVcamUnitTestShmV2_" + std::to_string(pid);

    SharedCaptureProducer producer;
    ASSERT_TRUE(producer.Open(test_shm), "Producer failed to open v2 shared memory");

    SharedCaptureConsumer obs_consumer;
    SharedCaptureConsumer discord_consumer;
    ASSERT_TRUE(obs_consumer.Open(test_shm), "OBS consumer failed to open shared memory");
    ASSERT_TRUE(discord_consumer.Open(test_shm), "Discord consumer failed to open shared memory");

    // Publish native 2868x1320 (2.8K 19.5:9) frame
    const uint32_t native_w = 2868, native_h = 1320;
    std::vector<uint8_t> native_2k_frame(static_cast<size_t>(native_w) * native_h * 3 / 2, 212);
    producer.PublishNV12Frame(native_2k_frame.data(), native_w, native_h, 16666667ULL, native_w, native_h, 24500);

    uint32_t adv_w = 0, adv_h = 0;
    ASSERT_TRUE(obs_consumer.QueryAdvertisedDimensions(adv_w, adv_h) && adv_w == 2868 && adv_h == 1320,
                "Advertised native 2.8K dimensions mismatch");

    std::vector<uint8_t> read_nv12(kMaxNv12FrameBytes, 0);
    uint32_t out_fw = 0, out_fh = 0, out_sw = 0, out_sh = 0;
    uint64_t seq = 0, pts = 0;
    ASSERT_TRUE(obs_consumer.ReadLatestNV12Frame(
                    read_nv12.data(), read_nv12.size(), out_fw, out_fh, seq, pts, &out_sw, &out_sh),
                "OBS consumer failed to read 2.8K NV12 frame");
    ASSERT_TRUE(seq == 1 && out_fw == 2868 && out_fh == 1320 && read_nv12[600 * 2868 + 1400] == 212,
                "Shared memory 2.8K NV12 frame integrity check failed");

    // Push 960 stereo frames (20ms @ 48kHz) and verify SPMC audio delivery
    std::vector<int16_t> pcm_in(960 * 2);
    for (size_t i = 0; i < 960; ++i) {
        pcm_in[i * 2] = static_cast<int16_t>(1000 + static_cast<int>(i));
        pcm_in[i * 2 + 1] = static_cast<int16_t>(-1000 - static_cast<int>(i));
    }
    producer.PushAudioPcmS16LE(pcm_in.data(), 960);

    std::vector<int16_t> obs_pcm(480 * 2, 0);
    std::vector<int16_t> discord_pcm(480 * 2, 0);
    size_t n_obs = obs_consumer.ReadAudioPcmS16LE(obs_pcm.data(), 480);
    size_t n_disc = discord_consumer.ReadAudioPcmS16LE(discord_pcm.data(), 480);
    ASSERT_TRUE(n_obs == 480 && n_disc == 480, "Both consumers must independently receive full 480 stereo frames");
    ASSERT_TRUE(obs_pcm == discord_pcm, "SPMC audio ring buffer must deliver identical samples to all readers");

    return true;
}

bool TestUsbSpeedResolutionGatingAndMaxClamp() {
    // Verify USB port link speed -> selectable max resolution ceiling mapping
    ASSERT_TRUE(MaxSelectableDimForUsbSpeed(UsbLinkSpeed::Low_1_5Mbps) == 1280U,
                "USB 1.0 should cap selectable max resolution at 1280p");
    ASSERT_TRUE(MaxSelectableDimForUsbSpeed(UsbLinkSpeed::Full_12Mbps) == 1280U,
                "USB 1.1 should cap selectable max resolution at 1280p");
    ASSERT_TRUE(MaxSelectableDimForUsbSpeed(UsbLinkSpeed::High_480Mbps) == 2560U,
                "USB 2.0 (480Mbps) should cap selectable max resolution at 2560p (2K)");
    ASSERT_TRUE(MaxSelectableDimForUsbSpeed(UsbLinkSpeed::Super_5Gbps) == 3840U,
                "USB 3.0 (5Gbps) should unlock full 3840p native resolution");
    ASSERT_TRUE(MaxSelectableDimForUsbSpeed(UsbLinkSpeed::SuperPlus_10Gbps) == 3840U,
                "USB 3.2 Gen 2 (10Gbps) should unlock full 3840p native resolution");
    ASSERT_TRUE(MaxSelectableDimForUsbSpeed(UsbLinkSpeed::SuperPlus_20Gbps) == 3840U,
                "USB 3.2 Gen 2x2 (20Gbps) should unlock full 3840p native resolution");

    // Verify proportional clamping preserves exact device aspect ratio & 4x2 alignment
    uint32_t w = 0, h = 0;
    // iPhone 16 Pro Max (2868x1320) unlocked on USB 3.x (3840 cap) -> unchanged 2868x1320
    ClampAdaptiveDimensionsToMax(2868, 1320, 3840, w, h);
    ASSERT_TRUE(w == 2868 && h == 1320, "3840 cap should keep native 2868x1320 untouched");

    // iPhone 16 Pro Max (2868x1320) capped at 2560p (USB 2.0 max) -> 2560x1178 (4x2 aligned)
    ClampAdaptiveDimensionsToMax(2868, 1320, 2560, w, h);
    ASSERT_TRUE(w == 2560 && (h % 2) == 0 && h >= 1176 && h <= 1180,
                "2560 cap should proportionally clamp 2868x1320 to ~2560x1178");

    // iPhone 16 Pro Max (2868x1320) capped at 1920p -> 1920x884 (4x2 aligned)
    ClampAdaptiveDimensionsToMax(2868, 1320, 1920, w, h);
    ASSERT_TRUE(w == 1920 && h == 884, "1920 cap should proportionally clamp 2868x1320 to 1920x884");

    return true;
}

bool TestGitHubReleasesJsonAndSemVerUpdater() {
    // 1. SemVer comparison checks
    ASSERT_TRUE(!IsRemoteVersionNewer("1.5.0", "v1.5.0"), "Identical version must not trigger update");
    ASSERT_TRUE(!IsRemoteVersionNewer("1.5.0.0", "v1.4.0"), "Older remote tag must never downgrade");
    ASSERT_TRUE(!IsRemoteVersionNewer("1.5.0", "v1.5.0-beta.1"), "Pre-release must not outrank final release");
    ASSERT_TRUE(IsRemoteVersionNewer("1.5.0", "v1.5.1"), "Patch bump should trigger update");
    ASSERT_TRUE(IsRemoteVersionNewer("1.5.0", "v1.6.0"), "Minor bump should trigger update");
    ASSERT_TRUE(IsRemoteVersionNewer("1.5.0", "2.0.0"), "Major bump without v prefix should trigger update");

    // 2. Realistic GitHub REST API /releases/latest JSON with nested author/uploader html_url, emoji surrogates, and sha256 digest
    const char* sample_github_json = R"JSON({
      "url": "https://api.github.com/repos/joey971018yt/iOS-usb-cam/releases/123456",
      "html_url": "https://github.com/joey971018yt/iOS-usb-cam/releases/tag/v1.6.0",
      "tag_name": "v1.6.0",
      "name": "JCast for iOS v1.6.0 \uD83D\uDE80",
      "draft": false,
      "prerelease": false,
      "published_at": "2026-11-27T02:00:00Z",
      "author": {
        "login": "joey971018yt",
        "html_url": "https://github.com/joey971018yt"
      },
      "body": "## What's New\r\n- Fast WinHTTP updater\n- SHA-256 verification",
      "assets": [
        {
          "name": "JCast-for-iOS-Portable-v1.6.0.zip",
          "size": 1480000,
          "digest": "sha256:73e2b1258f14c2c9ccfae1b544ea0fd7412ef7060ca1c6ed5bed4b095eac48ec",
          "browser_download_url": "https://github.com/joey971018yt/iOS-usb-cam/releases/download/v1.6.0/JCast-for-iOS-Portable-v1.6.0.zip",
          "uploader": { "html_url": "https://github.com/actions-user" }
        },
        {
          "name": "JCast-for-iOS-Setup-v1.6.0.exe",
          "size": 2850000,
          "digest": "SHA256:4321E761BB1FFE42930028BD96AFEA0067A089D22DE0A7DB04FB76F4F7F51F99",
          "browser_download_url": "https://github.com/joey971018yt/iOS-usb-cam/releases/download/v1.6.0/JCast-for-iOS-Setup-v1.6.0.exe"
        }
      ]
    })JSON";

    GitHubReleaseInfo info;
    std::string err;
    ASSERT_TRUE(ParseGitHubReleaseJson(sample_github_json, info, &err),
                ("Failed to parse GitHub release JSON: " + err).c_str());
    ASSERT_TRUE(info.tag_name == "v1.6.0", "Parsed tag_name mismatch");
    ASSERT_TRUE(info.html_url == "https://github.com/joey971018yt/iOS-usb-cam/releases/tag/v1.6.0",
                "Top-level html_url must not collide with nested author.html_url");
    ASSERT_TRUE(info.title.find("\xF0\x9F\x9A\x80") != std::string::npos,
                "UTF-16 surrogate pair \\uD83D\\uDE80 should decode to UTF-8 rocket emoji");
    ASSERT_TRUE(!info.draft && !info.prerelease, "Draft/prerelease flags should be false");
    ASSERT_TRUE(info.assets.size() == 2, "Expected 2 parsed release assets");
    ASSERT_TRUE(info.setup_exe.name == "JCast-for-iOS-Setup-v1.6.0.exe" && info.setup_exe.size_bytes == 2850000ULL,
                "Setup.exe asset selection mismatch");
    ASSERT_TRUE(info.setup_exe.sha256_hex == "4321e761bb1ffe42930028bd96afea0067a089d22de0a7db04fb76f4f7f51f99",
                "Expected normalized lowercase 64-char SHA-256 hex for Setup.exe");
    ASSERT_TRUE(info.portable_zip.name == "JCast-for-iOS-Portable-v1.6.0.zip" &&
                    info.portable_zip.size_bytes == 1480000ULL,
                "Portable.zip asset selection mismatch");
    ASSERT_TRUE(IsRemoteVersionNewer("1.5.0", info.tag_name), "v1.6.0 should be detected as newer than 1.5.0");

    return true;
}

bool TestPreviewDownscaleAndFullResScreenshotPipeline() {
    // 1. Verify ComputeAspectFitRect for 19.5:9 landscape, 9:19.5 portrait, and 4:3 iPad Pro
    LetterboxRect r_land = ComputeAspectFitRect(2868, 1320, 355, 147);
    ASSERT_TRUE(r_land.height == 147 && r_land.width >= 318 && r_land.width <= 320 && r_land.y == 0,
                "2868x1320 landscape inside 355x147 inline preview should fit full height (~319x147)");

    LetterboxRect r_port = ComputeAspectFitRect(1320, 2868, 355, 147);
    ASSERT_TRUE(r_port.height == 147 && r_port.width >= 67 && r_port.width <= 69 && r_port.x > 140,
                "1320x2868 portrait inside 355x147 inline preview should pillarbox center (~68x147)");

    LetterboxRect r_ipad = ComputeAspectFitRect(2732, 2048, 1920, 1080);
    ASSERT_TRUE(r_ipad.height == 1080 && r_ipad.width >= 1440 && r_ipad.width <= 1442,
                "2732x2048 iPad Pro inside 1920x1080 fullscreen monitor should fit ~1441x1080");

    // 2. Verify zero-copy preview sampling, duplicate-seq CPU skip, and 1:1 native 2K+ screenshot capture
#ifdef _WIN32
    int pid = _getpid();
#else
    int pid = static_cast<int>(getpid());
#endif
    std::string test_shm = "IosUsbVcamPreviewShotTest_" + std::to_string(pid);

    SharedCaptureProducer producer;
    ASSERT_TRUE(producer.Open(test_shm), "Failed to create shared memory producer for preview/screenshot test");

    SharedCaptureConsumer preview_consumer;
    ASSERT_TRUE(preview_consumer.Open(test_shm), "Failed to open shared memory consumer for preview/screenshot test");

    const uint32_t kNativeW = 2868;
    const uint32_t kNativeH = 1320;
    std::vector<uint8_t> nv12_src(static_cast<size_t>(kNativeW) * kNativeH * 3ULL / 2ULL, 0);
    GenerateAnimatedColorBarsNV12(nv12_src.data(), kNativeW, kNativeH, 7);

    producer.SetAdvertisedDimensions(kNativeW, kNativeH);
    producer.PublishNV12Frame(nv12_src.data(), kNativeW, kNativeH, 111'000'000ULL, kNativeW, kNativeH, 32500U);

    uint64_t last_seq = 0;
    bool updated = false;
    uint32_t fit_w = 0, fit_h = 0, sw = 0, sh = 0, kbps = 0;
    std::vector<uint8_t> preview_bgra;

    ASSERT_TRUE(
        preview_consumer.ReadLatestPreviewBGRA32(
            355, 147, false, last_seq, preview_bgra, fit_w, fit_h, sw, sh, &updated, &kbps),
        "ReadLatestPreviewBGRA32 should succeed on live 2868x1320 frame");
    ASSERT_TRUE(updated && last_seq == 1ULL, "First preview tick must report out_pixels_updated == true");
    ASSERT_TRUE(sw == kNativeW && sh == kNativeH && kbps == 32500U, "Preview metadata mismatch");
    ASSERT_TRUE(fit_h == 147 && fit_w >= 318 && fit_w <= 320, "Preview fit dimensions mismatch");
    ASSERT_TRUE(preview_bgra.size() == static_cast<size_t>(fit_w) * fit_h * 4ULL && preview_bgra[3] == 255U,
                "Preview BGRA buffer size or alpha channel invalid");

    // Calling ReadLatestPreviewBGRA32 again with unchanged frame_seq and force_refresh=false MUST skip pixel work!
    bool updated_dup = true;
    ASSERT_TRUE(
        preview_consumer.ReadLatestPreviewBGRA32(
            355, 147, false, last_seq, preview_bgra, fit_w, fit_h, sw, sh, &updated_dup, &kbps),
        "Duplicate ReadLatestPreviewBGRA32 should still report producer alive");
    ASSERT_TRUE(!updated_dup, "Unchanged frame_seq with !force_refresh must skip pixel conversion (0% idle CPU)");

    // Resizing the monitor window (force_refresh=true) must resample cleanly to the new box
    bool updated_resize = false;
    ASSERT_TRUE(
        preview_consumer.ReadLatestPreviewBGRA32(
            1280, 720, true, last_seq, preview_bgra, fit_w, fit_h, sw, sh, &updated_resize, &kbps),
        "Resized ReadLatestPreviewBGRA32 should resample cleanly");
    ASSERT_TRUE(updated_resize && fit_w == 1280 && fit_h >= 588 && fit_h <= 590,
                "Resized 1280x720 preview fit dimensions mismatch");

    // Full 1:1 native-resolution lossless screenshot capture must return exact 2868x1320 BGRA32
    std::vector<uint8_t> full_shot_bgra;
    uint32_t shot_w = 0, shot_h = 0;
    ASSERT_TRUE(preview_consumer.CaptureLatestFullResBGRA32(full_shot_bgra, shot_w, shot_h),
                "CaptureLatestFullResBGRA32 failed");
    ASSERT_TRUE(shot_w == kNativeW && shot_h == kNativeH &&
                    full_shot_bgra.size() == static_cast<size_t>(kNativeW) * kNativeH * 4ULL,
                "Full-resolution screenshot dimensions must be exact 1:1 2868x1320");
    // Top-left bar of 75% broadcast HD color bars (Y=180, U=128, V=128) decodes to RGB(191, 191, 191), A=255
    ASSERT_TRUE(full_shot_bgra[0] >= 188 && full_shot_bgra[0] <= 194 &&
                    full_shot_bgra[1] >= 188 && full_shot_bgra[1] <= 194 &&
                    full_shot_bgra[2] >= 188 && full_shot_bgra[2] <= 194 &&
                    full_shot_bgra[3] == 255,
                "Top-left 75% broadcast white bar pixel in full-res screenshot should be ~191 with A=255");

    // Verify timestamped screenshot filename convention
    std::filesystem::path tmp_dir = std::filesystem::temp_directory_path() / ("jcast_shot_test_" + std::to_string(pid));
    std::filesystem::path shot_path = BuildTimestampedScreenshotPath(tmp_dir, shot_w, shot_h);
    std::string fname_u8 = shot_path.filename().string();
    ASSERT_TRUE(fname_u8.find("JCast_") == 0 && fname_u8.find("_2868x1320.png") != std::string::npos,
                "BuildTimestampedScreenshotPath filename format mismatch");

#ifdef _WIN32
    // On Windows (MSVC CI & Windows hosts), verify end-to-end WIC lossless PNG encoding & PNG IHDR header
    std::string png_err;
    ASSERT_TRUE(SaveTopDownBGRA32AsPng(full_shot_bgra.data(), shot_w, shot_h, shot_path, &png_err),
                ("SaveTopDownBGRA32AsPng failed on Windows: " + png_err).c_str());
    std::ifstream png_in(shot_path, std::ios::binary);
    uint8_t hdr[24] = {0};
    png_in.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
    png_in.close();
    const uint8_t kPngMagic[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    ASSERT_TRUE(std::memcmp(hdr, kPngMagic, 8) == 0, "Saved screenshot does not start with valid PNG magic signature");
    uint32_t ihdr_w = (static_cast<uint32_t>(hdr[16]) << 24) | (static_cast<uint32_t>(hdr[17]) << 16) |
                      (static_cast<uint32_t>(hdr[18]) << 8) | static_cast<uint32_t>(hdr[19]);
    uint32_t ihdr_h = (static_cast<uint32_t>(hdr[20]) << 24) | (static_cast<uint32_t>(hdr[21]) << 16) |
                      (static_cast<uint32_t>(hdr[22]) << 8) | static_cast<uint32_t>(hdr[23]);
    ASSERT_TRUE(ihdr_w == kNativeW && ihdr_h == kNativeH, "PNG IHDR dimensions must match 1:1 2868x1320");
    std::error_code ec;
    std::filesystem::remove_all(tmp_dir, ec);
#endif

    preview_consumer.Close();
    producer.Close();
    return true;
}

bool TestStreamHealthTelemetryAndGrading() {
    ASSERT_TRUE(sizeof(VideoSlotHeader) == 64U, "VideoSlotHeader must remain 64 bytes");

    // 1. Deterministic health grade classifier checks
    StreamHealthSnapshot s_idle{};
    ASSERT_TRUE(EvaluateStreamHealthGrade(s_idle) == StreamHealthGrade::Idle, "Inactive stream must be Idle");

    StreamHealthSnapshot s_wait{};
    s_wait.active = true;
    s_wait.waiting_first_frame = true;
    ASSERT_TRUE(EvaluateStreamHealthGrade(s_wait) == StreamHealthGrade::WaitingStream,
                "Waiting for first frame must grade as WaitingStream");

    StreamHealthSnapshot s_exc{};
    s_exc.active = true;
    s_exc.fps = 59.9;
    s_exc.bitrate_mbps = 28.4;
    s_exc.decode_latency_ms = 1.1;
    s_exc.pipeline_latency_ms = 1.6;
    s_exc.hw_accelerated = true;
    ASSERT_TRUE(EvaluateStreamHealthGrade(s_exc) == StreamHealthGrade::Excellent,
                "60fps 28.4Mbps 1.6ms D3D11VA stream must grade as Excellent");

    StreamHealthSnapshot s_vfr = s_exc;
    s_vfr.fps = 5.0;
    s_vfr.bitrate_mbps = 0.35;
    s_vfr.pipeline_latency_ms = 1.2;
    ASSERT_TRUE(EvaluateStreamHealthGrade(s_vfr) == StreamHealthGrade::StaticVfr,
                "Low-FPS low-bitrate fast-decode iOS untouched screen must grade as StaticVfr, not Warning");

    StreamHealthSnapshot s_cpu_lag = s_exc;
    s_cpu_lag.decode_latency_ms = 14.5;
    s_cpu_lag.pipeline_latency_ms = 16.9;
    s_cpu_lag.hw_accelerated = false;
    ASSERT_TRUE(EvaluateStreamHealthGrade(s_cpu_lag) == StreamHealthGrade::Warning,
                "Over-budget >14ms pipeline latency must trigger Warning grade");

    StreamHealthSnapshot s_bw_drop = s_exc;
    s_bw_drop.fps = 14.0;
    s_bw_drop.bitrate_mbps = 12.0;
    ASSERT_TRUE(EvaluateStreamHealthGrade(s_bw_drop) == StreamHealthGrade::Warning,
                "High motion bitrate with <22fps drop must trigger Warning grade");

    // 2. End-to-end lock-free IPC telemetry roundtrip
#ifdef _WIN32
    int pid = _getpid();
#else
    int pid = static_cast<int>(getpid());
#endif
    std::string test_shm = "IosUsbVcamTelemetryTest_" + std::to_string(pid);

    SharedCaptureProducer producer;
    ASSERT_TRUE(producer.Open(test_shm), "Failed to open producer for telemetry test");
    producer.TouchHeartbeat(false);

    SharedCaptureConsumer consumer;
    ASSERT_TRUE(consumer.Open(test_shm), "Failed to open consumer for telemetry test");

    StreamHealthSnapshot snap{};
    ASSERT_TRUE(consumer.ReadStreamHealth(snap), "ReadStreamHealth should return true while host heartbeat is alive");
    ASSERT_TRUE(snap.active && snap.waiting_first_frame && snap.grade == StreamHealthGrade::WaitingStream,
                "Before first video frame, ReadStreamHealth should report WaitingStream");

    const uint32_t kW = 2868, kH = 1320;
    std::vector<uint8_t> nv12(static_cast<size_t>(kW) * kH * 3ULL / 2ULL, 128);
    producer.AddEncodedVideoBytes(1420000ULL);
    producer.SetAudioPacketCount(420ULL);

    FrameTelemetryMeta meta{};
    meta.fps_x100 = 5994U;
    meta.decode_latency_us = 1150U;
    meta.pipeline_latency_us = 1680U;
    meta.stream_flags =
        kStreamFlagHwAccelerated | kStreamFlagAdaptiveRes | (static_cast<uint32_t>(UsbLinkSpeed::Super_5Gbps) << 8);
    meta.dropped_frames = 0U;

    producer.PublishNV12Frame(nv12.data(), kW, kH, 16666667ULL, kW, kH, 28400U, meta);

    ASSERT_TRUE(consumer.ReadStreamHealth(snap), "ReadStreamHealth should succeed on active 2K+ stream");
    ASSERT_TRUE(snap.active && !snap.waiting_first_frame, "Snapshot must be active streaming");
    ASSERT_TRUE(snap.fps > 59.8 && snap.fps < 60.1, "Snapshot FPS mismatch");
    ASSERT_TRUE(snap.bitrate_mbps > 28.3 && snap.bitrate_mbps < 28.5, "Snapshot Mbps mismatch");
    ASSERT_TRUE(snap.peak_bitrate_mbps >= 28.4, "Snapshot peak Mbps mismatch");
    ASSERT_TRUE(snap.decode_latency_ms > 1.14 && snap.decode_latency_ms < 1.16, "Decode latency ms mismatch");
    ASSERT_TRUE(snap.pipeline_latency_ms > 1.67 && snap.pipeline_latency_ms < 1.69, "Pipeline latency ms mismatch");
    ASSERT_TRUE(snap.hw_accelerated && snap.adaptive_res && !snap.mock_source, "Stream flags mismatch");
    ASSERT_TRUE(snap.usb_speed_code == static_cast<uint8_t>(UsbLinkSpeed::Super_5Gbps), "USB speed code mismatch");
    ASSERT_TRUE(snap.total_video_bytes == 1420000ULL && snap.total_audio_packets == 420ULL,
                "Cumulative byte/audio counters mismatch");
    ASSERT_TRUE(snap.grade == StreamHealthGrade::Excellent, "Active 2K+60fps stream grade must be Excellent");

    producer.Close();
    ASSERT_TRUE(!consumer.ReadStreamHealth(snap) && !snap.active && snap.grade == StreamHealthGrade::Idle,
                "After producer Close(), ReadStreamHealth must reset cleanly to Idle");
    consumer.Close();
    return true;
}

bool TestFaultDiagnosticsAndRedaction() {
    // 1. Verify LookupDiagSpec covers all E101..E503 codes with non-empty Chinese root-cause & fix steps
    const DiagCode kAllCodes[] = {
        DiagCode::E101_MissingCoreFiles,
        DiagCode::E102_MissingAppleService,
        DiagCode::E103_MissingMediaFoundation,
        DiagCode::E104_DshowNotRegistered,
        DiagCode::E201_ZadigDriverConflict,
        DiagCode::E202_UpperFilterMissing,
        DiagCode::E203_UsbOpenAccessDenied,
        DiagCode::E204_UsbInterfaceBusy,
        DiagCode::E301_IosNotTrustedOrLocked,
        DiagCode::E302_SetConfigFailed,
        DiagCode::E303_HandshakeNoFrames,
        DiagCode::E304_UsbTransferIoDrop,
        DiagCode::E401_DecoderInitFailed,
        DiagCode::E402_GpuFallbackToSoftware,
        DiagCode::E403_HighDecodeLatency,
        DiagCode::E501_AudioEndpointLost,
        DiagCode::E502_SharedMemInitFailed,
        DiagCode::E503_HostUnexpectedExit,
    };
    for (DiagCode dc : kAllCodes) {
        const auto& sp = LookupDiagSpec(dc);
        ASSERT_TRUE(sp.code == dc, "LookupDiagSpec code mismatch");
        ASSERT_TRUE(std::strncmp(sp.code_str, "JCAST-E", 7) == 0, "DiagCodeSpec code_str prefix mismatch");
        ASSERT_TRUE(std::strlen(sp.title_utf8) > 6 && std::strlen(sp.cause_utf8) > 10 &&
                        std::strlen(sp.fix_steps_utf8) > 10,
                    "DiagCodeSpec must have non-empty Chinese title, cause, and fix steps");
        ASSERT_TRUE(ParseDiagCodeToken(sp.code_str) == dc, "ParseDiagCodeToken full JCAST-Exxx failed");
        ASSERT_TRUE(ParseDiagCodeToken(sp.code_str + 6) == dc, "ParseDiagCodeToken short Exxx failed");
    }

    // 2. Verify FormatHostDiagLine -> ParseHostDiagLine structured IPC stdout roundtrip
    {
        std::string line = FormatHostDiagLine(
            DiagCode::E402_GpuFallbackToSoftware,
            DiagSeverity::Warning,
            "D3D11VA TDR reset detected; switched seamlessly to CPU software decoder");
        ASSERT_TRUE(line.rfind("[JCAST-DIAG|E402|WARN] ", 0) == 0, "FormatHostDiagLine header mismatch");
        ParsedDiagEvent ev{};
        ASSERT_TRUE(ParseHostDiagLine(line, ev), "ParseHostDiagLine failed on valid line");
        ASSERT_TRUE(ev.code == DiagCode::E402_GpuFallbackToSoftware &&
                        ev.severity == DiagSeverity::Warning &&
                        ev.detail_utf8.find("D3D11VA TDR reset") != std::string::npos,
                    "ParsedDiagEvent roundtrip fields mismatch");

        ParsedDiagEvent normal_ev{};
        ASSERT_TRUE(!ParseHostDiagLine("[Host] Connected: Product=0x12A8", normal_ev),
                    "Normal host log line must NOT parse as [JCAST-DIAG|...]");
    }

    // 3. Verify ClassifyUsbOrTransportError accurately maps libusb / transport errors
    ASSERT_TRUE(
        ClassifyUsbOrTransportError("Failed to load libusb-1.0.dll (missing file)") ==
            DiagCode::E101_MissingCoreFiles,
        "Missing libusb-1.0.dll should classify as E101");
    ASSERT_TRUE(
        ClassifyUsbOrTransportError("libusb_open failed (code -3: LIBUSB_ERROR_ACCESS)") ==
            DiagCode::E203_UsbOpenAccessDenied,
        "libusb_open (-3) should classify as E203");
    ASSERT_TRUE(
        ClassifyUsbOrTransportError("Failed to claim QuickTime USB interface (code -6: LIBUSB_ERROR_BUSY)") ==
            DiagCode::E204_UsbInterfaceBusy,
        "claim_interface (-6) should classify as E204");
    ASSERT_TRUE(
        ClassifyUsbOrTransportError("libusb_set_configuration(5) failed (-5)") ==
            DiagCode::E302_SetConfigFailed,
        "set_configuration failure should classify as E302");
    ASSERT_TRUE(
        ClassifyUsbOrTransportError("Device did not expose QuickTime SubClass 0x2A after re-enumeration") ==
            DiagCode::E301_IosNotTrustedOrLocked,
        "Missing SubClass 0x2A should classify as E301");
    ASSERT_TRUE(
        ClassifyUsbOrTransportError("USB Bulk IN transfer failed (-1: LIBUSB_ERROR_IO)") ==
            DiagCode::E304_UsbTransferIoDrop,
        "Bulk IN (-1) drop should classify as E304");

    // 4. Verify Windows NTSTATUS / Win32 exit code translation
    ASSERT_TRUE(DescribeProcessExitCode(0xC0000135U).find("STATUS_DLL_NOT_FOUND") != std::string::npos,
                "0xC0000135 must decode to STATUS_DLL_NOT_FOUND");
    ASSERT_TRUE(DescribeProcessExitCode(0xC0000005U).find("STATUS_ACCESS_VIOLATION") != std::string::npos,
                "0xC0000005 must decode to STATUS_ACCESS_VIOLATION");

    // 5. Verify privacy-safe redaction scrubs Windows user folder names & modern/classic Apple UDIDs
    std::string sensitive =
        "Path: C:\\Users\\JoeyChen\\Pictures\\JCast Screenshots\\shot.png\n"
        "PublicPath: C:\\Users\\Public\\Documents\\ok.txt\n"
        "[Host] Connected: Product=0x12A8 Serial=00008130-001A2B3C4D5E6F70 LinkSpeed=USB 3.0\n"
        "RawUDID1: 00008120-0099AABBCCDDEEFF and LegacyUDID: 41f2a9c088b1234567890abcdef1234567890abc\n";
    std::string redacted = RedactSensitiveReportText(sensitive);
    ASSERT_TRUE(redacted.find("JoeyChen") == std::string::npos &&
                    redacted.find("C:\\Users\\<USER>\\Pictures") != std::string::npos,
                "Windows user profile directory must be redacted to <USER>");
    ASSERT_TRUE(redacted.find("C:\\Users\\Public\\Documents") != std::string::npos,
                "C:\\Users\\Public must not be redacted");
    ASSERT_TRUE(redacted.find("001A2B3C4D5E6F70") == std::string::npos &&
                    redacted.find("Serial=0000****-REDACTED") != std::string::npos,
                "Serial= Apple UDID must be redacted");
    ASSERT_TRUE(redacted.find("0099AABBCCDDEEFF") == std::string::npos &&
                    redacted.find("41f2a9c088b1234567890abcdef1234567890abc") == std::string::npos &&
                    redacted.find("41f2****-REDACTED") != std::string::npos,
                "Standalone 25-char and 40-char Apple UDIDs must be redacted");

    // 6. Verify GitHub New Issue URL builder produces safe HTTPS GitHub URL with encoded summary
    std::string gh_url = BuildGitHubNewIssueUrl(
        "joey971018yt",
        "iOS-usb-cam",
        "1.8.0",
        DiagCode::E301_IosNotTrustedOrLocked,
        "User C:\\Users\\Alice\\Desktop Serial=00008130-0011223344556677");
    ASSERT_TRUE(
        gh_url.rfind("https://github.com/joey971018yt/iOS-usb-cam/issues/new?title=", 0) == 0,
        "BuildGitHubNewIssueUrl prefix mismatch");
    ASSERT_TRUE(gh_url.find("JCAST-E301") != std::string::npos &&
                    gh_url.find("Alice") == std::string::npos &&
                    gh_url.find("0011223344556677") == std::string::npos,
                "BuildGitHubNewIssueUrl must include error code and redact sensitive summary fields");

    return true;
}

}  // namespace

int main() {
    std::cout << "[JCast for iOS] Running protocol, USB speed, updater, preview, screenshot, telemetry & diagnostics tests...\n";
    if (!TestExactQuickTimeHexVectors()) return EXIT_FAILURE;
    std::cout << "  [PASS] TestExactQuickTimeHexVectors\n";
    if (!TestSpsResolutionAndAvccConversion()) return EXIT_FAILURE;
    std::cout << "  [PASS] TestSpsResolutionAndAvccConversion\n";
    if (!TestFragmentedHandshakeAndMediaSession()) return EXIT_FAILURE;
    std::cout << "  [PASS] TestFragmentedHandshakeAndMediaSession\n";
    if (!Test2KPlusAdaptiveAspectAndMultiReaderIpc()) return EXIT_FAILURE;
    std::cout << "  [PASS] Test2KPlusAdaptiveAspectAndMultiReaderIpc\n";
    if (!TestUsbSpeedResolutionGatingAndMaxClamp()) return EXIT_FAILURE;
    std::cout << "  [PASS] TestUsbSpeedResolutionGatingAndMaxClamp\n";
    if (!TestGitHubReleasesJsonAndSemVerUpdater()) return EXIT_FAILURE;
    std::cout << "  [PASS] TestGitHubReleasesJsonAndSemVerUpdater\n";
    if (!TestPreviewDownscaleAndFullResScreenshotPipeline()) return EXIT_FAILURE;
    std::cout << "  [PASS] TestPreviewDownscaleAndFullResScreenshotPipeline\n";
    if (!TestStreamHealthTelemetryAndGrading()) return EXIT_FAILURE;
    std::cout << "  [PASS] TestStreamHealthTelemetryAndGrading\n";
    if (!TestFaultDiagnosticsAndRedaction()) return EXIT_FAILURE;
    std::cout << "  [PASS] TestFaultDiagnosticsAndRedaction\n";
    std::cout << "[JCast for iOS] All tests passed successfully!\n";
    return EXIT_SUCCESS;
}
