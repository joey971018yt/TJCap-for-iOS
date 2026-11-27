#include "ios_vcam/app_updater.h"
#include "ios_vcam/coremedia_codec.h"
#include "ios_vcam/image_convert.h"
#include "ios_vcam/qt_session.h"
#include "ios_vcam/shared_ipc.h"
#include "ios_vcam/usb_ios_transport.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

}  // namespace

int main() {
    std::cout << "[JCast for iOS] Running protocol, USB speed gating, updater & 2K+ pipeline unit tests...\n";
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
    std::cout << "[JCast for iOS] All tests passed successfully!\n";
    return EXIT_SUCCESS;
}
