#include "ios_vcam/coremedia_codec.h"

#include <cstring>

namespace ios_vcam {

namespace {

class BitReader {
public:
    explicit BitReader(std::span<const uint8_t> rbsp) : data_(rbsp) {}

    bool ReadBit(uint32_t& out) {
        if (byte_pos_ >= data_.size()) {
            return false;
        }
        out = (data_[byte_pos_] >> (7 - bit_pos_)) & 1U;
        if (++bit_pos_ == 8) {
            bit_pos_ = 0;
            ++byte_pos_;
        }
        return true;
    }

    bool ReadBits(size_t count, uint32_t& out) {
        out = 0;
        for (size_t i = 0; i < count; ++i) {
            uint32_t b = 0;
            if (!ReadBit(b)) {
                return false;
            }
            out = (out << 1) | b;
        }
        return true;
    }

    bool ReadUE(uint32_t& out) {
        size_t leading_zeros = 0;
        uint32_t b = 0;
        while (true) {
            if (!ReadBit(b)) {
                return false;
            }
            if (b == 1) {
                break;
            }
            ++leading_zeros;
            if (leading_zeros > 31) {
                return false;
            }
        }
        if (leading_zeros == 0) {
            out = 0;
            return true;
        }
        uint32_t suffix = 0;
        if (!ReadBits(leading_zeros, suffix)) {
            return false;
        }
        out = ((1U << leading_zeros) - 1U) + suffix;
        return true;
    }

    bool ReadSE(int32_t& out) {
        uint32_t code_num = 0;
        if (!ReadUE(code_num)) {
            return false;
        }
        int32_t abs_val = static_cast<int32_t>((code_num + 1U) >> 1);
        out = (code_num & 1U) ? abs_val : -abs_val;
        return true;
    }

private:
    std::span<const uint8_t> data_;
    size_t byte_pos_ = 0;
    int bit_pos_ = 0;
};

std::vector<uint8_t> RemoveEmulationPreventionBytes(std::span<const uint8_t> nal) {
    std::vector<uint8_t> rbsp;
    rbsp.reserve(nal.size());
    for (size_t i = 0; i < nal.size(); ++i) {
        if (i + 2 < nal.size() && nal[i] == 0x00 && nal[i + 1] == 0x00 && nal[i + 2] == 0x03) {
            rbsp.push_back(0x00);
            rbsp.push_back(0x00);
            i += 2;
        } else {
            rbsp.push_back(nal[i]);
        }
    }
    return rbsp;
}

bool SkipScalingList(BitReader& br, size_t size_of_scaling_list) {
    int32_t last_scale = 8;
    int32_t next_scale = 8;
    for (size_t j = 0; j < size_of_scaling_list; ++j) {
        if (next_scale != 0) {
            int32_t delta_scale = 0;
            if (!br.ReadSE(delta_scale)) {
                return false;
            }
            next_scale = (last_scale + delta_scale + 256) % 256;
        }
        last_scale = (next_scale == 0) ? last_scale : next_scale;
    }
    return true;
}

// Recursively searches an index-keyed CoreMedia dictionary ('dict' or 'extn') for key 105 (avcC)
std::optional<std::vector<uint8_t>> FindAvcCInIndexDict(std::span<const uint8_t> data, uint32_t expected_magic) {
    if (data.size() < 8) {
        return std::nullopt;
    }
    uint32_t total_len = CoreMediaCodec::ReadU32LE(data, 0);
    uint32_t magic_val = CoreMediaCodec::ReadU32LE(data, 4);
    if (total_len > data.size() || total_len < 8 || magic_val != expected_magic) {
        return std::nullopt;
    }
    size_t pos = 8;
    while (pos + 8 <= total_len) {
        uint32_t kv_len = CoreMediaCodec::ReadU32LE(data, pos);
        uint32_t kv_magic = CoreMediaCodec::ReadU32LE(data, pos + 4);
        if (kv_len < 20 || pos + kv_len > total_len || kv_magic != magic::kKeyv) {
            break;
        }
        uint32_t k_len = CoreMediaCodec::ReadU32LE(data, pos + 8);
        uint32_t k_magic = CoreMediaCodec::ReadU32LE(data, pos + 12);
        if (k_magic == magic::kIdxk && k_len == 10 && pos + 8 + k_len + 8 <= pos + kv_len) {
            // Note: idxk uses 2-byte uint16 index (total key atom length = 10) or 4-byte index (length = 12)
            uint32_t idx_key = (data[pos + 16]) | (static_cast<uint32_t>(data[pos + 17]) << 8);
            size_t val_off = pos + 8 + k_len;
            uint32_t v_len = CoreMediaCodec::ReadU32LE(data, val_off);
            uint32_t v_magic = CoreMediaCodec::ReadU32LE(data, val_off + 4);
            if (v_len >= 8 && val_off + v_len <= pos + kv_len) {
                if (idx_key == 105 && v_magic == magic::kDatv) {
                    return std::vector<uint8_t>(data.begin() + val_off + 8, data.begin() + val_off + v_len);
                }
                if (v_magic == magic::kDict) {
                    auto nested = FindAvcCInIndexDict(data.subspan(val_off, v_len), magic::kDict);
                    if (nested.has_value()) {
                        return nested;
                    }
                }
            }
        } else if (k_magic == magic::kIdxk && k_len == 12 && pos + 8 + k_len + 8 <= pos + kv_len) {
            uint32_t idx_key = CoreMediaCodec::ReadU32LE(data, pos + 16);
            size_t val_off = pos + 8 + k_len;
            uint32_t v_len = CoreMediaCodec::ReadU32LE(data, val_off);
            uint32_t v_magic = CoreMediaCodec::ReadU32LE(data, val_off + 4);
            if (v_len >= 8 && val_off + v_len <= pos + kv_len) {
                if (idx_key == 105 && v_magic == magic::kDatv) {
                    return std::vector<uint8_t>(data.begin() + val_off + 8, data.begin() + val_off + v_len);
                }
                if (v_magic == magic::kDict) {
                    auto nested = FindAvcCInIndexDict(data.subspan(val_off, v_len), magic::kDict);
                    if (nested.has_value()) {
                        return nested;
                    }
                }
            }
        } else if (k_magic == magic::kStrk && k_len >= 8 && pos + 8 + k_len + 8 <= pos + kv_len) {
            std::string key_str(reinterpret_cast<const char*>(data.data() + pos + 16), k_len - 8);
            size_t val_off = pos + 8 + k_len;
            uint32_t v_len = CoreMediaCodec::ReadU32LE(data, val_off);
            uint32_t v_magic = CoreMediaCodec::ReadU32LE(data, val_off + 4);
            if (v_len >= 8 && val_off + v_len <= pos + kv_len) {
                if (key_str == "avcC" && v_magic == magic::kDatv) {
                    return std::vector<uint8_t>(data.begin() + val_off + 8, data.begin() + val_off + v_len);
                }
                if (v_magic == magic::kDict) {
                    auto nested = FindAvcCInIndexDict(data.subspan(val_off, v_len), magic::kDict);
                    if (nested.has_value()) {
                        return nested;
                    }
                }
            }
        }
        pos += kv_len;
    }
    return std::nullopt;
}

}  // namespace

void CoreMediaCodec::WriteU32LE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFU));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFU));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFU));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFU));
}

void CoreMediaCodec::WriteU64LE(std::vector<uint8_t>& out, uint64_t v) {
    WriteU32LE(out, static_cast<uint32_t>(v & 0xFFFFFFFFULL));
    WriteU32LE(out, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFULL));
}

void CoreMediaCodec::WriteF64LE(std::vector<uint8_t>& out, double v) {
    uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    WriteU64LE(out, bits);
}

uint32_t CoreMediaCodec::ReadU32LE(std::span<const uint8_t> buf, size_t offset) {
    if (offset + 4 > buf.size()) {
        return 0;
    }
    return static_cast<uint32_t>(buf[offset]) |
           (static_cast<uint32_t>(buf[offset + 1]) << 8) |
           (static_cast<uint32_t>(buf[offset + 2]) << 16) |
           (static_cast<uint32_t>(buf[offset + 3]) << 24);
}

uint64_t CoreMediaCodec::ReadU64LE(std::span<const uint8_t> buf, size_t offset) {
    if (offset + 8 > buf.size()) {
        return 0;
    }
    return static_cast<uint64_t>(ReadU32LE(buf, offset)) |
           (static_cast<uint64_t>(ReadU32LE(buf, offset + 4)) << 32);
}

double CoreMediaCodec::ReadF64LE(std::span<const uint8_t> buf, size_t offset) {
    uint64_t bits = ReadU64LE(buf, offset);
    double v = 0.0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

std::vector<uint8_t> CoreMediaCodec::SerializeCMTime(const CMTime& t) {
    std::vector<uint8_t> out;
    out.reserve(24);
    WriteU64LE(out, t.value);
    WriteU32LE(out, t.timescale);
    WriteU32LE(out, t.flags);
    WriteU64LE(out, t.epoch);
    return out;
}

std::optional<CMTime> CoreMediaCodec::ParseCMTime(std::span<const uint8_t> data) {
    if (data.size() < 24) {
        return std::nullopt;
    }
    CMTime t;
    t.value = ReadU64LE(data, 0);
    t.timescale = ReadU32LE(data, 8);
    t.flags = ReadU32LE(data, 12);
    t.epoch = ReadU64LE(data, 16);
    return t;
}

std::vector<uint8_t> CoreMediaCodec::SerializeNSNumber(const NSNumber& num) {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(num.kind));
    switch (num.kind) {
        case NSNumber::Kind::UInt32:
        case NSNumber::Kind::SInt32:
            WriteU32LE(out, num.u32);
            break;
        case NSNumber::Kind::UInt64:
            WriteU64LE(out, num.u64);
            break;
        case NSNumber::Kind::Float64:
            WriteF64LE(out, num.f64);
            break;
    }
    return out;
}

std::optional<NSNumber> CoreMediaCodec::ParseNSNumber(std::span<const uint8_t> data) {
    if (data.empty()) {
        return std::nullopt;
    }
    uint8_t spec = data[0];
    switch (spec) {
        case 3:
        case 5: {
            if (data.size() < 5) {
                return std::nullopt;
            }
            NSNumber n;
            n.kind = static_cast<NSNumber::Kind>(spec);
            n.u32 = ReadU32LE(data, 1);
            return n;
        }
        case 4: {
            if (data.size() < 9) {
                return std::nullopt;
            }
            return NSNumber::FromUInt64(ReadU64LE(data, 1));
        }
        case 6: {
            if (data.size() < 9) {
                return std::nullopt;
            }
            return NSNumber::FromFloat64(ReadF64LE(data, 1));
        }
        default:
            return std::nullopt;
    }
}

std::vector<uint8_t> CoreMediaCodec::SerializeAudioStreamRangedDescription(const AudioStreamBasicDescription& asbd) {
    std::vector<uint8_t> out;
    out.reserve(56);
    WriteF64LE(out, asbd.sample_rate);
    WriteU32LE(out, asbd.format_id);
    WriteU32LE(out, asbd.format_flags);
    WriteU32LE(out, asbd.bytes_per_packet);
    WriteU32LE(out, asbd.frames_per_packet);
    WriteU32LE(out, asbd.bytes_per_frame);
    WriteU32LE(out, asbd.channels_per_frame);
    WriteU32LE(out, asbd.bits_per_channel);
    WriteU32LE(out, asbd.reserved);
    // AudioValueRange mSampleRateRange (min, max) = 16 bytes -> total 56 bytes
    WriteF64LE(out, asbd.sample_rate);
    WriteF64LE(out, asbd.sample_rate);
    return out;
}

std::optional<AudioStreamBasicDescription> CoreMediaCodec::ParseAudioStreamBasicDescription(
    std::span<const uint8_t> data) {
    if (data.size() < 40) {
        return std::nullopt;
    }
    AudioStreamBasicDescription asbd;
    asbd.sample_rate = ReadF64LE(data, 0);
    asbd.format_id = ReadU32LE(data, 8);
    asbd.format_flags = ReadU32LE(data, 12);
    asbd.bytes_per_packet = ReadU32LE(data, 16);
    asbd.frames_per_packet = ReadU32LE(data, 20);
    asbd.bytes_per_frame = ReadU32LE(data, 24);
    asbd.channels_per_frame = ReadU32LE(data, 28);
    asbd.bits_per_channel = ReadU32LE(data, 32);
    asbd.reserved = ReadU32LE(data, 36);
    return asbd;
}

std::vector<uint8_t> CoreMediaCodec::SerializeStringDict(const StringDict& dict) {
    std::vector<uint8_t> body;
    for (const auto& entry : dict.entries) {
        std::vector<uint8_t> key_bytes;
        WriteU32LE(key_bytes, static_cast<uint32_t>(entry.key.size() + 8));
        WriteU32LE(key_bytes, magic::kStrk);
        key_bytes.insert(key_bytes.end(), entry.key.begin(), entry.key.end());

        std::vector<uint8_t> val_bytes;
        if (std::holds_alternative<bool>(entry.value)) {
            WriteU32LE(val_bytes, 9U);
            WriteU32LE(val_bytes, magic::kBulv);
            val_bytes.push_back(std::get<bool>(entry.value) ? 1U : 0U);
        } else if (std::holds_alternative<NSNumber>(entry.value)) {
            auto nb = SerializeNSNumber(std::get<NSNumber>(entry.value));
            WriteU32LE(val_bytes, static_cast<uint32_t>(nb.size() + 8));
            WriteU32LE(val_bytes, magic::kNmbv);
            val_bytes.insert(val_bytes.end(), nb.begin(), nb.end());
        } else if (std::holds_alternative<std::string>(entry.value)) {
            const auto& s = std::get<std::string>(entry.value);
            WriteU32LE(val_bytes, static_cast<uint32_t>(s.size() + 8));
            WriteU32LE(val_bytes, magic::kStrv);
            val_bytes.insert(val_bytes.end(), s.begin(), s.end());
        } else if (std::holds_alternative<std::vector<uint8_t>>(entry.value)) {
            const auto& b = std::get<std::vector<uint8_t>>(entry.value);
            WriteU32LE(val_bytes, static_cast<uint32_t>(b.size() + 8));
            WriteU32LE(val_bytes, magic::kDatv);
            val_bytes.insert(val_bytes.end(), b.begin(), b.end());
        } else if (std::holds_alternative<StringDict>(entry.value)) {
            val_bytes = SerializeStringDict(std::get<StringDict>(entry.value));
        }

        uint32_t kv_len = static_cast<uint32_t>(8 + key_bytes.size() + val_bytes.size());
        WriteU32LE(body, kv_len);
        WriteU32LE(body, magic::kKeyv);
        body.insert(body.end(), key_bytes.begin(), key_bytes.end());
        body.insert(body.end(), val_bytes.begin(), val_bytes.end());
    }

    std::vector<uint8_t> out;
    out.reserve(8 + body.size());
    WriteU32LE(out, static_cast<uint32_t>(8 + body.size()));
    WriteU32LE(out, magic::kDict);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::optional<StringDict> CoreMediaCodec::ParseStringDict(std::span<const uint8_t> data) {
    if (data.size() < 8) {
        return std::nullopt;
    }
    uint32_t total_len = ReadU32LE(data, 0);
    uint32_t m = ReadU32LE(data, 4);
    if (m != magic::kDict || total_len < 8 || total_len > data.size()) {
        return std::nullopt;
    }
    StringDict dict;
    size_t pos = 8;
    while (pos + 8 <= total_len) {
        uint32_t kv_len = ReadU32LE(data, pos);
        uint32_t kv_magic = ReadU32LE(data, pos + 4);
        if (kv_magic != magic::kKeyv || kv_len < 16 || pos + kv_len > total_len) {
            return std::nullopt;
        }
        uint32_t k_len = ReadU32LE(data, pos + 8);
        uint32_t k_magic = ReadU32LE(data, pos + 12);
        if (k_magic != magic::kStrk || k_len < 8 || pos + 8 + k_len + 8 > pos + kv_len) {
            pos += kv_len;
            continue;
        }
        std::string key(reinterpret_cast<const char*>(data.data() + pos + 16), k_len - 8);
        size_t val_off = pos + 8 + k_len;
        uint32_t v_len = ReadU32LE(data, val_off);
        uint32_t v_magic = ReadU32LE(data, val_off + 4);
        if (v_len < 8 || val_off + v_len > pos + kv_len) {
            return std::nullopt;
        }
        auto v_span = data.subspan(val_off, v_len);
        if (v_magic == magic::kBulv && v_len >= 9) {
            dict.entries.push_back({key, v_span[8] != 0});
        } else if (v_magic == magic::kNmbv) {
            auto n = ParseNSNumber(v_span.subspan(8));
            if (n.has_value()) {
                dict.entries.push_back({key, *n});
            }
        } else if (v_magic == magic::kStrv) {
            dict.entries.push_back(
                {key, std::string(reinterpret_cast<const char*>(v_span.data() + 8), v_len - 8)});
        } else if (v_magic == magic::kDatv) {
            dict.entries.push_back({key, std::vector<uint8_t>(v_span.begin() + 8, v_span.end())});
        } else if (v_magic == magic::kDict) {
            auto sub = ParseStringDict(v_span);
            if (sub.has_value()) {
                dict.entries.push_back({key, *sub});
            }
        } else if (v_magic == magic::kFdsc) {
            auto fd = ParseFormatDescription(v_span);
            if (fd.has_value()) {
                dict.entries.push_back({key, *fd});
            }
        }
        pos += kv_len;
    }
    return dict;
}

std::optional<AvcDecoderConfig> CoreMediaCodec::ParseAvcC(std::span<const uint8_t> data) {
    // ISO/IEC 14496-15 AVCDecoderConfigurationRecord
    if (data.size() < 7 || data[0] != 1) {
        return std::nullopt;
    }
    AvcDecoderConfig cfg;
    cfg.nal_length_size = static_cast<uint8_t>((data[4] & 0x03U) + 1U);
    uint8_t num_sps = static_cast<uint8_t>(data[5] & 0x1FU);
    size_t pos = 6;
    for (uint8_t i = 0; i < num_sps; ++i) {
        if (pos + 2 > data.size()) {
            return std::nullopt;
        }
        uint16_t sps_len = static_cast<uint16_t>((static_cast<uint16_t>(data[pos]) << 8) | data[pos + 1]);
        pos += 2;
        if (pos + sps_len > data.size()) {
            return std::nullopt;
        }
        std::vector<uint8_t> sps(data.begin() + pos, data.begin() + pos + sps_len);
        auto dims = ParseH264SpsDimensions(sps);
        if (dims.has_value()) {
            cfg.width = dims->width;
            cfg.height = dims->height;
        }
        cfg.sps_list.push_back(std::move(sps));
        pos += sps_len;
    }
    if (pos < data.size()) {
        uint8_t num_pps = data[pos++];
        for (uint8_t i = 0; i < num_pps; ++i) {
            if (pos + 2 > data.size()) {
                return std::nullopt;
            }
            uint16_t pps_len = static_cast<uint16_t>((static_cast<uint16_t>(data[pos]) << 8) | data[pos + 1]);
            pos += 2;
            if (pos + pps_len > data.size()) {
                return std::nullopt;
            }
            cfg.pps_list.emplace_back(data.begin() + pos, data.begin() + pos + pps_len);
            pos += pps_len;
        }
    }
    return cfg;
}

std::optional<SpsDimensions> CoreMediaCodec::ParseH264SpsDimensions(std::span<const uint8_t> sps_nalu) {
    if (sps_nalu.empty()) {
        return std::nullopt;
    }
    size_t offset = 0;
    if (sps_nalu.size() >= 4 && sps_nalu[0] == 0 && sps_nalu[1] == 0 && sps_nalu[2] == 0 && sps_nalu[3] == 1) {
        offset = 4;
    } else if (sps_nalu.size() >= 3 && sps_nalu[0] == 0 && sps_nalu[1] == 0 && sps_nalu[2] == 1) {
        offset = 3;
    }
    if (offset >= sps_nalu.size() || (sps_nalu[offset] & 0x1FU) != 7U) {
        return std::nullopt;
    }
    auto rbsp = RemoveEmulationPreventionBytes(sps_nalu.subspan(offset + 1));
    if (rbsp.size() < 4) {
        return std::nullopt;
    }

    BitReader br(rbsp);
    uint32_t profile_idc = 0;
    uint32_t constraint_flags = 0;
    uint32_t level_idc = 0;
    uint32_t seq_parameter_set_id = 0;
    if (!br.ReadBits(8, profile_idc) || !br.ReadBits(8, constraint_flags) ||
        !br.ReadBits(8, level_idc) || !br.ReadUE(seq_parameter_set_id)) {
        return std::nullopt;
    }

    uint32_t chroma_format_idc = 1;
    uint32_t separate_colour_plane_flag = 0;
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 || profile_idc == 244 ||
        profile_idc == 44 || profile_idc == 83 || profile_idc == 86 || profile_idc == 118 ||
        profile_idc == 128 || profile_idc == 138 || profile_idc == 139 || profile_idc == 134 ||
        profile_idc == 135) {
        if (!br.ReadUE(chroma_format_idc)) {
            return std::nullopt;
        }
        if (chroma_format_idc == 3) {
            if (!br.ReadBit(separate_colour_plane_flag)) {
                return std::nullopt;
            }
        }
        uint32_t bit_depth_luma_minus8 = 0;
        uint32_t bit_depth_chroma_minus8 = 0;
        uint32_t qpprime_y_zero_transform_bypass_flag = 0;
        uint32_t seq_scaling_matrix_present_flag = 0;
        if (!br.ReadUE(bit_depth_luma_minus8) || !br.ReadUE(bit_depth_chroma_minus8) ||
            !br.ReadBit(qpprime_y_zero_transform_bypass_flag) ||
            !br.ReadBit(seq_scaling_matrix_present_flag)) {
            return std::nullopt;
        }
        if (seq_scaling_matrix_present_flag) {
            size_t count = (chroma_format_idc != 3) ? 8 : 12;
            for (size_t i = 0; i < count; ++i) {
                uint32_t present = 0;
                if (!br.ReadBit(present)) {
                    return std::nullopt;
                }
                if (present && !SkipScalingList(br, i < 6 ? 16 : 64)) {
                    return std::nullopt;
                }
            }
        }
    }

    uint32_t log2_max_frame_num_minus4 = 0;
    uint32_t pic_order_cnt_type = 0;
    if (!br.ReadUE(log2_max_frame_num_minus4) || !br.ReadUE(pic_order_cnt_type)) {
        return std::nullopt;
    }
    if (pic_order_cnt_type == 0) {
        uint32_t log2_max_poc_lsb_minus4 = 0;
        if (!br.ReadUE(log2_max_poc_lsb_minus4)) {
            return std::nullopt;
        }
    } else if (pic_order_cnt_type == 1) {
        uint32_t delta_pic_order_always_zero_flag = 0;
        int32_t offset_for_non_ref_pic = 0;
        int32_t offset_for_top_to_bottom_field = 0;
        uint32_t num_ref_frames_in_poc_cycle = 0;
        if (!br.ReadBit(delta_pic_order_always_zero_flag) || !br.ReadSE(offset_for_non_ref_pic) ||
            !br.ReadSE(offset_for_top_to_bottom_field) || !br.ReadUE(num_ref_frames_in_poc_cycle)) {
            return std::nullopt;
        }
        for (uint32_t i = 0; i < num_ref_frames_in_poc_cycle; ++i) {
            int32_t dummy = 0;
            if (!br.ReadSE(dummy)) {
                return std::nullopt;
            }
        }
    }

    uint32_t max_num_ref_frames = 0;
    uint32_t gaps_in_frame_num_value_allowed_flag = 0;
    uint32_t pic_width_in_mbs_minus1 = 0;
    uint32_t pic_height_in_map_units_minus1 = 0;
    uint32_t frame_mbs_only_flag = 0;
    if (!br.ReadUE(max_num_ref_frames) || !br.ReadBit(gaps_in_frame_num_value_allowed_flag) ||
        !br.ReadUE(pic_width_in_mbs_minus1) || !br.ReadUE(pic_height_in_map_units_minus1) ||
        !br.ReadBit(frame_mbs_only_flag)) {
        return std::nullopt;
    }
    if (!frame_mbs_only_flag) {
        uint32_t mb_adaptive_frame_field_flag = 0;
        if (!br.ReadBit(mb_adaptive_frame_field_flag)) {
            return std::nullopt;
        }
    }
    uint32_t direct_8x8_inference_flag = 0;
    uint32_t frame_cropping_flag = 0;
    if (!br.ReadBit(direct_8x8_inference_flag) || !br.ReadBit(frame_cropping_flag)) {
        return std::nullopt;
    }

    uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    if (frame_cropping_flag) {
        if (!br.ReadUE(crop_left) || !br.ReadUE(crop_right) || !br.ReadUE(crop_top) || !br.ReadUE(crop_bottom)) {
            return std::nullopt;
        }
    }

    uint32_t chroma_array_type = (separate_colour_plane_flag == 0) ? chroma_format_idc : 0;
    uint32_t crop_unit_x = 1;
    uint32_t crop_unit_y = 2 - frame_mbs_only_flag;
    if (chroma_array_type == 1) {
        crop_unit_x = 2;
        crop_unit_y = 2 * (2 - frame_mbs_only_flag);
    } else if (chroma_array_type == 2) {
        crop_unit_x = 2;
        crop_unit_y = 2 - frame_mbs_only_flag;
    }

    uint32_t raw_w = (pic_width_in_mbs_minus1 + 1U) * 16U;
    uint32_t raw_h = (2U - frame_mbs_only_flag) * (pic_height_in_map_units_minus1 + 1U) * 16U;
    uint32_t sub_w = crop_unit_x * (crop_left + crop_right);
    uint32_t sub_h = crop_unit_y * (crop_top + crop_bottom);
    if (sub_w >= raw_w || sub_h >= raw_h) {
        return std::nullopt;
    }

    return SpsDimensions{raw_w - sub_w, raw_h - sub_h};
}

std::vector<uint8_t> CoreMediaCodec::ConvertAvccToAnnexB(
    std::span<const uint8_t> avcc_sample,
    uint8_t nal_length_size,
    const AvcDecoderConfig* prepend_parameter_sets,
    bool* out_is_keyframe) {
    if (nal_length_size == 0 || nal_length_size > 4) {
        nal_length_size = 4;
    }

    bool is_idr = false;
    size_t scan = 0;
    while (scan + nal_length_size <= avcc_sample.size()) {
        uint32_t nalu_len = 0;
        for (uint8_t i = 0; i < nal_length_size; ++i) {
            nalu_len = (nalu_len << 8) | avcc_sample[scan + i];
        }
        scan += nal_length_size;
        if (nalu_len == 0 || scan + nalu_len > avcc_sample.size()) {
            break;
        }
        uint8_t nal_type = avcc_sample[scan] & 0x1FU;
        if (nal_type == 5U) {
            is_idr = true;
        }
        scan += nalu_len;
    }

    if (out_is_keyframe != nullptr) {
        *out_is_keyframe = is_idr;
    }

    static constexpr uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};
    std::vector<uint8_t> out;
    out.reserve(avcc_sample.size() + 128);

    if (prepend_parameter_sets != nullptr) {
        for (const auto& sps : prepend_parameter_sets->sps_list) {
            out.insert(out.end(), kStartCode, kStartCode + 4);
            out.insert(out.end(), sps.begin(), sps.end());
        }
        for (const auto& pps : prepend_parameter_sets->pps_list) {
            out.insert(out.end(), kStartCode, kStartCode + 4);
            out.insert(out.end(), pps.begin(), pps.end());
        }
    }

    size_t pos = 0;
    while (pos + nal_length_size <= avcc_sample.size()) {
        uint32_t nalu_len = 0;
        for (uint8_t i = 0; i < nal_length_size; ++i) {
            nalu_len = (nalu_len << 8) | avcc_sample[pos + i];
        }
        pos += nal_length_size;
        if (nalu_len == 0 || pos + nalu_len > avcc_sample.size()) {
            break;
        }
        out.insert(out.end(), kStartCode, kStartCode + 4);
        out.insert(out.end(), avcc_sample.begin() + pos, avcc_sample.begin() + pos + nalu_len);
        pos += nalu_len;
    }
    return out;
}

std::optional<FormatDescription> CoreMediaCodec::ParseFormatDescription(std::span<const uint8_t> data) {
    if (data.size() < 20) {
        return std::nullopt;
    }
    uint32_t total_len = ReadU32LE(data, 0);
    uint32_t m = ReadU32LE(data, 4);
    if (m != magic::kFdsc || total_len < 20 || total_len > data.size()) {
        return std::nullopt;
    }
    FormatDescription fd;
    size_t pos = 8;
    while (pos + 8 <= total_len) {
        uint32_t sub_len = ReadU32LE(data, pos);
        uint32_t sub_magic = ReadU32LE(data, pos + 4);
        if (sub_len < 8 || pos + sub_len > total_len) {
            break;
        }
        if (sub_magic == magic::kMdia && sub_len >= 12) {
            fd.media_type = ReadU32LE(data, pos + 8);
        } else if (sub_magic == magic::kVdim && sub_len >= 16) {
            fd.width = ReadU32LE(data, pos + 8);
            fd.height = ReadU32LE(data, pos + 12);
        } else if (sub_magic == magic::kCodc && sub_len >= 12) {
            fd.codec = ReadU32LE(data, pos + 8);
        } else if (sub_magic == magic::kAsbd && sub_len >= 48) {
            auto asbd = ParseAudioStreamBasicDescription(data.subspan(pos + 8, sub_len - 8));
            if (asbd.has_value()) {
                fd.asbd = *asbd;
            }
        } else if (sub_magic == magic::kExtn) {
            auto avcc_raw = FindAvcCInIndexDict(data.subspan(pos, sub_len), magic::kExtn);
            if (avcc_raw.has_value()) {
                auto parsed_avcc = ParseAvcC(*avcc_raw);
                if (parsed_avcc.has_value()) {
                    fd.avc_config = std::move(*parsed_avcc);
                    if (fd.avc_config.width > 0 && fd.avc_config.height > 0) {
                        fd.width = fd.avc_config.width;
                        fd.height = fd.avc_config.height;
                    }
                }
            }
        }
        pos += sub_len;
    }
    return fd;
}

std::optional<SampleBufferData> CoreMediaCodec::ParseSampleBuffer(
    std::span<const uint8_t> data,
    uint32_t media_type) {
    if (data.size() < 8) {
        return std::nullopt;
    }
    uint32_t total_len = ReadU32LE(data, 0);
    uint32_t m = ReadU32LE(data, 4);
    if (m != magic::kSbuf || total_len < 8 || total_len > data.size()) {
        return std::nullopt;
    }
    SampleBufferData sb;
    size_t pos = 8;
    while (pos + 8 <= total_len) {
        uint32_t atom_len = ReadU32LE(data, pos);
        uint32_t atom_magic = ReadU32LE(data, pos + 4);
        if (atom_len < 8 || pos + atom_len > total_len) {
            break;
        }
        if (atom_magic == magic::kOpts && atom_len >= 32) {
            auto t = ParseCMTime(data.subspan(pos + 8, 24));
            if (t.has_value()) {
                sb.pts = *t;
            }
        } else if (atom_magic == magic::kStia && atom_len >= 8 + 72) {
            // First CMSampleTimingInfo: Duration (24B), PresentationTimeStamp (24B), DecodeTimeStamp (24B)
            auto pts = ParseCMTime(data.subspan(pos + 8 + 24, 24));
            if (pts.has_value() && sb.pts.value == 0) {
                sb.pts = *pts;
            }
        } else if (atom_magic == magic::kNsmp && atom_len >= 12) {
            sb.num_samples = ReadU32LE(data, pos + 8);
        } else if (atom_magic == magic::kSsiz && atom_len >= 12) {
            size_t count = (atom_len - 8) / 4;
            sb.sample_sizes.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                sb.sample_sizes.push_back(ReadU32LE(data, pos + 8 + i * 4));
            }
        } else if (atom_magic == magic::kFdsc) {
            auto fd = ParseFormatDescription(data.subspan(pos, atom_len));
            if (fd.has_value()) {
                sb.has_format_description = true;
                sb.format_description = std::move(*fd);
            }
        } else if (atom_magic == magic::kSdat) {
            sb.sample_data.assign(data.begin() + pos + 8, data.begin() + pos + atom_len);
        }
        pos += atom_len;
    }
    (void)media_type;
    return sb;
}

std::vector<uint8_t> CoreMediaCodec::BuildPingPacket() {
    std::vector<uint8_t> out;
    out.reserve(16);
    WriteU32LE(out, 16U);
    WriteU32LE(out, magic::kPing);
    WriteU32LE(out, 0U);
    WriteU32LE(out, 1U);
    return out;
}

StringDict CoreMediaCodec::BuildHpd1DeviceInfoDict(bool valeria_clean_bar, uint32_t max_w, uint32_t max_h) {
    StringDict display_size;
    display_size.entries.push_back({"Width", NSNumber::FromFloat64(static_cast<double>(max_w))});
    display_size.entries.push_back({"Height", NSNumber::FromFloat64(static_cast<double>(max_h))});

    StringDict hpd1;
    hpd1.entries.push_back({"Valeria", valeria_clean_bar});
    hpd1.entries.push_back({"HEVCDecoderSupports444", true});
    hpd1.entries.push_back({"DisplaySize", display_size});
    return hpd1;
}

StringDict CoreMediaCodec::BuildHpa1DeviceInfoDict() {
    AudioStreamBasicDescription default_asbd;
    auto asbd_ranged = SerializeAudioStreamRangedDescription(default_asbd);

    StringDict hpa1;
    hpa1.entries.push_back({"BufferAheadInterval", NSNumber::FromFloat64(0.07300000000000001)});
    hpa1.entries.push_back({"deviceUID", std::string("Valeria")});
    hpa1.entries.push_back({"ScreenLatency", NSNumber::FromFloat64(0.04)});
    hpa1.entries.push_back({"formats", std::move(asbd_ranged)});
    hpa1.entries.push_back({"EDIDAC3Support", NSNumber::FromUInt32(0)});
    hpa1.entries.push_back({"deviceName", std::string("Valeria")});
    return hpa1;
}

std::vector<uint8_t> CoreMediaCodec::BuildAsynDictPacket(
    uint32_t subtype,
    uint64_t clock_ref,
    const StringDict& dict) {
    auto dict_bytes = SerializeStringDict(dict);
    std::vector<uint8_t> out;
    out.reserve(20 + dict_bytes.size());
    WriteU32LE(out, static_cast<uint32_t>(20 + dict_bytes.size()));
    WriteU32LE(out, magic::kAsyn);
    WriteU64LE(out, clock_ref);
    WriteU32LE(out, subtype);
    out.insert(out.end(), dict_bytes.begin(), dict_bytes.end());
    return out;
}

std::vector<uint8_t> CoreMediaCodec::BuildAsynNeedPacket(uint64_t video_device_clock_ref) {
    std::vector<uint8_t> out;
    out.reserve(20);
    WriteU32LE(out, 20U);
    WriteU32LE(out, magic::kAsyn);
    WriteU64LE(out, video_device_clock_ref);
    WriteU32LE(out, magic::kNeed);
    return out;
}

std::vector<uint8_t> CoreMediaCodec::BuildAsynHpd0Packet() {
    std::vector<uint8_t> out;
    out.reserve(20);
    WriteU32LE(out, 20U);
    WriteU32LE(out, magic::kAsyn);
    WriteU64LE(out, magic::kEmptyCFType);
    WriteU32LE(out, magic::kHpd0);
    return out;
}

std::vector<uint8_t> CoreMediaCodec::BuildAsynHpa0Packet(uint64_t audio_device_clock_ref) {
    std::vector<uint8_t> out;
    out.reserve(20);
    WriteU32LE(out, 20U);
    WriteU32LE(out, magic::kAsyn);
    WriteU64LE(out, audio_device_clock_ref);
    WriteU32LE(out, magic::kHpa0);
    return out;
}

std::vector<uint8_t> CoreMediaCodec::BuildRplyClockRef(uint64_t correlation_id, uint64_t local_clock_ref) {
    std::vector<uint8_t> out;
    out.reserve(28);
    WriteU32LE(out, 28U);
    WriteU32LE(out, magic::kRply);
    WriteU64LE(out, correlation_id);
    WriteU32LE(out, 0U);
    WriteU64LE(out, local_clock_ref);
    return out;
}

std::vector<uint8_t> CoreMediaCodec::BuildRplyAfmtOk(uint64_t correlation_id) {
    StringDict dict;
    dict.entries.push_back({"Error", NSNumber::FromUInt32(0)});
    auto dict_bytes = SerializeStringDict(dict);

    std::vector<uint8_t> out;
    out.reserve(20 + dict_bytes.size());
    WriteU32LE(out, static_cast<uint32_t>(20 + dict_bytes.size()));
    WriteU32LE(out, magic::kRply);
    WriteU64LE(out, correlation_id);
    WriteU32LE(out, 0U);
    out.insert(out.end(), dict_bytes.begin(), dict_bytes.end());
    return out;
}

std::vector<uint8_t> CoreMediaCodec::BuildRplyCMTime(uint64_t correlation_id, const CMTime& time) {
    auto t_bytes = SerializeCMTime(time);
    std::vector<uint8_t> out;
    out.reserve(20 + t_bytes.size());
    WriteU32LE(out, static_cast<uint32_t>(20 + t_bytes.size()));
    WriteU32LE(out, magic::kRply);
    WriteU64LE(out, correlation_id);
    WriteU32LE(out, 0U);
    out.insert(out.end(), t_bytes.begin(), t_bytes.end());
    return out;
}

std::vector<uint8_t> CoreMediaCodec::BuildRplySkew(uint64_t correlation_id, double skew_hz) {
    std::vector<uint8_t> out;
    out.reserve(28);
    // Note: SKEW RPLY is 28 bytes: 4 len + 4 'rply' + 8 corrId + 4 zero + 8 float64 (48000.0)
    WriteU32LE(out, 28U);
    WriteU32LE(out, magic::kRply);
    WriteU64LE(out, correlation_id);
    WriteU32LE(out, 0U);
    WriteF64LE(out, skew_hz);
    return out;
}

std::vector<uint8_t> CoreMediaCodec::BuildRplyZero64(uint64_t correlation_id) {
    std::vector<uint8_t> out;
    out.reserve(24);
    WriteU32LE(out, 24U);
    WriteU32LE(out, magic::kRply);
    WriteU64LE(out, correlation_id);
    WriteU64LE(out, 0ULL);
    return out;
}

}  // namespace ios_vcam
