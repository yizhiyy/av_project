// UdpStreamingBackend.h
// UDP 传输后端头文件。
// 通过 UDP 数据报向已注册的接收端发送视频帧，支持：
//   - 大帧分片（UdpFrameFragmentHeader 协议头）；
//   - 多客户端批量发送（sendmmsg）；
//   - FEC 前向纠错（XOR 奇偶校验分片）；
//   - NACK 重传（缓存已发分片，响应客户端丢包请求）。

#ifndef SSERVER_MODULES_TRANSPORT_UDP_UDPSTREAMINGBACKEND_H
#define SSERVER_MODULES_TRANSPORT_UDP_UDPSTREAMINGBACKEND_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>

#include "common/metrics/LatencyRecorder.h"
#include "common/net/StreamProtocol.h"
#include "config/AppConfig.h"
#include "modules/transport/ITransportBackend.h"

namespace sserver {
namespace modules {
namespace transport {
namespace udp {

// UdpStreamingBackend：UDP 传输后端的实现。
class UdpStreamingBackend : public ITransportBackend {
public:
    explicit UdpStreamingBackend(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder);
    ~UdpStreamingBackend() override;

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
    // 一个 UDP 接收端（客户端）的注册信息。
    struct UdpClientEndpoint {
        sockaddr_in address;                       // 客户端地址（IP:端口）
        std::uint64_t last_seen_ns;                // 最近一次收到保活消息的时间（单调时钟）
        common::net::UdpReceiverReport latest_report;  // 客户端最近上报的接收统计
        std::uint64_t latest_report_timestamp_ns;  // 最近一次上报时间
        bool has_report;                           // 是否已收到过接收报告
    };

    // 缓存的单个 UDP 分片（用于 NACK 重传）。
    struct CachedUdpFragment {
        std::uint16_t fragment_index = 0;   // 分片序号
        std::size_t datagram_size = 0;      // 数据报总字节数
        std::vector<char> datagram;         // 完整数据报内容（头 + 分片头 + 载荷）
    };

    // 缓存的一帧全部分片（用于 NACK 重传）。
    struct CachedUdpFrame {
        std::uint64_t frame_sequence = 0;   // 帧序号
        std::uint64_t cached_timestamp_ns = 0;  // 缓存时刻（用于过期清理）
        std::vector<CachedUdpFragment> fragments;  // 帧内的全部分片
    };

    // 创建并绑定 UDP 套接字。
    bool OpenSocket();

    // 关闭套接字。
    void CloseSocket();

    // 接收线程：处理客户端的保活消息、接收报告与 NACK 请求。
    void ReceiveLoop();

    // 注册/刷新客户端；可附带最新接收报告。
    void RegisterClient(
            const sockaddr_in &address,
            std::uint64_t now_ns,
            const common::net::UdpReceiverReport *report);

    // 清理超时未上报的客户端（须持有 clients_mutex_）。
    void PruneStaleClientsLocked(std::uint64_t now_ns);

    // 获取当前有效客户端的地址快照。
    std::vector<sockaddr_in> SnapshotClients(std::uint64_t now_ns);

    // 将一帧拆分为多个 UDP 分片发送给所有客户端（含 FEC/缓存逻辑）。
    bool SendFrameFragments(common::model::EncodedFramePtr frame, const std::vector<sockaddr_in> &clients);

    // 把一帧的分片放入重传缓存。
    void CacheFrameFragments(const CachedUdpFrame &frame);

    // 清理过期的重传缓存（须持有 retransmit_cache_mutex_）。
    void PruneRetransmitCacheLocked(std::uint64_t now_ns);

    // 处理客户端的 NACK 请求：从缓存中找到分片并重发。
    void HandleNackRequest(
            const sockaddr_in &address,
            std::uint64_t now_ns,
            const common::net::UdpNackHeader &nack_header,
            const std::vector<common::net::UdpNackItem> &nack_items);

    // 格式化地址为 "ip:port"。
    std::string FormatEndpoint(const sockaddr_in &address) const;

    // 格式化客户端接收报告（用于日志）。
    std::string FormatClientReport(const UdpClientEndpoint &client) const;

    // 判断两个端点是否相同（IP + 端口）。
    bool SameEndpoint(const sockaddr_in &lhs, const sockaddr_in &rhs) const;

private:
    config::TransportConfig config_;            // 传输配置
    std::string backend_name_;                  // 后端名称（"udp"）
    int latency_log_interval_frames_;           // 延迟统计日志的帧间隔
    int socket_fd_;                             // UDP 套接字描述符，未打开时为 -1
    int bound_port_;                            // 实际绑定的本地端口
    std::atomic_bool running_;                  // 接收线程运行标志
    std::thread receive_thread_;                // 接收线程
    std::mutex clients_mutex_;                  // 保护客户端列表
    std::vector<UdpClientEndpoint> clients_;    // 已注册的客户端列表
    std::mutex retransmit_cache_mutex_;         // 保护重传缓存
    std::deque<CachedUdpFrame> retransmit_cache_;  // NACK 重传缓存（按时间有序）
    std::shared_ptr<common::metrics::LatencyRecorder> send_latency_recorder_;  // 发送延迟统计器
    std::atomic<core::ModuleState> state_;      // 模块状态
    std::uint64_t sent_frames_;                 // 已发送帧数
    std::uint64_t dropped_fragmented_frames_;   // 因分片/发送失败丢弃的帧数
    std::uint64_t sent_fragments_;              // 已发送数据分片数
    std::uint64_t fec_fragments_sent_;          // 已发送 FEC 分片数
    std::uint64_t failed_fragments_;            // 发送失败的分片数
    std::uint64_t nack_requests_received_;      // 收到的 NACK 请求数
    std::uint64_t nack_fragments_requested_;    // NACK 请求涉及的分片总数
    std::uint64_t retransmitted_fragments_sent_;  // 重传成功的分片数
    std::uint64_t retransmit_fragment_misses_;    // 缓存中未找到的重传分片数
    std::uint64_t retransmit_fragments_throttled_;  // 因单次上限被限流的重传分片数
    std::vector<char> send_datagram_;           // 复用的发送缓冲区（避免反复分配）
};

}  // namespace udp
}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_UDP_UDPSTREAMINGBACKEND_H
