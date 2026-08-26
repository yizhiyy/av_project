// RtpStreamingBackend.h
// RTP 传输后端头文件。
// 通过 UDP 套接字将编码帧按 RTP 协议发送到指定接收端，
// 发送前由 RtpPacketizer 完成 H.264 打包，并对多包帧做限速（pacing）发送。

#ifndef SSERVER_MODULES_TRANSPORT_RTP_RTPSTREAMINGBACKEND_H
#define SSERVER_MODULES_TRANSPORT_RTP_RTPSTREAMINGBACKEND_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <netinet/in.h>

#include "common/metrics/LatencyRecorder.h"
#include "config/AppConfig.h"
#include "modules/transport/ITransportBackend.h"
#include "modules/transport/rtp/RtpPacketizer.h"

namespace sserver {
namespace modules {
namespace transport {
namespace rtp {

// RtpStreamingBackend：RTP 传输后端的实现。
// 工作流程：初始化（保存配置）-> 启动（配置远端地址、打开 UDP 套接字、创建打包器、写 SDP）
//          -> Broadcast（打包 + 限速发送 + 延迟统计）-> 停止/关闭。
class RtpStreamingBackend : public ITransportBackend {
public:
    explicit RtpStreamingBackend(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder);
    ~RtpStreamingBackend() override;

    bool initialize(const core::ApplicationContext &context) override;
    bool start() override;
    void stop() override;
    void shutdown() override;
    core::ModuleState state() const override;
    void Broadcast(common::model::EncodedFramePtr frame) override;
    int bound_port() const override;
    TransportBackend backend() const override;
    const std::string &backend_name() const override;

private:
    // 创建并绑定 UDP 套接字。
    bool OpenSocket();

    // 关闭并释放套接字。
    void CloseSocket();

    // 根据配置解析 RTP 接收端地址。
    bool ConfigureRemoteAddress();

    // 将会话参数写入 SDP 文件（供 VLC/ffplay 等播放器直接打开）。
    bool WriteSdpFile() const;

    // 计算一帧内多个 RTP 包之间的发送间隔（pacing）。
    std::chrono::nanoseconds ComputePacingInterval(
            std::size_t packet_count,
            std::uint64_t current_capture_timestamp_ns);

    // 按计算出的间隔精确休眠，均匀发送单个包。
    void PacePacketBurst(
            std::size_t packet_index,
            std::chrono::steady_clock::time_point frame_send_start,
            std::chrono::nanoseconds pacing_interval) const;

private:
    config::TransportConfig config_;            // 传输配置
    std::string backend_name_;                  // 后端名称（"rtp"）
    int latency_log_interval_frames_;           // 延迟统计日志的帧间隔
    int socket_fd_;                             // UDP 套接字描述符，未打开时为 -1
    int bound_port_;                            // 实际绑定的本地端口
    sockaddr_in remote_address_;                // RTP 接收端地址
    bool has_remote_address_;                   // 远端地址是否已成功解析
    std::shared_ptr<common::metrics::LatencyRecorder> send_latency_recorder_;  // 发送延迟统计器
    std::unique_ptr<RtpPacketizer> packetizer_; // RTP 打包器
    std::atomic<core::ModuleState> state_;      // 模块状态
    std::uint64_t sent_frames_;                 // 已成功发送的帧数
    std::uint64_t sent_packets_;                // 已发送的 RTP 包数
    std::uint64_t failed_frames_;               // 发送/打包失败的帧数
    std::uint64_t fallback_frame_interval_ns_;  // 回退使用的帧间隔（纳秒）
    std::uint64_t previous_frame_capture_timestamp_ns_;  // 上一帧采集时间戳（用于计算实际帧间隔）
};

}  // namespace rtp
}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_RTP_RTPSTREAMINGBACKEND_H
