#include "modules/transport/tcp/TcpClientSession.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

#include "common/log/Logger.h"
#include "common/net/StreamProtocol.h"
#include "common/time/MonotonicClock.h"

namespace sserver {
namespace modules {
namespace transport {
namespace tcp {

// 构造函数：保存配置、初始化统计字段并缓存远端端点。
TcpClientSession::TcpClientSession(
        int socket_fd,
        const config::TransportConfig &config,
        const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder,
        int latency_log_interval_frames)
        : socket_fd_(socket_fd),
          config_(config),
          running_(false),
          send_latency_recorder_(send_latency_recorder),
          queue_wait_latency_recorder_(4096),
          send_time_latency_recorder_(4096),
          latency_log_interval_frames_(latency_log_interval_frames),
          sent_frames_(0),
          overflow_dropped_frames_(0),
          stale_dropped_frames_(0),
          dropped_incoming_non_keyframes_(0),
          backpressure_events_(0),
          max_queue_depth_(0),
          remote_endpoint_(BuildRemoteEndpoint()) {
    // 低延迟场景下禁用 Nagle 算法，避免小包被合并导致延迟增大。
    if (config_.enable_nodelay) {
        int flag = 1;
        setsockopt(socket_fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    }
}

// 析构函数：确保会话被停止、线程被回收。
TcpClientSession::~TcpClientSession() {
    Stop();
}

// 启动：开启接收线程（保活/断连检测）与发送线程（队列取帧发送）。
bool TcpClientSession::Start() {
    if (socket_fd_ < 0) {
        return false;
    }
    running_ = true;
    receive_thread_ = std::thread(&TcpClientSession::ReceiveLoop, this);
    send_thread_ = std::thread(&TcpClientSession::SendLoop, this);
    return true;
}

// 停止：关闭套接字唤醒阻塞线程，等待线程退出并清空队列。
void TcpClientSession::Stop() {
    // 先原子地将 running_ 置为 false，再关闭套接字（两个分支效果相同，
    // 仅保证只有一次关闭）。
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        CloseSocket();
    } else {
        CloseSocket();
    }

    outbound_frames_.NotifyAll();  // 唤醒可能在等待队列的发送线程

    // 回收线程；避免从线程自身调用 Stop 时自 join。
    if (receive_thread_.joinable() && receive_thread_.get_id() != std::this_thread::get_id()) {
        receive_thread_.join();
    }
    if (send_thread_.joinable() && send_thread_.get_id() != std::this_thread::get_id()) {
        send_thread_.join();
    }
    outbound_frames_.Clear();
}

// 返回会话是否仍在运行。
bool TcpClientSession::IsRunning() const {
    return running_.load();
}

// 将一帧放入发送队列；队列满时按配置策略丢弃旧帧或非关键帧。
void TcpClientSession::EnqueueFrame(common::model::EncodedFramePtr frame) {
    // 会话已停止或帧无效时直接丢弃。
    if (!running_.load() || !frame) {
        return;
    }

    QueuedFrame queued_frame;
    queued_frame.frame = frame;
    queued_frame.enqueue_timestamp_ns = common::time::MonotonicNowNs();

    // 记录背压事件：队列已满时仍收到新帧。
    const std::size_t queue_depth_before_push = outbound_frames_.Size();
    if (queue_depth_before_push >= config_.max_pending_frames) {
        ++backpressure_events_;
    }

    // 队列丢帧策略：
    // - drop_oldest_non_key: 优先丢弃非关键帧，保护关键帧以维持解码连续性
    // - drop_oldest: 直接丢弃队列最旧的帧
    std::size_t dropped = 0;
    if (config_.queue_drop_policy == "drop_oldest_non_key") {
        // 队列满且当前入队的是非关键帧，而队列中已经没有非关键帧可丢时，
        // 直接丢弃本帧（保留关键帧）。
        if (!frame->is_keyframe &&
            queue_depth_before_push >= config_.max_pending_frames &&
            !outbound_frames_.AnyMatching([](const QueuedFrame &candidate) {
                return candidate.frame != nullptr && !candidate.frame->is_keyframe;
            })) {
            ++dropped_incoming_non_keyframes_;
            return;
        }

        // 队列满时优先挤掉队列中最早的非关键帧。
        dropped = outbound_frames_.PushDropSelective(
                queued_frame,
                config_.max_pending_frames,
                [](const QueuedFrame &candidate) {
                    return candidate.frame != nullptr && !candidate.frame->is_keyframe;
                });
    } else {
        // 默认策略：队列满时丢弃最旧帧。
        dropped = outbound_frames_.PushDropOldestCountDropped(queued_frame, config_.max_pending_frames);
    }
    overflow_dropped_frames_ += static_cast<std::uint64_t>(dropped);

    // 记录历史最大队列深度，用于诊断积压情况。
    const std::size_t current_depth = outbound_frames_.Size();
    if (current_depth > max_queue_depth_) {
        max_queue_depth_ = current_depth;
    }
}

// 返回缓存的远端端点字符串。
std::string TcpClientSession::remote_endpoint() const {
    return remote_endpoint_;
}

// 接收线程：以 2 秒超时轮询可读事件，读取并校验 keepalive 消息；
// 连续多次超时视为客户端失活，主动断开。
void TcpClientSession::ReceiveLoop() {
    int keep_alive_budget = 5;  // 连续超时上限（5 * 2s = 10s 无消息则断开）
    while (running_.load()) {
        // 监听套接字可读事件。
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(socket_fd_, &read_fds);

        timeval timeout{};
        timeout.tv_sec = 2;
        timeout.tv_usec = 0;

        const int ready = select(socket_fd_ + 1, &read_fds, nullptr, nullptr, &timeout);
        if (!running_.load()) {
            break;
        }

        // 超时：扣减保活预算。
        if (ready == 0) {
            --keep_alive_budget;
            if (keep_alive_budget <= 0) {
                common::log::Logger::Warn("client keepalive timeout: " + remote_endpoint_);
                break;
            }
            continue;
        }

        // select 出错：信号中断继续，其他错误断开。
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            common::log::Logger::Warn("client select failed: " + remote_endpoint_);
            break;
        }

        // 读取完整消息头。
        common::net::MessageHeader header{};
        if (!ReceiveAll(reinterpret_cast<char *>(&header), sizeof(header))) {
            break;
        }

        // 校验魔数与消息类型，确认是 keepalive 后重置预算。
        if (common::net::HasValidMessageMagic(header) &&
            header.message_type == static_cast<std::uint16_t>(common::net::MessageType::kKeepAlive)) {
            keep_alive_budget = 5;
        }
    }

    // 线程退出时标记停止并关闭套接字，让发送线程也尽快退出。
    running_ = false;
    CloseSocket();
    outbound_frames_.NotifyAll();
}

// 发送线程：从队列取帧、丢弃过期帧、发送并统计延迟。
void TcpClientSession::SendLoop() {
    while (running_.load()) {
        QueuedFrame queued_frame;
        // 带超时等待，便于及时响应停止请求。
        if (!outbound_frames_.WaitPopFor(&queued_frame, std::chrono::milliseconds(2))) {
            continue;
        }

        const std::uint64_t send_start_timestamp_ns = common::time::MonotonicNowNs();
        // 计算排队等待时间；若超过阈值且不是关键帧，则丢弃该过期帧。
        if (queued_frame.enqueue_timestamp_ns != 0 && send_start_timestamp_ns >= queued_frame.enqueue_timestamp_ns) {
            const std::uint64_t queue_wait_ns = send_start_timestamp_ns - queued_frame.enqueue_timestamp_ns;
            queue_wait_latency_recorder_.RecordNs(queue_wait_ns);
            if (config_.max_queue_wait_ms > 0 &&
                queued_frame.frame != nullptr &&
                !queued_frame.frame->is_keyframe &&
                queue_wait_ns > static_cast<std::uint64_t>(config_.max_queue_wait_ms) * 1000ULL * 1000ULL) {
                ++stale_dropped_frames_;
                continue;
            }
        }

        // 发送失败说明连接已断开，退出循环。
        if (!SendFrame(queued_frame.frame, send_start_timestamp_ns)) {
            break;
        }

        const std::uint64_t send_end_timestamp_ns = common::time::MonotonicNowNs();
        // 统计单次发送耗时。
        if (send_end_timestamp_ns >= send_start_timestamp_ns) {
            send_time_latency_recorder_.RecordNs(send_end_timestamp_ns - send_start_timestamp_ns);
        }

        // 统计“采集到发送完成”的端到端发送延迟，并周期性打印会话统计。
        if (send_latency_recorder_ != nullptr && queued_frame.frame->capture_timestamp_ns != 0) {
            send_latency_recorder_->RecordNs(send_end_timestamp_ns - queued_frame.frame->capture_timestamp_ns);
            ++sent_frames_;
            if (latency_log_interval_frames_ > 0 &&
                sent_frames_ % static_cast<std::uint64_t>(latency_log_interval_frames_) == 0) {
                common::log::Logger::Info(
                        "transport latency " + remote_endpoint_ + " " + send_latency_recorder_->Format("capture_to_send"));
                common::log::Logger::Info(
                        "tcp queue stats " + remote_endpoint_ +
                        " current_depth=" + std::to_string(outbound_frames_.Size()) +
                        " max_depth=" + std::to_string(max_queue_depth_) +
                        " overflow_dropped_frames=" + std::to_string(overflow_dropped_frames_) +
                        " stale_dropped_frames=" + std::to_string(stale_dropped_frames_) +
                        " dropped_incoming_non_keyframes=" + std::to_string(dropped_incoming_non_keyframes_) +
                        " backpressure_events=" + std::to_string(backpressure_events_) +
                        " max_queue_wait_ms=" + std::to_string(config_.max_queue_wait_ms) +
                        " queue_drop_policy=" + config_.queue_drop_policy +
                        " " + queue_wait_latency_recorder_.Format("queue_wait") +
                        " " + send_time_latency_recorder_.Format("send_time"));
            }
        }
    }

    running_ = false;
    CloseSocket();
    outbound_frames_.NotifyAll();
}

// 阻塞式读取指定长度的数据（处理部分读与信号中断）。
bool TcpClientSession::ReceiveAll(char *buffer, std::size_t length) {
    std::lock_guard<std::mutex> lock(receive_mutex_);

    std::size_t received = 0;
    while (received < length && running_.load()) {
        const ssize_t result = recv(socket_fd_, buffer + received, length - received, 0);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            // 对端关闭连接。
            return false;
        }
        received += static_cast<std::size_t>(result);
    }
    return received == length;
}

// 按 StreamProtocol 协议封装并发送一帧：消息头 +（可选）帧诊断元数据 + 帧载荷。
bool TcpClientSession::SendFrame(common::model::EncodedFramePtr frame, std::uint64_t send_start_timestamp_ns) {
    // 组装消息头。
    common::net::MessageHeader header{};
    common::net::FillMessageMagic(header.head_id);
    header.message_type = static_cast<std::uint16_t>(common::net::MessageType::kAvStream);
    header.sub_type = static_cast<std::uint16_t>(frame->type);

    // 可选：在载荷前嵌入帧诊断元数据（序号、各阶段时间戳），供接收端分析延迟。
    common::net::FrameDiagnosticMetadata metadata{};
    if (config_.embed_frame_metadata) {
        metadata.sequence = frame->sequence;
        metadata.capture_timestamp_ns = frame->capture_timestamp_ns;
        metadata.encode_start_timestamp_ns = frame->encode_start_timestamp_ns;
        metadata.encode_end_timestamp_ns = frame->encode_end_timestamp_ns;
        metadata.transport_send_timestamp_ns = send_start_timestamp_ns;
        header.payload_length = static_cast<std::uint32_t>(sizeof(metadata) + frame->payload.size());
    } else {
        header.payload_length = static_cast<std::uint32_t>(frame->payload.size());
    }

    const char *metadata_ptr = config_.embed_frame_metadata
            ? reinterpret_cast<const char *>(&metadata)
            : nullptr;
    const std::size_t metadata_length = config_.embed_frame_metadata ? sizeof(metadata) : 0;
    const char *payload_ptr = frame->payload.empty()
            ? nullptr
            : reinterpret_cast<const char *>(frame->payload.data());
    return SendMessageParts(
            reinterpret_cast<const char *>(&header),
            sizeof(header),
            metadata_ptr,
            metadata_length,
            payload_ptr,
            frame->payload.size());
}

// 用 sendmsg 一次性发送多个不连续的内存段（头/元数据/载荷），
// 并处理部分发送：剩余数据前移后继续发送，直至全部完成。
bool TcpClientSession::SendMessageParts(
        const char *header,
        std::size_t header_length,
        const char *metadata,
        std::size_t metadata_length,
        const char *payload,
        std::size_t payload_length) {
    std::lock_guard<std::mutex> lock(send_mutex_);

    // 收集非空的内存段组成 iovec 数组。
    iovec iovecs[3];
    std::size_t lengths[3] = {header_length, metadata_length, payload_length};
    const char *buffers[3] = {header, metadata, payload};
    std::size_t active_count = 0;

    for (std::size_t index = 0; index < 3; ++index) {
        if (buffers[index] != nullptr && lengths[index] > 0) {
            iovecs[active_count].iov_base = const_cast<char *>(buffers[index]);
            iovecs[active_count].iov_len = lengths[index];
            ++active_count;
        }
    }

    // 循环发送直到全部写完。
    while (active_count > 0 && running_.load()) {
        msghdr message{};
        message.msg_iov = iovecs;
        message.msg_iovlen = active_count;

        // MSG_NOSIGNAL 防止对端关闭时触发 SIGPIPE 杀死进程。
        const ssize_t result = sendmsg(socket_fd_, &message, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }

        // 计算本次发送覆盖了几个完整的 iovec。
        std::size_t sent = static_cast<std::size_t>(result);
        std::size_t shift = 0;
        while (shift < active_count && sent >= iovecs[shift].iov_len) {
            sent -= iovecs[shift].iov_len;
            ++shift;
        }

        // 完整发送的段直接移除，剩余段前移。
        if (shift > 0) {
            for (std::size_t index = shift; index < active_count; ++index) {
                iovecs[index - shift] = iovecs[index];
            }
            active_count -= shift;
        }

        // 若最后一个段被部分发送，则推进其起始位置。
        if (active_count > 0 && sent > 0) {
            iovecs[0].iov_base = static_cast<char *>(iovecs[0].iov_base) + sent;
            iovecs[0].iov_len -= sent;
        }
    }

    return active_count == 0;
}

// 关闭套接字：先 shutdown 再 close，及时通知对端并释放描述符。
void TcpClientSession::CloseSocket() {
    if (socket_fd_ >= 0) {
        shutdown(socket_fd_, SHUT_RDWR);
        close(socket_fd_);
        socket_fd_ = -1;
    }
}

// 获取对端 IP 与端口，拼成 "ip:port" 字符串；失败时返回 "unknown-client"。
std::string TcpClientSession::BuildRemoteEndpoint() const {
    sockaddr_in address{};
    socklen_t address_length = sizeof(address);
    if (getpeername(socket_fd_, reinterpret_cast<sockaddr *>(&address), &address_length) != 0) {
        return "unknown-client";
    }

    char ip_buffer[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &address.sin_addr, ip_buffer, sizeof(ip_buffer));
    return std::string(ip_buffer) + ":" + std::to_string(ntohs(address.sin_port));
}

}  // namespace tcp
}  // namespace transport
}  // namespace modules
}  // namespace sserver
