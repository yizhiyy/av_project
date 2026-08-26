#include "modules/transport/udp/UdpStreamingBackend.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "common/log/Logger.h"
#include "common/net/StreamProtocol.h"
#include "common/time/MonotonicClock.h"

namespace sserver {
namespace modules {
namespace transport {
namespace udp {

namespace {

// 将 source 数据异或累加到 target 中，用于 FEC 前向纠错码的计算
void XorInto(std::vector<std::uint8_t> *target, const std::uint8_t *source, std::size_t length) {
    if (target == nullptr || source == nullptr || length == 0) {
        return;
    }

    // 若目标长度不足，先扩展（新字节填 0，不影响异或结果）。
    if (target->size() < length) {
        target->resize(length, 0);
    }
    // 逐字节异或：FEC 奇偶分片 = 所有数据分片按位异或。
    for (std::size_t index = 0; index < length; ++index) {
        (*target)[index] ^= source[index];
    }
}

// 判断数据报是否为保活消息（校验消息魔数与类型）。
bool IsKeepAlivePacket(const char *buffer, std::size_t length) {
    if (length < sizeof(common::net::MessageHeader)) {
        return false;
    }

    const common::net::MessageHeader *header = reinterpret_cast<const common::net::MessageHeader *>(buffer);
    return common::net::HasValidMessageMagic(*header) &&
           header->message_type == static_cast<std::uint16_t>(common::net::MessageType::kKeepAlive);
}

// 判断数据报是否为 NACK 重传请求。
bool IsNackPacket(const char *buffer, std::size_t length) {
    if (length < sizeof(common::net::MessageHeader)) {
        return false;
    }

    const common::net::MessageHeader *header = reinterpret_cast<const common::net::MessageHeader *>(buffer);
    return common::net::HasValidMessageMagic(*header) &&
           header->message_type == static_cast<std::uint16_t>(common::net::MessageType::kUdpNack);
}

// 从保活数据报中解析客户端接收报告（UdpReceiverReport）。
bool ParseKeepAliveReport(
        const char *buffer,
        std::size_t length,
        common::net::UdpReceiverReport *report) {
    if (report == nullptr || length < sizeof(common::net::MessageHeader)) {
        return false;
    }

    const common::net::MessageHeader *header = reinterpret_cast<const common::net::MessageHeader *>(buffer);
    // 校验魔数、类型以及载荷长度是否足够容纳接收报告。
    if (!common::net::HasValidMessageMagic(*header) ||
        header->message_type != static_cast<std::uint16_t>(common::net::MessageType::kKeepAlive) ||
        header->payload_length < sizeof(common::net::UdpReceiverReport) ||
        length < sizeof(common::net::MessageHeader) + sizeof(common::net::UdpReceiverReport)) {
        return false;
    }

    // 报告紧跟在消息头之后，直接按结构体拷贝。
    std::memcpy(
            report,
            buffer + sizeof(common::net::MessageHeader),
            sizeof(common::net::UdpReceiverReport));
    return true;
}

// 解析 NACK 请求：消息头后依次为 NackHeader 与 NackItem 数组。
bool ParseNackRequest(
        const char *buffer,
        std::size_t length,
        common::net::UdpNackHeader *nack_header,
        std::vector<common::net::UdpNackItem> *nack_items) {
    if (nack_header == nullptr || nack_items == nullptr || length < sizeof(common::net::MessageHeader)) {
        return false;
    }

    const common::net::MessageHeader *header = reinterpret_cast<const common::net::MessageHeader *>(buffer);
    // 校验魔数、类型以及是否至少包含 NackHeader。
    if (!common::net::HasValidMessageMagic(*header) ||
        header->message_type != static_cast<std::uint16_t>(common::net::MessageType::kUdpNack) ||
        header->payload_length < sizeof(common::net::UdpNackHeader) ||
        length < sizeof(common::net::MessageHeader) + sizeof(common::net::UdpNackHeader)) {
        return false;
    }

    std::memcpy(
            nack_header,
            buffer + sizeof(common::net::MessageHeader),
            sizeof(common::net::UdpNackHeader));
    // 校验载荷长度与 request_count 一致（防止越界解析）。
    const std::size_t expected_payload_length =
            sizeof(common::net::UdpNackHeader) +
            static_cast<std::size_t>(nack_header->request_count) * sizeof(common::net::UdpNackItem);
    if (header->payload_length != expected_payload_length ||
        length < sizeof(common::net::MessageHeader) + expected_payload_length) {
        return false;
    }

    // 拷贝请求项数组。
    nack_items->resize(nack_header->request_count);
    if (!nack_items->empty()) {
        std::memcpy(
                nack_items->data(),
                buffer + sizeof(common::net::MessageHeader) + sizeof(common::net::UdpNackHeader),
                nack_items->size() * sizeof(common::net::UdpNackItem));
    }
    return true;
}

}  // namespace

// 构造函数：初始化成员与统计计数器，后端名称固定为 "udp"。
UdpStreamingBackend::UdpStreamingBackend(
        const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder)
        : backend_name_("udp"),
          latency_log_interval_frames_(120),
          socket_fd_(-1),
          bound_port_(0),
          running_(false),
          send_latency_recorder_(send_latency_recorder),
          state_(core::ModuleState::kCreated),
          sent_frames_(0),
          dropped_fragmented_frames_(0),
          sent_fragments_(0),
          fec_fragments_sent_(0),
          failed_fragments_(0),
          nack_requests_received_(0),
          nack_fragments_requested_(0),
          retransmitted_fragments_sent_(0),
          retransmit_fragment_misses_(0),
          retransmit_fragments_throttled_(0) {
}

// 析构函数：确保资源被释放。
UdpStreamingBackend::~UdpStreamingBackend() {
    shutdown();
}

// 初始化：保存传输配置与统计参数。
bool UdpStreamingBackend::initialize(const core::ApplicationContext &context) {
    config_ = context.config.transport;
    latency_log_interval_frames_ = context.config.runtime.latency_log_interval_frames;
    state_ = core::ModuleState::kInitialized;
    return true;
}

// 启动：打开 UDP 套接字并启动接收线程。
bool UdpStreamingBackend::start() {
    // 传输未启用时直接进入 Running，让上层流程照常继续。
    if (!config_.enabled) {
        state_ = core::ModuleState::kRunning;
        return true;
    }

    if (!OpenSocket()) {
        state_ = core::ModuleState::kFailed;
        return false;
    }

    running_ = true;
    receive_thread_ = std::thread(&UdpStreamingBackend::ReceiveLoop, this);
    state_ = core::ModuleState::kRunning;
    return true;
}

// 停止：关闭套接字、回收接收线程并清空客户端与重传缓存。
void UdpStreamingBackend::stop() {
    if (state_.load() != core::ModuleState::kRunning) {
        return;
    }

    running_ = false;
    CloseSocket();  // 关闭后 select 立即返回，线程可退出

    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }

    // 清空客户端列表与重传缓存。
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(retransmit_cache_mutex_);
        retransmit_cache_.clear();
    }

    state_ = core::ModuleState::kStopped;
}

// 关闭：停止后置为 Shutdown。
void UdpStreamingBackend::shutdown() {
    stop();
    state_ = core::ModuleState::kShutdown;
}

// 返回模块状态（线程安全）。
core::ModuleState UdpStreamingBackend::state() const {
    return state_.load();
}

// 广播一帧：先取客户端快照，再分片发送，最后记录延迟与统计。
void UdpStreamingBackend::Broadcast(common::model::EncodedFramePtr frame) {
    if (!config_.enabled || !frame || socket_fd_ < 0) {
        return;
    }

    // 获取当前有效客户端（同时清理超时客户端）。
    const std::uint64_t now_ns = common::time::MonotonicNowNs();
    const std::vector<sockaddr_in> clients = SnapshotClients(now_ns);
    if (clients.empty()) {
        return;
    }

    // 分片发送；失败时计数并限频告警。
    if (!SendFrameFragments(frame, clients)) {
        ++dropped_fragmented_frames_;
        if (dropped_fragmented_frames_ == 1 || dropped_fragmented_frames_ % 30 == 0) {
            common::log::Logger::Warn("udp frame dropped because fragmentation or send failed");
        }
        return;
    }

    // 记录“采集到发送”延迟，并周期性打印发送统计与各客户端接收报告。
        if (send_latency_recorder_ != nullptr && frame->capture_timestamp_ns != 0) {
            send_latency_recorder_->RecordNs(common::time::MonotonicNowNs() - frame->capture_timestamp_ns);
            ++sent_frames_;
            if (latency_log_interval_frames_ > 0 &&
                sent_frames_ % static_cast<std::uint64_t>(latency_log_interval_frames_) == 0) {
            common::log::Logger::Info("transport latency udp-broadcast " +
                                      send_latency_recorder_->Format("capture_to_send"));
            common::log::Logger::Info(
                    "udp transport stats"
                    " sent_frames=" + std::to_string(sent_frames_) +
                    " sent_fragments=" + std::to_string(sent_fragments_) +
                    " fec_fragments_sent=" + std::to_string(fec_fragments_sent_) +
                    " failed_fragments=" + std::to_string(failed_fragments_) +
                    " dropped_frames=" + std::to_string(dropped_fragmented_frames_) +
                    " nack_requests_received=" + std::to_string(nack_requests_received_) +
                    " nack_fragments_requested=" + std::to_string(nack_fragments_requested_) +
                    " retransmitted_fragments_sent=" + std::to_string(retransmitted_fragments_sent_) +
                    " retransmit_fragment_misses=" + std::to_string(retransmit_fragment_misses_) +
                    " retransmit_fragments_throttled=" + std::to_string(retransmit_fragments_throttled_));
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (std::size_t index = 0; index < clients_.size(); ++index) {
                if (clients_[index].has_report) {
                    common::log::Logger::Info(FormatClientReport(clients_[index]));
                }
            }
        }
    }
}

// 返回实际绑定端口。
int UdpStreamingBackend::bound_port() const {
    return bound_port_;
}

// 返回后端类型：UDP。
TransportBackend UdpStreamingBackend::backend() const {
    return TransportBackend::kUdp;
}

// 返回后端名称。
const std::string &UdpStreamingBackend::backend_name() const {
    return backend_name_;
}

// 打开 UDP 套接字：创建、设置缓冲、非阻塞、绑定本地地址并记录实际端口。
bool UdpStreamingBackend::OpenSocket() {
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        common::log::Logger::Error("failed to create udp transport socket");
        return false;
    }

    // 允许地址复用；增大收/发缓冲以减少短时突发丢包。
    int reuse = 1;
    setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &config_.udp_receive_buffer_bytes, sizeof(config_.udp_receive_buffer_bytes));
    setsockopt(socket_fd_, SOL_SOCKET, SO_SNDBUF, &config_.udp_send_buffer_bytes, sizeof(config_.udp_send_buffer_bytes));

    // 非阻塞模式，配合 select 实现可中断的接收循环。
    const int current_flags = fcntl(socket_fd_, F_GETFL, 0);
    if (current_flags >= 0) {
        fcntl(socket_fd_, F_SETFL, current_flags | O_NONBLOCK);
    }

    // 绑定本地地址与端口。
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(config_.listen_port));
    address.sin_addr.s_addr = inet_addr(config_.bind_address.c_str());

    if (bind(socket_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        common::log::Logger::Error("failed to bind udp transport socket");
        CloseSocket();
        return false;
    }

    // 记录实际绑定端口（端口为 0 时由内核分配）。
    if (config_.listen_port == 0) {
        sockaddr_in bound_address{};
        socklen_t bound_length = sizeof(bound_address);
        if (getsockname(socket_fd_, reinterpret_cast<sockaddr *>(&bound_address), &bound_length) == 0) {
            bound_port_ = ntohs(bound_address.sin_port);
            common::log::Logger::Info("udp transport bound to ephemeral port " + std::to_string(bound_port_));
        }
    } else {
        bound_port_ = config_.listen_port;
    }

    return true;
}

// 关闭套接字。
void UdpStreamingBackend::CloseSocket() {
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
}

// 接收线程：轮询 UDP 套接字，处理保活/接收报告与 NACK 请求。
void UdpStreamingBackend::ReceiveLoop() {
    std::vector<char> buffer(config_.udp_max_datagram_size);

    while (running_.load()) {
        // 监听可读事件（200ms 超时，便于及时退出线程）。
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(socket_fd_, &read_fds);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200 * 1000;

        const int ready = select(socket_fd_ + 1, &read_fds, nullptr, nullptr, &timeout);
        if (!running_.load()) {
            break;
        }
        // 超时：顺带清理超时未上报的客户端。
        if (ready == 0) {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            PruneStaleClientsLocked(common::time::MonotonicNowNs());
            continue;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            common::log::Logger::Warn("udp transport select failed");
            break;
        }

        // 读取一个数据报及其来源地址。
        sockaddr_in client_address{};
        socklen_t client_length = sizeof(client_address);
        const ssize_t received = recvfrom(
                socket_fd_,
                buffer.data(),
                buffer.size(),
                0,
                reinterpret_cast<sockaddr *>(&client_address),
                &client_length);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            common::log::Logger::Warn("udp transport recvfrom failed");
            break;
        }

        const std::uint64_t now_ns = common::time::MonotonicNowNs();
        // 保活消息：注册/刷新客户端，并尽量解析接收报告。
        if (IsKeepAlivePacket(buffer.data(), static_cast<std::size_t>(received))) {
            common::net::UdpReceiverReport report{};
            const bool has_report = ParseKeepAliveReport(buffer.data(), static_cast<std::size_t>(received), &report);
            RegisterClient(client_address, now_ns, has_report ? &report : nullptr);
        } else if (IsNackPacket(buffer.data(), static_cast<std::size_t>(received))) {
            // NACK 请求：注册客户端并处理分片重传。
            common::net::UdpNackHeader nack_header{};
            std::vector<common::net::UdpNackItem> nack_items;
            if (ParseNackRequest(buffer.data(), static_cast<std::size_t>(received), &nack_header, &nack_items)) {
                RegisterClient(client_address, now_ns, nullptr);
                HandleNackRequest(client_address, now_ns, nack_header, nack_items);
            }
        }
    }

    running_ = false;
    CloseSocket();
}

// 注册或刷新客户端；可选附带最新的接收报告。
void UdpStreamingBackend::RegisterClient(
        const sockaddr_in &address,
        std::uint64_t now_ns,
        const common::net::UdpReceiverReport *report) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    PruneStaleClientsLocked(now_ns);

    // 已存在的客户端：刷新最后活跃时间；有报告则更新报告。
    for (std::size_t index = 0; index < clients_.size(); ++index) {
        if (SameEndpoint(clients_[index].address, address)) {
            clients_[index].last_seen_ns = now_ns;
            if (report != nullptr) {
                clients_[index].latest_report = *report;
                clients_[index].latest_report_timestamp_ns = now_ns;
                clients_[index].has_report = true;
                common::log::Logger::Info(FormatClientReport(clients_[index]));
            }
            return;
        }
    }

    // 新客户端：加入列表并记录日志。
    UdpClientEndpoint endpoint;
    endpoint.address = address;
    endpoint.last_seen_ns = now_ns;
    endpoint.latest_report_timestamp_ns = 0;
    endpoint.has_report = false;
    if (report != nullptr) {
        endpoint.latest_report = *report;
        endpoint.latest_report_timestamp_ns = now_ns;
        endpoint.has_report = true;
    }
    clients_.push_back(endpoint);
    common::log::Logger::Info("udp client registered: " + FormatEndpoint(address));
    if (endpoint.has_report) {
        common::log::Logger::Info(FormatClientReport(endpoint));
    }
}

// 清理超过配置超时时间未上报的客户端（调用方须持有 clients_mutex_）。
void UdpStreamingBackend::PruneStaleClientsLocked(std::uint64_t now_ns) {
    const std::uint64_t timeout_ns = static_cast<std::uint64_t>(config_.udp_client_timeout_ms) * 1000ULL * 1000ULL;
    clients_.erase(
            std::remove_if(
                    clients_.begin(),
                    clients_.end(),
                    [now_ns, timeout_ns](const UdpClientEndpoint &client) {
                        return now_ns > client.last_seen_ns && now_ns - client.last_seen_ns > timeout_ns;
                    }),
            clients_.end());
}

// 获取当前有效客户端的地址快照（同时清理超时客户端）。
std::vector<sockaddr_in> UdpStreamingBackend::SnapshotClients(std::uint64_t now_ns) {
    std::vector<sockaddr_in> snapshot;
    std::lock_guard<std::mutex> lock(clients_mutex_);
    PruneStaleClientsLocked(now_ns);
    snapshot.reserve(clients_.size());
    for (std::size_t index = 0; index < clients_.size(); ++index) {
        snapshot.push_back(clients_[index].address);
    }
    return snapshot;
}

// 将一帧拆分为多个 UDP 分片发送，支持：
// - 批量发送（sendmmsg）优化多客户端场景
// - FEC 前向纠错（XOR 奇偶校验分片）
// - NACK 重传缓存（缓存已发分片供后续重传）
bool UdpStreamingBackend::SendFrameFragments(
        common::model::EncodedFramePtr frame,
        const std::vector<sockaddr_in> &clients) {
    if (!frame) {
        return false;
    }

    // 数据报由 消息头 + 分片头 + 分片载荷 组成，载荷上限必须大于头部。
    const std::size_t header_size = sizeof(common::net::MessageHeader) + sizeof(common::net::UdpFrameFragmentHeader);
    if (config_.udp_target_payload_size <= header_size || config_.udp_max_datagram_size <= header_size) {
        return false;
    }

    // 计算单分片最大载荷：取配置目标值，且不超过数据报上限减头部。
    const std::size_t max_fragment_payload_size = std::min(
            config_.udp_target_payload_size,
            config_.udp_max_datagram_size - header_size);
    // 分片数量 = 向上取整（空载荷也至少 1 片）。
    const std::size_t fragment_count_size = frame->payload.empty()
            ? 1
            : (frame->payload.size() + max_fragment_payload_size - 1) / max_fragment_payload_size;
    // 协议中分片序号是 16 位，数量不能超过上限。
    if (fragment_count_size > 0xFFFFu) {
        common::log::Logger::Warn("udp frame dropped because fragment count exceeds protocol limit");
        return false;
    }

    const std::uint16_t fragment_count = static_cast<std::uint16_t>(fragment_count_size);
    // 组装公共消息头。
    common::net::MessageHeader header{};
    common::net::FillMessageMagic(header.head_id);
    header.message_type = static_cast<std::uint16_t>(common::net::MessageType::kAvStream);
    header.sub_type = static_cast<std::uint16_t>(frame->type);

    // 记录发送时刻，并准备 NACK 缓存结构与 FEC 异或累加缓冲区。
    const std::uint64_t transport_send_timestamp_ns = common::time::MonotonicNowNs();
    CachedUdpFrame cached_frame;
    cached_frame.frame_sequence = frame->sequence;
    cached_frame.cached_timestamp_ns = transport_send_timestamp_ns;
    std::vector<std::uint8_t> fec_payload;

    // 复用发送缓冲区，避免每个分片重新分配内存。
    if (send_datagram_.size() < config_.udp_max_datagram_size) {
        send_datagram_.resize(config_.udp_max_datagram_size);
    }

    // 多客户端时用 sendmmsg 批量发送，减少系统调用次数。
    const bool batch_send = clients.size() > 1;
    std::vector<struct mmsghdr> batch_messages;
    std::vector<struct iovec> batch_iovecs;
    if (batch_send) {
        batch_messages.resize(clients.size());
        batch_iovecs.resize(clients.size());
    }

    // 逐分片构造并发送。
    for (std::uint16_t fragment_index = 0; fragment_index < fragment_count; ++fragment_index) {
        // 计算本分片在帧载荷中的偏移与长度。
        const std::size_t fragment_offset = static_cast<std::size_t>(fragment_index) * max_fragment_payload_size;
        const std::size_t fragment_payload_size = frame->payload.empty()
                ? 0
                : std::min(max_fragment_payload_size, frame->payload.size() - fragment_offset);

        // 组装分片头：帧序号、时间戳（可选）、分片位置与角色。
        common::net::UdpFrameFragmentHeader fragment_header{};
        fragment_header.frame_sequence = frame->sequence;
        fragment_header.capture_timestamp_ns = config_.embed_frame_metadata ? frame->capture_timestamp_ns : 0;
        fragment_header.encode_start_timestamp_ns = config_.embed_frame_metadata ? frame->encode_start_timestamp_ns : 0;
        fragment_header.encode_end_timestamp_ns = config_.embed_frame_metadata ? frame->encode_end_timestamp_ns : 0;
        fragment_header.transport_send_timestamp_ns = config_.embed_frame_metadata ? transport_send_timestamp_ns : 0;
        fragment_header.frame_payload_size = static_cast<std::uint32_t>(frame->payload.size());
        fragment_header.fragment_offset = static_cast<std::uint32_t>(fragment_offset);
        fragment_header.fragment_index = fragment_index;
        fragment_header.fragment_count = fragment_count;
        fragment_header.fragment_role = static_cast<std::uint16_t>(common::net::UdpFragmentRole::kData);

        // 填充消息头中的载荷长度，计算整个数据报大小。
        header.payload_length = static_cast<std::uint32_t>(sizeof(fragment_header) + fragment_payload_size);
        const std::size_t datagram_size = sizeof(header) + sizeof(fragment_header) + fragment_payload_size;

        // 把消息头、分片头与载荷依次拷贝进发送缓冲区。
        std::memcpy(send_datagram_.data(), &header, sizeof(header));
        std::memcpy(send_datagram_.data() + sizeof(header), &fragment_header, sizeof(fragment_header));
        if (fragment_payload_size > 0) {
            std::memcpy(
                    send_datagram_.data() + sizeof(header) + sizeof(fragment_header),
                    frame->payload.data() + fragment_offset,
                    fragment_payload_size);
        }

        // 启用 NACK 时缓存完整数据报供后续重传。
        if (config_.udp_enable_nack) {
            CachedUdpFragment cached_fragment;
            cached_fragment.fragment_index = fragment_index;
            cached_fragment.datagram_size = datagram_size;
            cached_fragment.datagram.assign(send_datagram_.begin(), send_datagram_.begin() + static_cast<std::ptrdiff_t>(datagram_size));
            cached_frame.fragments.push_back(cached_fragment);
        }
        // 启用 FEC 时把本分片载荷异或累加进校验缓冲区。
        if (config_.udp_enable_fec && fragment_payload_size > 0) {
            XorInto(&fec_payload, frame->payload.data() + fragment_offset, fragment_payload_size);
        }

        // 发送：多客户端批量发送，单客户端直接 sendto。
        bool sent_fragment = false;
        if (batch_send) {
            for (std::size_t ci = 0; ci < clients.size(); ++ci) {
                batch_iovecs[ci].iov_base = send_datagram_.data();
                batch_iovecs[ci].iov_len = datagram_size;
                std::memset(&batch_messages[ci], 0, sizeof(batch_messages[ci]));
                batch_messages[ci].msg_hdr.msg_iov = &batch_iovecs[ci];
                batch_messages[ci].msg_hdr.msg_iovlen = 1;
                batch_messages[ci].msg_hdr.msg_name = const_cast<sockaddr_in *>(&clients[ci]);
                batch_messages[ci].msg_hdr.msg_namelen = sizeof(clients[ci]);
            }
            const int sent_count = sendmmsg(socket_fd_, batch_messages.data(),
                                             static_cast<unsigned int>(batch_messages.size()), MSG_NOSIGNAL);
            if (sent_count > 0) {
                sent_fragment = true;
                sent_fragments_ += static_cast<std::uint64_t>(sent_count);
                failed_fragments_ += static_cast<std::uint64_t>(clients.size()) - static_cast<std::uint64_t>(sent_count);
            }
        } else if (!clients.empty()) {
            const ssize_t sent = sendto(
                    socket_fd_,
                    send_datagram_.data(),
                    datagram_size,
                    MSG_NOSIGNAL,
                    reinterpret_cast<const sockaddr *>(&clients[0]),
                    sizeof(clients[0]));
            if (sent == static_cast<ssize_t>(datagram_size)) {
                sent_fragment = true;
                ++sent_fragments_;
            } else {
                ++failed_fragments_;
            }
        }
        // 任一数据分片发送失败即放弃整帧。
        if (!sent_fragment) {
            return false;
        }
    }

    // 发送 FEC 奇偶校验分片：将所有数据分片异或合并后作为一个额外分片发送
    // 接收端可通过此分片恢复任意一个丢失的数据分片
    if (config_.udp_enable_fec && fragment_count > 1 && !fec_payload.empty()) {
        // 奇偶分片复用分片头结构，但 fragment_role 标记为 kXorParity。
        common::net::UdpFrameFragmentHeader parity_header{};
        parity_header.frame_sequence = frame->sequence;
        parity_header.capture_timestamp_ns = config_.embed_frame_metadata ? frame->capture_timestamp_ns : 0;
        parity_header.encode_start_timestamp_ns = config_.embed_frame_metadata ? frame->encode_start_timestamp_ns : 0;
        parity_header.encode_end_timestamp_ns = config_.embed_frame_metadata ? frame->encode_end_timestamp_ns : 0;
        parity_header.transport_send_timestamp_ns = config_.embed_frame_metadata ? transport_send_timestamp_ns : 0;
        parity_header.frame_payload_size = static_cast<std::uint32_t>(frame->payload.size());
        parity_header.fragment_offset = 0;
        parity_header.fragment_index = 0;
        parity_header.fragment_count = fragment_count;
        parity_header.fragment_role = static_cast<std::uint16_t>(common::net::UdpFragmentRole::kXorParity);

        // 组装并发送奇偶数据报。
        header.payload_length = static_cast<std::uint32_t>(sizeof(parity_header) + fec_payload.size());
        const std::size_t parity_datagram_size = sizeof(header) + sizeof(parity_header) + fec_payload.size();
        std::memcpy(send_datagram_.data(), &header, sizeof(header));
        std::memcpy(send_datagram_.data() + sizeof(header), &parity_header, sizeof(parity_header));
        std::memcpy(
                send_datagram_.data() + sizeof(header) + sizeof(parity_header),
                fec_payload.data(),
                fec_payload.size());

        bool sent_parity = false;
        if (batch_send) {
            for (std::size_t ci = 0; ci < clients.size(); ++ci) {
                batch_iovecs[ci].iov_base = send_datagram_.data();
                batch_iovecs[ci].iov_len = parity_datagram_size;
                std::memset(&batch_messages[ci], 0, sizeof(batch_messages[ci]));
                batch_messages[ci].msg_hdr.msg_iov = &batch_iovecs[ci];
                batch_messages[ci].msg_hdr.msg_iovlen = 1;
                batch_messages[ci].msg_hdr.msg_name = const_cast<sockaddr_in *>(&clients[ci]);
                batch_messages[ci].msg_hdr.msg_namelen = sizeof(clients[ci]);
            }
            const int sent_count = sendmmsg(socket_fd_, batch_messages.data(),
                                             static_cast<unsigned int>(batch_messages.size()), MSG_NOSIGNAL);
            if (sent_count > 0) {
                sent_parity = true;
                sent_fragments_ += static_cast<std::uint64_t>(sent_count);
                fec_fragments_sent_ += static_cast<std::uint64_t>(sent_count);
                failed_fragments_ += static_cast<std::uint64_t>(clients.size()) - static_cast<std::uint64_t>(sent_count);
            }
        } else if (!clients.empty()) {
            const ssize_t sent = sendto(
                    socket_fd_,
                    send_datagram_.data(),
                    parity_datagram_size,
                    MSG_NOSIGNAL,
                    reinterpret_cast<const sockaddr *>(&clients[0]),
                    sizeof(clients[0]));
            if (sent == static_cast<ssize_t>(parity_datagram_size)) {
                sent_parity = true;
                ++sent_fragments_;
                ++fec_fragments_sent_;
            } else {
                ++failed_fragments_;
            }
        }
        if (!sent_parity) {
            return false;
        }
    }

    // 发送成功后把整帧分片放入重传缓存（仅启用 NACK 时）。
    if (config_.udp_enable_nack && !cached_frame.fragments.empty()) {
        CacheFrameFragments(cached_frame);
    }

    return true;
}

// 将一帧的分片放入重传缓存；超出容量时丢弃最旧的帧。
void UdpStreamingBackend::CacheFrameFragments(const CachedUdpFrame &frame) {
    std::lock_guard<std::mutex> lock(retransmit_cache_mutex_);
    PruneRetransmitCacheLocked(common::time::MonotonicNowNs());
    retransmit_cache_.push_back(frame);
    while (retransmit_cache_.size() > config_.udp_retransmit_cache_frames) {
        retransmit_cache_.pop_front();
    }
}

// 从缓存头部清理超过最大保留时间的帧（调用方须持有 retransmit_cache_mutex_）。
void UdpStreamingBackend::PruneRetransmitCacheLocked(std::uint64_t now_ns) {
    const std::uint64_t max_age_ns =
            static_cast<std::uint64_t>(config_.udp_retransmit_cache_max_age_ms) * 1000ULL * 1000ULL;
    while (!retransmit_cache_.empty()) {
        const CachedUdpFrame &frame = retransmit_cache_.front();
        if (now_ns >= frame.cached_timestamp_ns && now_ns - frame.cached_timestamp_ns > max_age_ns) {
            retransmit_cache_.pop_front();
            continue;
        }
        break;
    }
}

// 处理客户端 NACK 重传请求：在重传缓存中查找请求的分片并重发。
void UdpStreamingBackend::HandleNackRequest(
        const sockaddr_in &address,
        std::uint64_t now_ns,
        const common::net::UdpNackHeader & /* nack_header */,
        const std::vector<common::net::UdpNackItem> &nack_items) {
    // 未启用 NACK、无请求项或套接字无效时直接忽略。
    if (!config_.udp_enable_nack || nack_items.empty() || socket_fd_ < 0) {
        return;
    }

    // 先清理过期缓存。
    {
        std::lock_guard<std::mutex> lock(retransmit_cache_mutex_);
        PruneRetransmitCacheLocked(now_ns);
    }

    // 更新统计。
    ++nack_requests_received_;
    nack_fragments_requested_ += nack_items.size();

    std::lock_guard<std::mutex> lock(retransmit_cache_mutex_);
    // 单次请求最多重传的分片数受配置限制，超出部分限流。
    const std::size_t allowed_fragments =
            std::min<std::size_t>(nack_items.size(), config_.udp_retransmit_max_fragments_per_request);
    if (nack_items.size() > allowed_fragments) {
        retransmit_fragments_throttled_ += nack_items.size() - allowed_fragments;
    }

    // 逐个请求项在缓存中查找对应帧/分片并重发。
    for (std::size_t item_index = 0; item_index < allowed_fragments; ++item_index) {
        const common::net::UdpNackItem &item = nack_items[item_index];
        bool found_fragment = false;
        for (std::size_t frame_index = 0; frame_index < retransmit_cache_.size() && !found_fragment; ++frame_index) {
            const CachedUdpFrame &cached_frame = retransmit_cache_[frame_index];
            if (cached_frame.frame_sequence != item.frame_sequence) {
                continue;
            }
            for (std::size_t fragment_index = 0; fragment_index < cached_frame.fragments.size(); ++fragment_index) {
                const CachedUdpFragment &cached_fragment = cached_frame.fragments[fragment_index];
                if (cached_fragment.fragment_index != item.fragment_index) {
                    continue;
                }
                // 找到分片：仅向请求方发送。
                const ssize_t sent = sendto(
                        socket_fd_,
                        cached_fragment.datagram.data(),
                        cached_fragment.datagram_size,
                        MSG_NOSIGNAL,
                        reinterpret_cast<const sockaddr *>(&address),
                        sizeof(address));
                if (sent == static_cast<ssize_t>(cached_fragment.datagram_size)) {
                    ++retransmitted_fragments_sent_;
                }
                found_fragment = true;
                break;
            }
        }
        // 缓存中找不到（已过期/被挤出）时计数。
        if (!found_fragment) {
            ++retransmit_fragment_misses_;
        }
    }
}

// 格式化端点地址为 "ip:port"。
std::string UdpStreamingBackend::FormatEndpoint(const sockaddr_in &address) const {
    char ip_buffer[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &address.sin_addr, ip_buffer, sizeof(ip_buffer));
    return std::string(ip_buffer) + ":" + std::to_string(ntohs(address.sin_port));
}

// 格式化客户端接收报告，包含帧完成数、乱序、丢片率与抖动等指标。
std::string UdpStreamingBackend::FormatClientReport(const UdpClientEndpoint &client) const {
    const common::net::UdpReceiverReport &report = client.latest_report;
    const std::uint64_t total_fragment_attempts = report.fragments_received + report.timed_out_fragments;
    const double fragment_loss_percent = total_fragment_attempts == 0
            ? 0.0
            : static_cast<double>(report.timed_out_fragments) * 100.0 / static_cast<double>(total_fragment_attempts);

    return "udp client report"
            " endpoint=" + FormatEndpoint(client.address) +
            " completed_frames=" + std::to_string(report.completed_frames) +
            " reordered_frames=" + std::to_string(report.reordered_frames) +
            " fragments=" + std::to_string(report.fragments_received) +
            " duplicate_fragments=" + std::to_string(report.duplicate_fragments) +
            " timed_out_fragments=" + std::to_string(report.timed_out_fragments) +
            " timed_out_frames=" + std::to_string(report.timed_out_frames) +
            " invalid_datagrams=" + std::to_string(report.invalid_datagrams) +
            " fragment_loss=" + std::to_string(fragment_loss_percent) + "%" +
            " jitter_avg=" + std::to_string(report.jitter_avg_ms) + "ms" +
            " jitter_max=" + std::to_string(report.jitter_max_ms) + "ms";
}

// 判断两个端点是否相同（端口 + IP 均相等）。
bool UdpStreamingBackend::SameEndpoint(const sockaddr_in &lhs, const sockaddr_in &rhs) const {
    return lhs.sin_port == rhs.sin_port && lhs.sin_addr.s_addr == rhs.sin_addr.s_addr;
}

}  // namespace udp
}  // namespace transport
}  // namespace modules
}  // namespace sserver
