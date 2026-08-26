#include "modules/transport/rtp/RtpPacketizer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <random>

#include "common/net/H264AnnexB.h"
#include "common/net/RtpProtocol.h"

namespace sserver {
namespace modules {
namespace transport {
namespace rtp {

namespace {

// 生成随机的初始 RTP 序列号：
// 以当前时间和 SSRC 混合作为随机种子，避免多个发送端从相同序号开始。
std::uint16_t BuildInitialSequenceNumber(std::uint32_t ssrc) {
    const std::uint64_t now_ns = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    const std::uint32_t seed = static_cast<std::uint32_t>(now_ns & 0xFFFFFFFFu) ^
                               static_cast<std::uint32_t>(now_ns >> 32) ^
                               ssrc;
    std::mt19937 generator(seed);
    std::uniform_int_distribution<std::uint32_t> distribution(
            0,
            static_cast<std::uint32_t>(std::numeric_limits<std::uint16_t>::max()));
    return static_cast<std::uint16_t>(distribution(generator));
}

// 将 RTP 头字段直接写入缓冲区（不依赖 WriteRtpHeader 的动态扩容路径）。
// 用于 FU-A 分片时一次性构建完整包，减少中间拷贝。
void WriteRtpHeaderToBuffer(
        std::uint8_t *buffer,
        const common::net::RtpHeaderFields &fields,
        const common::net::RtpHeaderExtension *extension) {
    const bool has_extension = extension != nullptr && !extension->payload.empty();
    // 第一个字节：RTP 版本（2 位） + 扩展位（1 位）+ 其余保留位。
    buffer[0] = static_cast<std::uint8_t>((common::net::kRtpVersion << 6) | (has_extension ? 0x10 : 0x00));
    // 第二个字节：标记位（M）+ 载荷类型（7 位）。
    buffer[1] = static_cast<std::uint8_t>((fields.marker ? 0x80 : 0x00) | (fields.payload_type & 0x7F));
    common::net::WriteBe16(fields.sequence_number, buffer + 2);   // 序列号（大端）
    common::net::WriteBe32(fields.timestamp, buffer + 4);         // 时间戳（大端）
    common::net::WriteBe32(fields.ssrc, buffer + 8);              // SSRC（大端）
    if (has_extension) {
        // 扩展头：profile id + 长度（以 32 位字为单位）+ 扩展载荷。
        common::net::WriteBe16(extension->profile_id, buffer + common::net::kRtpHeaderSize);
        common::net::WriteBe16(
                static_cast<std::uint16_t>(extension->payload.size() / 4),
                buffer + common::net::kRtpHeaderSize + 2);
        std::copy(extension->payload.begin(), extension->payload.end(),
                  buffer + common::net::kRtpHeaderSize + 4);
    }
}

// 构建一个完整的 RTP 包：RTP 头（含可选扩展）+ 载荷。
void BuildRtpPacketInto(
        const common::net::RtpHeaderFields &header_fields,
        const common::net::RtpHeaderExtension *header_extension,
        const std::uint8_t *payload_data,
        std::size_t payload_size,
        std::vector<std::uint8_t> *output) {
    if (!common::net::WriteRtpHeader(header_fields, header_extension, output)) {
        return;
    }
    output->insert(output->end(), payload_data, payload_data + payload_size);
}

}  // namespace

// 构造函数：保存打包参数，并生成随机的初始序列号。
RtpPacketizer::RtpPacketizer(
        std::uint8_t payload_type,
        std::uint32_t clock_rate,
        std::size_t max_payload_size,
        std::uint32_t ssrc,
        bool enable_latency_extension)
        : payload_type_(payload_type),
          clock_rate_(clock_rate),
          max_payload_size_(max_payload_size),
          ssrc_(ssrc),
          enable_latency_extension_(enable_latency_extension),
          sequence_number_(BuildInitialSequenceNumber(ssrc)) {
}

// 将一帧 H.264 编码数据打包为 RTP 包序列。
bool RtpPacketizer::Packetize(
        common::model::EncodedFramePtr frame,
        std::vector<std::vector<std::uint8_t> > *packets,
        std::string *error_message) {
    // 基本参数校验。
    if (!frame || packets == nullptr) {
        if (error_message != nullptr) {
            *error_message = "frame or packet output is null";
        }
        return false;
    }
    // 当前只支持视频帧打包。
    if (frame->type != common::model::StreamPayloadType::kVideo) {
        if (error_message != nullptr) {
            *error_message = "rtp packetizer currently supports video only";
        }
        return false;
    }
    // FU-A 至少需要 2 字节开销，载荷过小无法工作。
    if (max_payload_size_ <= 2) {
        if (error_message != nullptr) {
            *error_message = "rtp max payload size is too small";
        }
        return false;
    }

    // 将 Annex-B 码流拆分为 NAL 单元列表。
    common::net::SplitAnnexBNalus(frame->payload, &nalus_);
    if (nalus_.empty()) {
        if (error_message != nullptr) {
            *error_message = "frame payload does not contain Annex-B H264 NAL units";
        }
        return false;
    }

    // 保留调用方容器的容量，避免清空后频繁扩容。
    const std::size_t saved_capacity = packets->capacity();
    packets->clear();
    if (saved_capacity > packets->capacity()) {
        packets->reserve(saved_capacity);
    }

    // 计算本帧统一的 RTP 时间戳。
    const std::uint32_t timestamp = BuildTimestamp(*frame);
    // 可选：构建携带采集时间戳的 RTP 扩展头，用于端到端延迟统计。
    common::net::RtpHeaderExtension header_extension;
    const common::net::RtpHeaderExtension *header_extension_ptr = nullptr;
    if (enable_latency_extension_) {
        header_extension = common::net::BuildRtpLatencyHeaderExtension(
                common::net::RtpLatencyExtension{frame->capture_timestamp_ns, 0});
        header_extension_ptr = &header_extension;
    }

    // 逐个 NAL 打包；最后一个 NAL 的最后一个包置 M（marker）位。
    for (std::size_t index = 0; index < nalus_.size(); ++index) {
        const common::net::H264NaluView &nalu = nalus_[index];
        const bool marker = index + 1 == nalus_.size();
        if (nalu.size <= max_payload_size_) {
            // 单个 NAL 直接打包。
            if (!PacketizeSingleNalu(nalu.data, nalu.size, marker, timestamp, header_extension_ptr, packets)) {
                if (error_message != nullptr) {
                    *error_message = "failed to packetize single RTP/H264 NAL unit";
                }
                packets->clear();
                return false;
            }
            continue;
        }

        // 超长 NAL 使用 FU-A 分片打包。
        if (!PacketizeFragmentedNalu(
                    nalu.data,
                    nalu.size,
                    marker,
                    timestamp,
                    header_extension_ptr,
                    packets,
                    error_message)) {
            packets->clear();
            return false;
        }
    }

    return !packets->empty();
}

// 计算 RTP 时间戳：采集纳秒时间 * 时钟频率 / 1e9，取低 32 位。
// 无采集时间戳时回退用帧序号，保证时间戳单调可辨识。
std::uint32_t RtpPacketizer::BuildTimestamp(const common::model::EncodedFrame &frame) const {
    if (frame.capture_timestamp_ns == 0) {
        return static_cast<std::uint32_t>(frame.sequence & 0xFFFFFFFFu);
    }

    const std::uint64_t timestamp =
            (frame.capture_timestamp_ns * static_cast<std::uint64_t>(clock_rate_)) / 1000000000ULL;
    return static_cast<std::uint32_t>(timestamp & 0xFFFFFFFFu);
}

// 单 NAL 打包：整个 NAL 作为 RTP 载荷，头部填入 marker/序号/时间戳/SSRC。
bool RtpPacketizer::PacketizeSingleNalu(
        const std::uint8_t *nalu_data,
        std::size_t nalu_size,
        bool marker,
        std::uint32_t timestamp,
        const common::net::RtpHeaderExtension *header_extension,
        std::vector<std::vector<std::uint8_t> > *packets) {
    common::net::RtpHeaderFields header_fields;
    header_fields.marker = marker;
    header_fields.payload_type = payload_type_;
    header_fields.sequence_number = sequence_number_++;
    header_fields.timestamp = timestamp;
    header_fields.ssrc = ssrc_;

    packets->emplace_back();
    std::vector<std::uint8_t> &packet = packets->back();
    BuildRtpPacketInto(header_fields, header_extension, nalu_data, nalu_size, &packet);
    if (packet.empty()) {
        packets->pop_back();
        return false;
    }
    return true;
}

// FU-A 分片打包：将超过 max_payload_size 的 NAL 单元拆分为多个 RTP packet
// 每个分片包含 FU indicator + FU header（标识 start/end 位和原始 NAL type）
bool RtpPacketizer::PacketizeFragmentedNalu(
        const std::uint8_t *nalu_data,
        std::size_t nalu_size,
        bool marker,
        std::uint32_t timestamp,
        const common::net::RtpHeaderExtension *header_extension,
        std::vector<std::vector<std::uint8_t> > *packets,
        std::string *error_message) {
    // NAL 至少需要 1 字节头 + 1 字节数据才能分片。
    if (nalu_data == nullptr || nalu_size < 2) {
        if (error_message != nullptr) {
            *error_message = "cannot fragment empty H264 NAL unit";
        }
        return false;
    }

    // FU-A 每个分片额外占用 2 字节（FU indicator + FU header）。
    const std::size_t fragment_payload_capacity = max_payload_size_ - 2;
    if (fragment_payload_capacity == 0) {
        if (error_message != nullptr) {
            *error_message = "rtp max payload size is too small for FU-A";
        }
        return false;
    }

    // FU indicator 保留原 NAL 头的高 3 位（F + NRI），类型改为 28（FU-A）。
    const std::uint8_t nalu_header = nalu_data[0];
    const std::uint8_t fu_indicator = static_cast<std::uint8_t>((nalu_header & 0xE0) | common::net::kH264FuAType);
    // 原始 NAL 类型写入 FU header 的低 5 位。
    const std::uint8_t nal_type = static_cast<std::uint8_t>(nalu_header & 0x1F);

    const std::uint8_t fu_header_base = nal_type;

    // 从 NAL 数据偏移 1 开始（跳过 NAL 头），按块发送。
    std::size_t offset = 1;
    while (offset < nalu_size) {
        const std::size_t remaining = nalu_size - offset;
        const std::size_t chunk_size = std::min(fragment_payload_capacity, remaining);
        const bool start = offset == 1;    // 首分片
        const bool end = offset + chunk_size >= nalu_size;  // 末分片
        const std::uint8_t fu_header = static_cast<std::uint8_t>(
                (start ? 0x80 : 0x00) | (end ? 0x40 : 0x00) | fu_header_base);

        // 只有最后一个分片才置 marker 位。
        common::net::RtpHeaderFields header_fields;
        header_fields.marker = marker && end;
        header_fields.payload_type = payload_type_;
        header_fields.sequence_number = sequence_number_++;
        header_fields.timestamp = timestamp;
        header_fields.ssrc = ssrc_;

        packets->emplace_back();
        std::vector<std::uint8_t> &packet = packets->back();

        // 一次性构建 RTP header + FU indicator + FU header + NAL 数据块，减少拷贝次数
        const bool has_ext = header_extension != nullptr && !header_extension->payload.empty();
        const std::size_t ext_size = has_ext ? (common::net::kRtpExtensionHeaderSize + header_extension->payload.size()) : 0;
        const std::size_t hdr_size = common::net::kRtpHeaderSize + ext_size;
        const std::size_t total = hdr_size + 2 + chunk_size;
        packet.resize(total);
        WriteRtpHeaderToBuffer(packet.data(), header_fields, header_extension);
        packet[hdr_size] = fu_indicator;
        packet[hdr_size + 1] = fu_header;
        std::memcpy(packet.data() + hdr_size + 2, nalu_data + offset, chunk_size);

        if (packet.empty()) {
            packets->pop_back();
            if (error_message != nullptr) {
                *error_message = "failed to build RTP packet with latency extension";
            }
            return false;
        }
        offset += chunk_size;
    }

    return true;
}

}  // namespace rtp
}  // namespace transport
}  // namespace modules
}  // namespace sserver
