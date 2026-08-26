// RtpPacketizer.h
// RTP 打包器头文件。
// 将编码后的 H.264 帧（Annex-B 格式）按 RTP 协议拆分为多个 RTP 包，
// 支持“单 NAL 直接打包”和“FU-A 分片打包”两种模式。

#ifndef SSERVER_MODULES_TRANSPORT_RTP_RTPPACKETIZER_H
#define SSERVER_MODULES_TRANSPORT_RTP_RTPPACKETIZER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/net/H264AnnexB.h"
#include "common/net/RtpProtocol.h"
#include "common/model/EncodedFrame.h"

namespace sserver {
namespace modules {
namespace transport {
namespace rtp {

// RTP 打包器：将编码后的 H264 帧按 RTP 协议拆分为多个 packet
// 支持单 NAL 直接打包和 FU-A 分片打包两种模式
class RtpPacketizer {
public:
    // 构造参数：
    // payload_type          - RTP 载荷类型（如 96）；
    // clock_rate            - RTP 时间戳时钟频率（如 90000 Hz）；
    // max_payload_size      - 单个 RTP 包最大载荷字节数（不含 RTP 头）；
    // ssrc                  - 同步源标识；
    // enable_latency_extension - 是否携带自定义延迟统计扩展头。
    RtpPacketizer(
            std::uint8_t payload_type,
            std::uint32_t clock_rate,
            std::size_t max_payload_size,
            std::uint32_t ssrc,
            bool enable_latency_extension);

    // 将一帧编码数据打包为多个 RTP 包。
    // 成功时返回 true，packets 中为完整的一组 RTP 包。
    bool Packetize(
            common::model::EncodedFramePtr frame,
            std::vector<std::vector<std::uint8_t> > *packets,
            std::string *error_message);

private:
    // 根据帧的采集时间戳计算 RTP 时间戳（纳秒 -> RTP 时钟单位）。
    std::uint32_t BuildTimestamp(const common::model::EncodedFrame &frame) const;

    // 单个 NAL 单元直接打包（NAL 尺寸 <= 最大载荷时使用）。
    bool PacketizeSingleNalu(
            const std::uint8_t *nalu_data,
            std::size_t nalu_size,
            bool marker,
            std::uint32_t timestamp,
            const common::net::RtpHeaderExtension *header_extension,
            std::vector<std::vector<std::uint8_t> > *packets);

    // FU-A 分片打包：将超长 NAL 拆成多个 RTP 包。
    bool PacketizeFragmentedNalu(
            const std::uint8_t *nalu_data,
            std::size_t nalu_size,
            bool marker,
            std::uint32_t timestamp,
            const common::net::RtpHeaderExtension *header_extension,
            std::vector<std::vector<std::uint8_t> > *packets,
            std::string *error_message);

private:
    std::uint8_t payload_type_;        // RTP 载荷类型
    std::uint32_t clock_rate_;         // 时间戳时钟频率
    std::size_t max_payload_size_;     // 单包最大载荷
    std::uint32_t ssrc_;               // 同步源标识
    bool enable_latency_extension_;    // 是否启用延迟扩展头
    std::uint16_t sequence_number_;    // RTP 包序号（逐包自增，从随机值开始）
    std::vector<common::net::H264NaluView> nalus_;  // 当前帧拆分出的 NAL 单元列表（复用缓冲）
};

}  // namespace rtp
}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_RTP_RTPPACKETIZER_H
