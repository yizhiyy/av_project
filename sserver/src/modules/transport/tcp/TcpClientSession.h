// TcpClientSession.h
// TCP 客户端会话头文件。
// 每个已连接的 TCP 客户端对应一个会话对象，
// 内部使用“接收线程 + 发送线程 + 有界帧队列”向该客户端发送视频帧。

#ifndef SSERVER_MODULES_TRANSPORT_TCP_TCPCLIENTSESSION_H
#define SSERVER_MODULES_TRANSPORT_TCP_TCPCLIENTSESSION_H

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "common/concurrency/ThreadSafeQueue.h"
#include "common/metrics/LatencyRecorder.h"
#include "common/model/EncodedFrame.h"
#include "config/AppConfig.h"

namespace sserver {
namespace modules {
namespace transport {
namespace tcp {

// TcpClientSession：管理单个 TCP 客户端连接的发送/接收。
class TcpClientSession {
public:
    // socket_fd 为已 accept 的客户端套接字；
    // send_latency_recorder 用于记录发送延迟；latency_log_interval_frames 为统计日志间隔。
    TcpClientSession(
            int socket_fd,
            const config::TransportConfig &config,
            const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder,
            int latency_log_interval_frames);
    ~TcpClientSession();

    // 启动接收/发送线程。
    bool Start();

    // 停止会话：关闭套接字并回收线程。
    void Stop();

    // 会话是否仍在运行。
    bool IsRunning() const;

    // 将一帧数据放入发送队列（队列满时按策略丢帧）。
    void EnqueueFrame(common::model::EncodedFramePtr frame);

    // 远端端点描述（IP:端口）。
    std::string remote_endpoint() const;

private:
    // 队列中的待发送帧：保存帧本体与入队时间戳（用于统计排队延迟）。
    struct QueuedFrame {
        common::model::EncodedFramePtr frame;
        std::uint64_t enqueue_timestamp_ns = 0;
    };

    // 接收线程：读取并响应客户端的 keepalive 消息，检测连接断开。
    void ReceiveLoop();

    // 发送线程：从队列取帧并发送，统计排队/发送延迟。
    void SendLoop();

    // 阻塞读取指定长度的完整数据。
    bool ReceiveAll(char *buffer, std::size_t length);

    // 按 StreamProtocol 协议封装一帧（头 + 可选元数据 + 载荷）并发送。
    bool SendFrame(common::model::EncodedFramePtr frame, std::uint64_t send_start_timestamp_ns);

    // 通过 writev/sendmsg 一次性发送多个不连续的内存段（头/元数据/载荷）。
    bool SendMessageParts(const char *header, std::size_t header_length,
                          const char *metadata, std::size_t metadata_length,
                          const char *payload, std::size_t payload_length);

    // 关闭套接字（shutdown + close）。
    void CloseSocket();

    // 获取对端 IP 与端口。
    std::string BuildRemoteEndpoint() const;

private:
    int socket_fd_;                                             // 客户端套接字描述符
    config::TransportConfig config_;                            // 传输配置（队列深度、丢帧策略等）
    std::atomic_bool running_;                                  // 会话运行标志
    std::thread receive_thread_;                                // 接收线程
    std::thread send_thread_;                                   // 发送线程
    std::mutex receive_mutex_;                                  // 保护接收操作
    std::mutex send_mutex_;                                     // 保护发送操作
    common::concurrency::ThreadSafeQueue<QueuedFrame> outbound_frames_;  // 待发送帧的有界队列
    std::shared_ptr<common::metrics::LatencyRecorder> send_latency_recorder_;  // 全局发送延迟统计器
    common::metrics::LatencyRecorder queue_wait_latency_recorder_;   // 排队等待延迟统计
    common::metrics::LatencyRecorder send_time_latency_recorder_;    // 发送耗时统计
    int latency_log_interval_frames_;                           // 统计日志帧间隔
    std::uint64_t sent_frames_;                                 // 已发送帧数
    std::uint64_t overflow_dropped_frames_;                     // 队列溢出丢弃的帧数
    std::uint64_t stale_dropped_frames_;                        // 因等待超时丢弃的帧数
    std::uint64_t dropped_incoming_non_keyframes_;              // 入队时被直接丢弃的非关键帧数
    std::uint64_t backpressure_events_;                         // 队列满时的背压事件次数
    std::size_t max_queue_depth_;                               // 观察到的最大队列深度
    std::string remote_endpoint_;                               // 缓存的对端端点字符串
};

}  // namespace tcp
}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_TCP_TCPCLIENTSESSION_H
