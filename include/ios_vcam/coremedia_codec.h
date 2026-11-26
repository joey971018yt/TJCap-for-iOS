#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace ios_vcam {

// CoreMedia / QuickTime USB (Valeria) Little-Endian FourCC constants
namespace magic {
inline constexpr uint32_t kPing = 0x70696E67;  // 'ping' -> wire LE: 67 6E 69 70
inline constexpr uint32_t kSync = 0x73796E63;  // 'sync'
inline constexpr uint32_t kRply = 0x72706C79;  // 'rply'
inline constexpr uint32_t kAsyn = 0x6173796E;  // 'asyn'

// SYNC subtypes
inline constexpr uint32_t kCwpa = 0x63777061;  // 'cwpa'
inline constexpr uint32_t kAfmt = 0x61666D74;  // 'afmt'
inline constexpr uint32_t kCvrp = 0x63767270;  // 'cvrp'
inline constexpr uint32_t kClok = 0x636C6F6B;  // 'clok'
inline constexpr uint32_t kTime = 0x74696D65;  // 'time'
inline constexpr uint32_t kSkew = 0x736B6577;  // 'skew'
inline constexpr uint32_t kOg   = 0x676F2120;  // 'go! ' -> wire LE: 20 21 6F 67
inline constexpr uint32_t kStop = 0x73746F70;  // 'stop'

// ASYN subtypes
inline constexpr uint32_t kHpd1 = 0x68706431;  // 'hpd1'
inline constexpr uint32_t kHpd0 = 0x68706430;  // 'hpd0'
inline constexpr uint32_t kHpa1 = 0x68706131;  // 'hpa1'
inline constexpr uint32_t kHpa0 = 0x68706130;  // 'hpa0'
inline constexpr uint32_t kFeed = 0x66656564;  // 'feed'
inline constexpr uint32_t kEat  = 0x65617421;  // 'eat!'
inline constexpr uint32_t kNeed = 0x6E656564;  // 'need'
inline constexpr uint32_t kSprp = 0x73707270;  // 'sprp'
inline constexpr uint32_t kSrat = 0x73726174;  // 'srat'
inline constexpr uint32_t kTbas = 0x74626173;  // 'tbas'
inline constexpr uint32_t kTjmp = 0x746A6D70;  // 'tjmp'
inline constexpr uint32_t kRels = 0x72656C73;  // 'rels'

// TLV / Dictionary / SampleBuffer markers
inline constexpr uint32_t kDict = 0x64696374;  // 'dict'
inline constexpr uint32_t kKeyv = 0x6B657976;  // 'keyv'
inline constexpr uint32_t kStrk = 0x7374726B;  // 'strk'
inline constexpr uint32_t kIdxk = 0x6964786B;  // 'idxk'
inline constexpr uint32_t kBulv = 0x62756C76;  // 'bulv'
inline constexpr uint32_t kStrv = 0x73747276;  // 'strv'
inline constexpr uint32_t kDatv = 0x64617476;  // 'datv'
inline constexpr uint32_t kNmbv = 0x6E6D6276;  // 'nmbv'

inline constexpr uint32_t kFdsc = 0x66647363;  // 'fdsc'
inline constexpr uint32_t kMdia = 0x6D646961;  // 'mdia'
inline constexpr uint32_t kVide = 0x76696465;  // 'vide'
inline constexpr uint32_t kSoun = 0x736F756E;  // 'soun'
inline constexpr uint32_t kVdim = 0x7664696D;  // 'vdim'
inline constexpr uint32_t kCodc = 0x636F6463;  // 'codc'
inline constexpr uint32_t kAvc1 = 0x61766331;  // 'avc1'
inline constexpr uint32_t kExtn = 0x6578746E;  // 'extn'
inline constexpr uint32_t kAsbd = 0x61736264;  // 'asbd'
inline constexpr uint32_t kLpcm = 0x6C70636D;  // 'lpcm'

inline constexpr uint32_t kSbuf = 0x73627566;  // 'sbuf'
inline constexpr uint32_t kOpts = 0x6F707473;  // 'opts'
inline constexpr uint32_t kStia = 0x73746961;  // 'stia'
inline constexpr uint32_t kSdat = 0x73646174;  // 'sdat'
inline constexpr uint32_t kSatt = 0x73617474;  // 'satt'
inline constexpr uint32_t kSary = 0x73617279;  // 'sary'
inline constexpr uint32_t kSsiz = 0x7373697A;  // 'ssiz'
inline constexpr uint32_t kNsmp = 0x6E736D70;  // 'nsmp'

inline constexpr uint64_t kEmptyCFType = 1ULL;
}  // namespace magic

struct CMTime {
    uint64_t value = 0;
    uint32_t timescale = 1000000000U;  // Default nanoseconds
    uint32_t flags = 1U;               // kCMTimeFlags_Valid
    uint64_t epoch = 0;

    double Seconds() const {
        return timescale > 0 ? static_cast<double>(value) / static_cast<double>(timescale) : 0.0;
    }
};

struct NSNumber {
    enum class Kind : uint8_t {
        UInt32 = 3,
        UInt64 = 4,
        SInt32 = 5,
        Float64 = 6,
    };

    Kind kind = Kind::UInt32;
    uint32_t u32 = 0;
    uint64_t u64 = 0;
    double f64 = 0.0;

    static NSNumber FromUInt32(uint32_t v) {
        NSNumber n;
        n.kind = Kind::UInt32;
        n.u32 = v;
        return n;
    }
    static NSNumber FromUInt64(uint64_t v) {
        NSNumber n;
        n.kind = Kind::UInt64;
        n.u64 = v;
        return n;
    }
    static NSNumber FromFloat64(double v) {
        NSNumber n;
        n.kind = Kind::Float64;
        n.f64 = v;
        return n;
    }
};

struct AudioStreamBasicDescription {
    double sample_rate = 48000.0;
    uint32_t format_id = magic::kLpcm;
    uint32_t format_flags = 12;  // kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked
    uint32_t bytes_per_packet = 4;
    uint32_t frames_per_packet = 1;
    uint32_t bytes_per_frame = 4;
    uint32_t channels_per_frame = 2;
    uint32_t bits_per_channel = 16;
    uint32_t reserved = 0;
};

struct AvcDecoderConfig {
    uint8_t nal_length_size = 4;
    std::vector<std::vector<uint8_t>> sps_list;
    std::vector<std::vector<uint8_t>> pps_list;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct FormatDescription {
    uint32_t media_type = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t codec = 0;
    AvcDecoderConfig avc_config;
    AudioStreamBasicDescription asbd;
};

struct SampleBufferData {
    CMTime pts;
    uint32_t num_samples = 0;
    bool has_format_description = false;
    FormatDescription format_description;
    std::vector<uint8_t> sample_data;
    std::vector<uint32_t> sample_sizes;
};

struct StringDictEntry;

struct StringDict {
    std::vector<StringDictEntry> entries;
};

using DictValue = std::variant<bool, NSNumber, std::string, std::vector<uint8_t>, StringDict, FormatDescription>;

struct StringDictEntry {
    std::string key;
    DictValue value;
};

struct SpsDimensions {
    uint32_t width = 0;
    uint32_t height = 0;
};

class CoreMediaCodec {
public:
    // Little-Endian primitive helpers
    static void WriteU32LE(std::vector<uint8_t>& out, uint32_t v);
    static void WriteU64LE(std::vector<uint8_t>& out, uint64_t v);
    static void WriteF64LE(std::vector<uint8_t>& out, double v);
    static uint32_t ReadU32LE(std::span<const uint8_t> buf, size_t offset = 0);
    static uint64_t ReadU64LE(std::span<const uint8_t> buf, size_t offset = 0);
    static double ReadF64LE(std::span<const uint8_t> buf, size_t offset = 0);

    // CoreMedia object serialization/deserialization
    static std::vector<uint8_t> SerializeCMTime(const CMTime& t);
    static std::optional<CMTime> ParseCMTime(std::span<const uint8_t> data);

    static std::vector<uint8_t> SerializeNSNumber(const NSNumber& num);
    static std::optional<NSNumber> ParseNSNumber(std::span<const uint8_t> data);

    static std::vector<uint8_t> SerializeAudioStreamRangedDescription(const AudioStreamBasicDescription& asbd);
    static std::optional<AudioStreamBasicDescription> ParseAudioStreamBasicDescription(std::span<const uint8_t> data);

    static std::vector<uint8_t> SerializeStringDict(const StringDict& dict);
    static std::optional<StringDict> ParseStringDict(std::span<const uint8_t> data);

    static std::optional<FormatDescription> ParseFormatDescription(std::span<const uint8_t> data);
    static std::optional<SampleBufferData> ParseSampleBuffer(std::span<const uint8_t> data, uint32_t media_type);

    // H.264 avcC / SPS / AVCC-to-AnnexB helpers
    static std::optional<AvcDecoderConfig> ParseAvcC(std::span<const uint8_t> data);
    static std::optional<SpsDimensions> ParseH264SpsDimensions(std::span<const uint8_t> sps_nalu);
    static std::vector<uint8_t> ConvertAvccToAnnexB(
        std::span<const uint8_t> avcc_sample,
        uint8_t nal_length_size = 4,
        const AvcDecoderConfig* prepend_parameter_sets = nullptr,
        bool* out_is_keyframe = nullptr);

    // QuickTime / Valeria control packet builders
    static std::vector<uint8_t> BuildPingPacket();
    static StringDict BuildHpd1DeviceInfoDict(bool valeria_clean_bar = true, uint32_t max_w = 1920, uint32_t max_h = 1200);
    static StringDict BuildHpa1DeviceInfoDict();
    static std::vector<uint8_t> BuildAsynDictPacket(uint32_t subtype, uint64_t clock_ref, const StringDict& dict);
    static std::vector<uint8_t> BuildAsynNeedPacket(uint64_t video_device_clock_ref);
    static std::vector<uint8_t> BuildAsynHpd0Packet();
    static std::vector<uint8_t> BuildAsynHpa0Packet(uint64_t audio_device_clock_ref);

    static std::vector<uint8_t> BuildRplyClockRef(uint64_t correlation_id, uint64_t local_clock_ref);
    static std::vector<uint8_t> BuildRplyAfmtOk(uint64_t correlation_id);
    static std::vector<uint8_t> BuildRplyCMTime(uint64_t correlation_id, const CMTime& time);
    static std::vector<uint8_t> BuildRplySkew(uint64_t correlation_id, double skew_hz = 48000.0);
    static std::vector<uint8_t> BuildRplyZero64(uint64_t correlation_id);
};

}  // namespace ios_vcam
