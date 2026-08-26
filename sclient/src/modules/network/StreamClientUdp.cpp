
// UDP 接收实现。
//
// 一份视频帧通常会被发送端拆成多个 UDP 数据报。本文件负责把这些数据报
// 校验、按帧序号归组、写回原始帧缓冲，并在必要时使用 XOR-FEC 或 NACK
// 机制处理丢包。完整帧随后可直接交给上层，或先进入 jitter buffer 等待
// 合适的播放时机。
#include "modules/network/StreamClientInternal.h"

#include <algorithm>
#include <cmath>

namespace sclient {

using network_internal::BuildSocketError;
using network_internal::MonotonicNowNs;
using network_internal::XorInto;
using network_internal::kRecentCompletedUdpSequenceWindow;
using network_internal::kUdpAssemblyTimeoutNs;

// 从 UDP socket 中接收并输出一帧完整的视频数据。
//
// 每次循环先尝试弹出已经在 jitter buffer 中就绪的帧，然后尽可能地
// drain（排空）内核接收队列中的数据报。排空后再执行超时清理、FEC、NACK
// 和阻塞式 recv，既能提高吞吐量，也能避免在已有完整帧时继续等待网络。
bool StreamClient::ReceiveUdpFrame(ReceivedFrame *frame, std::string *error_message) {
    while (running_.load()) {
        // 使用单调时钟，避免系统时间回拨影响超时和 jitter 计算。
        const std::uint64_t loop_now_ns = MonotonicNowNs();
        if (TryPopReadyUdpFrame(frame, loop_now_ns)) {
            return true;
        }

        // 非阻塞地一次处理多个数据报，减少高码率场景下的系统调用次数。
        bool drained_any_datagram = false;
        while (running_.load()) {
            const ssize_t received = recv(socket_fd_, udp_datagram_buffer_.data(), udp_datagram_buffer_.size(), MSG_DONTWAIT);
            if (received < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                if (error_message != nullptr) {
                    *error_message = BuildSocketError("failed to receive UDP datagram");
                }
                return false;
            }
            if (received == 0) {
                if (error_message != nullptr) {
                    *error_message = "UDP socket closed";
                }
                return false;
            }

            drained_any_datagram = true;
            bool frame_ready = false;
            // drain 阶段先把可读报文处理完，完整帧暂存到 jitter buffer，
            // 统一在排空后再决定是否输出。
            if (!ProcessUdpDatagram(
                        udp_datagram_buffer_.data(),
                        static_cast<std::size_t>(received),
                        MonotonicNowNs(),
                        true,
                        frame,
                        &frame_ready)) {
                continue;
            }
            if (frame_ready) {
                return true;
            }
        }

        // 排空后再做一次恢复/弹出，处理“最后一个数据报刚好补齐帧”的情况。
        const std::uint64_t after_drain_now_ns = MonotonicNowNs();
        if (FinalizeRecoverableUdpFecFrames(after_drain_now_ns, true, frame)) {
            return true;
        }
        if (TryPopReadyUdpFrame(frame, after_drain_now_ns)) {
            return true;
        }
        if (drained_any_datagram) {
            continue;
        }

        // 没有新数据时才进行维护操作；有数据时优先继续排空，避免维护开销
        // 打断连续的数据报处理。
        PruneExpiredUdpAssemblies(after_drain_now_ns);
        if (FinalizeRecoverableUdpFecFrames(after_drain_now_ns, false, frame)) {
            return true;
        }
        MaybeSendUdpNackRequests(after_drain_now_ns);
        if (TryPopReadyUdpFrame(frame, after_drain_now_ns)) {
            return true;
        }

        const ssize_t received = recv(socket_fd_, udp_datagram_buffer_.data(), udp_datagram_buffer_.size(), 0);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            if (error_message != nullptr) {
                *error_message = BuildSocketError("failed to receive UDP datagram");
            }
            return false;
        }
        if (received == 0) {
            if (error_message != nullptr) {
                *error_message = "UDP socket closed";
            }
            return false;
        }

        bool frame_ready = false;
        // 阻塞接收路径只处理一个报文；若它直接补齐并满足输出条件，可立即返回。
        if (!ProcessUdpDatagram(
                    udp_datagram_buffer_.data(),
                    static_cast<std::size_t>(received),
                    MonotonicNowNs(),
                    false,
                    frame,
                    &frame_ready)) {
            continue;
        }
        if (frame_ready) {
            return true;
        }
    }

    if (error_message != nullptr) {
        *error_message = "client stopped";
    }
    return false;
}

// 解析并处理一个 UDP 数据报。
// 返回值表示本次调用是否已经立即完成并输出了一帧；frame_ready 是同一语义
// 的显式输出参数。启用 jitter buffer 且 defer_ready_pop 为 true 时，即使帧
// 已经组装完成，也只会入 buffer，因此本函数仍返回 false。
bool StreamClient::ProcessUdpDatagram(
        const char *data,
        std::size_t datagram_size,
        std::uint64_t now_ns,
        bool defer_ready_pop,
        ReceivedFrame *frame,
        bool *frame_ready) {
    UdpStatsSnapshotGuard snapshot_guard(this);
    if (frame_ready != nullptr) {
        *frame_ready = false;
    }

    ++udp_receive_stats_.datagrams_received;
    // 过期组装状态在每个数据报到达时顺手清理，防止丢包后 map 无限增长。
    PruneExpiredUdpAssemblies(now_ns);
    if (datagram_size < sizeof(protocol::MessageHeader) + sizeof(protocol::UdpFrameFragmentHeader)) {
        ++udp_receive_stats_.invalid_datagrams;
        return false;
    }

    // 先读取公共消息头，再读取 UDP 专用的分片头；memcpy 可避免未对齐访问。
    protocol::MessageHeader header{};
    std::memcpy(&header, data, sizeof(header));
    if (!protocol::HasValidMessageMagic(header)) {
        ++udp_receive_stats_.invalid_datagrams;
        return false;
    }
    if (header.message_type != static_cast<std::uint16_t>(protocol::MessageType::kAvStream) ||
        header.payload_length < sizeof(protocol::UdpFrameFragmentHeader)) {
        ++udp_receive_stats_.invalid_datagrams;
        return false;
    }

    protocol::UdpFrameFragmentHeader fragment_header{};
    std::memcpy(&fragment_header, data + sizeof(header), sizeof(fragment_header));
    // 数据分片的 index 必须落在 [0, fragment_count)；奇偶校验分片没有实际
    // 数据索引，因此只检查其 role 合法性。
    if (fragment_header.fragment_count == 0 ||
        fragment_header.fragment_role > static_cast<std::uint16_t>(protocol::UdpFragmentRole::kXorParity) ||
        (fragment_header.fragment_role == static_cast<std::uint16_t>(protocol::UdpFragmentRole::kData) &&
         fragment_header.fragment_index >= fragment_header.fragment_count)) {
        ++udp_receive_stats_.invalid_datagrams;
        return false;
    }

    const std::size_t fragment_payload_size = datagram_size - sizeof(header) - sizeof(fragment_header);
    if (fragment_payload_size + sizeof(fragment_header) != header.payload_length) {
        ++udp_receive_stats_.invalid_datagrams;
        return false;
    }
    if (fragment_header.fragment_role == static_cast<std::uint16_t>(protocol::UdpFragmentRole::kData) &&
        fragment_header.fragment_offset + fragment_payload_size > fragment_header.frame_payload_size) {
        ++udp_receive_stats_.invalid_datagrams;
        return false;
    }

    // 测试用的丢包注入发生在协议校验之后，确保不会把“人为丢弃”统计为无效报文。
    if (ShouldInjectUdpLoss(fragment_header)) {
        return false;
    }
    if (IsRecentlyCompletedUdpFrameSequence(fragment_header.frame_sequence)) {
        ++udp_receive_stats_.stale_datagrams_dropped;
        return false;
    }

    // map 的 key 是帧序号：首次见到该序号时创建组装上下文，后续分片复用它。
    UdpFrameAssembly &assembly = udp_assemblies_[fragment_header.frame_sequence];
    if (assembly.received_fragments.empty()) {
        assembly.header = header;
        assembly.metadata.sequence = fragment_header.frame_sequence;
        assembly.metadata.capture_timestamp_ns = config_.expect_metadata ? fragment_header.capture_timestamp_ns : 0;
        assembly.metadata.encode_start_timestamp_ns = config_.expect_metadata ? fragment_header.encode_start_timestamp_ns : 0;
        assembly.metadata.encode_end_timestamp_ns = config_.expect_metadata ? fragment_header.encode_end_timestamp_ns : 0;
        assembly.metadata.transport_send_timestamp_ns = config_.expect_metadata ? fragment_header.transport_send_timestamp_ns : 0;
        assembly.payload.resize(fragment_header.frame_payload_size);
        assembly.received_fragments.assign(fragment_header.fragment_count, false);
        assembly.fragment_offsets.assign(fragment_header.fragment_count, 0);
        assembly.fragment_payload_sizes.assign(fragment_header.fragment_count, 0);
        assembly.first_seen_timestamp_ns = now_ns;
        assembly.last_fragment_timestamp_ns = now_ns;
    }

    // 同一帧的所有分片必须描述同一个尺寸/分片数。若发送端参数发生变化，
    // 丢弃旧组装状态，避免把不同帧布局的数据写进同一缓冲区。
    if (assembly.received_fragments.size() != fragment_header.fragment_count ||
        assembly.payload.size() != fragment_header.frame_payload_size) {
        ++udp_receive_stats_.timed_out_frames;
        udp_receive_stats_.timed_out_fragments +=
                static_cast<std::uint64_t>(assembly.received_fragments.size() - assembly.received_fragment_count);
        udp_test_dropped_fragments_.erase(fragment_header.frame_sequence);
        udp_assemblies_.erase(fragment_header.frame_sequence);
        return false;
    }

    if (fragment_header.fragment_role == static_cast<std::uint16_t>(protocol::UdpFragmentRole::kXorParity)) {
        // XOR 校验分片只保存一份，不计入 received_fragment_count；它会在
        // 缺少恰好一个数据分片时参与恢复。
        if (config_.udp_fec_enabled) {
            assembly.fec_payload.assign(
                    reinterpret_cast<const std::uint8_t *>(data + sizeof(header) + sizeof(fragment_header)),
                    reinterpret_cast<const std::uint8_t *>(data + datagram_size));
            assembly.has_fec_payload = true;
            assembly.fec_payload_timestamp_ns = now_ns;
            ++udp_receive_stats_.fec_fragments_received;
        }
        assembly.last_fragment_timestamp_ns = now_ns;
    } else {
        // 数据分片按 fragment_offset 写入整帧 payload。重复分片不重复拷贝。
        ++udp_receive_stats_.fragments_received;
        if (!assembly.received_fragments[fragment_header.fragment_index]) {
            std::memcpy(
                    assembly.payload.data() + fragment_header.fragment_offset,
                    data + sizeof(header) + sizeof(fragment_header),
                    fragment_payload_size);
            assembly.received_fragments[fragment_header.fragment_index] = true;
            assembly.fragment_offsets[fragment_header.fragment_index] = fragment_header.fragment_offset;
            assembly.fragment_payload_sizes[fragment_header.fragment_index] =
                    static_cast<std::uint32_t>(fragment_payload_size);
            ++assembly.received_fragment_count;
            assembly.last_fragment_timestamp_ns = now_ns;
        } else {
            ++udp_receive_stats_.duplicate_fragments;
        }
    }

    // 收到数据分片后立即尝试 FEC；如果最后一个缺失分片已经通过 FEC 补齐，
    // 可以在当前数据报处理结束前直接完成帧。 
    if (fragment_header.fragment_role == static_cast<std::uint16_t>(protocol::UdpFragmentRole::kData) &&
        assembly.received_fragment_count < assembly.received_fragments.size()) {
        MaybeRecoverWithUdpFec(&assembly);
    }
    if (assembly.received_fragment_count == assembly.received_fragments.size() &&
        FinalizeCompletedUdpFrame(fragment_header.frame_sequence, &assembly, now_ns, defer_ready_pop, frame)) {
        if (frame_ready != nullptr) {
            *frame_ready = true;
        }
        return true;
    }

    return false;
}

// 扫描所有未完成帧，尝试利用已经到达的 XOR 分片恢复单个缺失分片。
// 该函数在 drain 阶段和维护阶段都会调用，因此可能一次恢复多帧。
bool StreamClient::FinalizeRecoverableUdpFecFrames(std::uint64_t now_ns, bool defer_ready_pop, ReceivedFrame *frame) {
    UdpStatsSnapshotGuard snapshot_guard(this);
    std::vector<std::uint64_t> recovered_sequences;
    for (std::map<std::uint64_t, UdpFrameAssembly>::iterator it = udp_assemblies_.begin();
         it != udp_assemblies_.end();
         ++it) {
        UdpFrameAssembly &assembly = it->second;
        if (assembly.received_fragment_count >= assembly.received_fragments.size()) {
            continue;
        }
        if (MaybeRecoverWithUdpFec(&assembly)) {
            recovered_sequences.push_back(it->first);
        }
    }

    for (std::size_t index = 0; index < recovered_sequences.size(); ++index) {
        const std::uint64_t sequence = recovered_sequences[index];
        std::map<std::uint64_t, UdpFrameAssembly>::iterator it = udp_assemblies_.find(sequence);
        if (it == udp_assemblies_.end()) {
            continue;
        }
        if (it->second.received_fragment_count != it->second.received_fragments.size()) {
            continue;
        }
        if (FinalizeCompletedUdpFrame(sequence, &it->second, now_ns, defer_ready_pop, frame)) {
            return true;
        }
    }
    return false;
}

// 根据目标延迟、最大等待时间和最大深度，从 jitter buffer 输出下一帧。
// buffer 按序列号排序；如果期望序号缺失且等待超限，则跳过缺失序号，
// 防止单个丢帧把后续所有帧永久卡住。
bool StreamClient::TryPopReadyUdpFrame(ReceivedFrame *frame, std::uint64_t now_ns) {
    UdpStatsSnapshotGuard snapshot_guard(this);
    if (frame == nullptr || !config_.udp_jitter_buffer_enabled || udp_jitter_buffer_.empty()) {
        return false;
    }

    const std::uint64_t target_delay_ns = CurrentJitterBufferTargetDelayNs();

    const std::string &mode = config_.udp_jitter_buffer_strategy;
    const bool use_adaptive_params = (mode == "auto" || mode == "off" || mode == "low" || mode == "smooth");
    const int effective_max_wait_ms = use_adaptive_params
            ? adaptive_jitter_.quality_max_wait_ms()
            : config_.udp_jitter_buffer_max_wait_ms;
    const std::size_t effective_max_frames = use_adaptive_params
            ? adaptive_jitter_.quality_max_frames()
            : config_.udp_jitter_buffer_max_frames;

    const std::uint64_t max_wait_ns =
            static_cast<std::uint64_t>(std::max(effective_max_wait_ms, 0)) * 1000ULL * 1000ULL;
    // 首帧到来时以其序列号作为输出基准；之后严格按递增序列消费。
    std::map<std::uint64_t, BufferedUdpFrame>::iterator oldest = udp_jitter_buffer_.begin();
    if (!has_next_jitter_buffer_sequence_) {
        next_jitter_buffer_sequence_ = oldest->first;
        has_next_jitter_buffer_sequence_ = true;
    }

    std::map<std::uint64_t, BufferedUdpFrame>::iterator expected = udp_jitter_buffer_.find(next_jitter_buffer_sequence_);
    if (expected == udp_jitter_buffer_.end()) {
        const std::uint64_t oldest_wait_ns = now_ns > oldest->second.buffered_timestamp_ns
                ? now_ns - oldest->second.buffered_timestamp_ns
                : 0;
        // 期望帧不存在：等待达到 max_wait，或 buffer 已达到容量上限时跳过它。
        const bool should_skip_missing =
                (max_wait_ns > 0 && oldest_wait_ns >= max_wait_ns) ||
                udp_jitter_buffer_.size() >= std::max<std::size_t>(effective_max_frames, 1);
        if (!should_skip_missing) {
            return false;
        }

        if (oldest->first > next_jitter_buffer_sequence_) {
            udp_receive_stats_.jitter_buffer_skipped_frames += oldest->first - next_jitter_buffer_sequence_;
        }
        next_jitter_buffer_sequence_ = oldest->first;
        expected = oldest;
    }

    const std::uint64_t wait_ns = now_ns > expected->second.buffered_timestamp_ns
            ? now_ns - expected->second.buffered_timestamp_ns
            : 0;
    // 已有帧只有在满足目标延迟/深度上限/最大等待时间之一时才输出。
    const bool reached_target_delay = target_delay_ns == 0 || wait_ns >= target_delay_ns;
    const bool reached_depth_limit =
            udp_jitter_buffer_.size() >= std::max<std::size_t>(effective_max_frames, 1);
    const bool reached_max_wait = max_wait_ns > 0 && wait_ns >= max_wait_ns;
    if (!reached_target_delay && !reached_depth_limit && !reached_max_wait) {
        return false;
    }

    *frame = std::move(expected->second.frame);
    RecordJitterBufferWait(wait_ns);
    ++udp_receive_stats_.jitter_buffer_emitted_frames;
    udp_jitter_buffer_.erase(expected);
    ++next_jitter_buffer_sequence_;
    if (udp_jitter_buffer_.empty()) {
        has_next_jitter_buffer_sequence_ = false;
    }
    UpdateJitterBufferDepthStats();
    return true;
}

// 将已经收齐（或经 FEC 恢复）的组装状态转换为 ReceivedFrame，并清理该帧的
// 临时状态。启用 jitter buffer 时只入队，不一定在当前调用中立即输出。
bool StreamClient::FinalizeCompletedUdpFrame(
        std::uint64_t frame_sequence,
        UdpFrameAssembly *assembly,
        std::uint64_t now_ns,
        bool defer_ready_pop,
        ReceivedFrame *frame) {
    if (assembly == nullptr || frame == nullptr || assembly->received_fragment_count != assembly->received_fragments.size()) {
        return false;
    }

    ReceivedFrame completed_frame;
    completed_frame.header = assembly->header;
    completed_frame.metadata = assembly->metadata;
    completed_frame.sender_metadata_available =
            config_.expect_metadata && network_internal::HasSenderLatencyMetadata(completed_frame.metadata);
    completed_frame.receive_timestamp_ns = now_ns;
    completed_frame.payload.swap(assembly->payload);
    // 测试配置可人为增加确定性的延迟，用于验证 jitter buffer 行为。
    completed_frame.receive_timestamp_ns += InjectedUdpJitterDelayNs(completed_frame);
    ++udp_receive_stats_.completed_frames;
    OnCompletedUdpFrame(completed_frame, now_ns);
    RememberCompletedUdpFrameSequence(frame_sequence);
    udp_test_dropped_fragments_.erase(frame_sequence);
    udp_assemblies_.erase(frame_sequence);
    if (!config_.udp_jitter_buffer_enabled) {
        *frame = std::move(completed_frame);
        return true;
    }

    BufferCompletedUdpFrame(std::move(completed_frame));
    if (defer_ready_pop) {
        return false;
    }
    MaybeSendUdpNackRequests(now_ns);
    return TryPopReadyUdpFrame(frame, now_ns);
}

// 计算当前 jitter buffer 目标等待时长。auto/off/low/smooth 由自适应控制器
// 决定；fixed/adaptive 等模式则使用配置值（adaptive 会叠加观测到的抖动）。
std::uint64_t StreamClient::CurrentJitterBufferTargetDelayNs() {
    const std::string &mode = config_.udp_jitter_buffer_strategy;

    if (mode == "auto" || mode == "off" || mode == "low" || mode == "smooth") {
        const std::uint64_t target_ns = adaptive_jitter_.TargetDelayNs();
        udp_receive_stats_.jitter_buffer_target_delay_ms =
                static_cast<double>(target_ns) / 1000000.0;
        return target_ns;
    }

    double target_delay_ms = static_cast<double>(std::max(config_.udp_jitter_buffer_target_delay_ms, 0));
    if (mode == "adaptive" && udp_receive_stats_.jitter_samples > 0) {
        const double adaptive_max_delay_ms = static_cast<double>(
                std::max(config_.udp_jitter_buffer_adaptive_max_delay_ms, config_.udp_jitter_buffer_target_delay_ms));
        target_delay_ms = std::min(adaptive_max_delay_ms, target_delay_ms + udp_receive_stats_.jitter_avg_ms * 2.0);
    }

    udp_receive_stats_.jitter_buffer_target_delay_ms = target_delay_ms;
    return static_cast<std::uint64_t>(target_delay_ms * 1000000.0);
}

// 生成测试用的确定性延迟模式，不影响生产配置下的正常接收。
std::uint64_t StreamClient::InjectedUdpJitterDelayNs(const ReceivedFrame &frame) const {
    if (config_.udp_test_jitter_pattern == "none" || config_.udp_test_jitter_amplitude_ms <= 0) {
        return 0;
    }

    const int period = std::max(config_.udp_test_jitter_period, 2);
    const std::uint64_t sequence = frame.metadata.sequence;
    double delay_ms = 0.0;

    if (config_.udp_test_jitter_pattern == "saw") {
        const int phase = static_cast<int>(sequence % static_cast<std::uint64_t>(period));
        delay_ms = static_cast<double>(config_.udp_test_jitter_amplitude_ms) *
                static_cast<double>(phase) /
                static_cast<double>(period - 1);
    } else if (config_.udp_test_jitter_pattern == "burst") {
        const int phase = static_cast<int>(sequence % static_cast<std::uint64_t>(period));
        delay_ms = phase < (period / 2)
                ? static_cast<double>(config_.udp_test_jitter_amplitude_ms)
                : 0.0;
    } else if (config_.udp_test_jitter_pattern == "alternate") {
        delay_ms = (sequence % 2ULL) == 0ULL
                ? 0.0
                : static_cast<double>(config_.udp_test_jitter_amplitude_ms);
    }

    return static_cast<std::uint64_t>(delay_ms * 1000000.0);
}

// 按配置模拟丢失数据分片。每个分片只注入一次，后续同一分片仍允许通过，
// 这样可以测试 NACK 重传是否真正恢复了数据。
bool StreamClient::ShouldInjectUdpLoss(const protocol::UdpFrameFragmentHeader &fragment_header) {
    if (config_.udp_test_loss_pattern == "none" ||
        fragment_header.fragment_count == 0 ||
        fragment_header.fragment_role != static_cast<std::uint16_t>(protocol::UdpFragmentRole::kData)) {
        return false;
    }

    bool should_drop = false;
    if (config_.udp_test_loss_pattern == "single") {
        const std::uint64_t period = static_cast<std::uint64_t>(std::max(config_.udp_test_loss_period, 1));
        should_drop = (fragment_header.frame_sequence % period) == 0ULL && fragment_header.fragment_index == 0;
    } else if (config_.udp_test_loss_pattern == "burst") {
        const std::uint64_t period = static_cast<std::uint64_t>(std::max(config_.udp_test_loss_period, 1));
        const std::size_t drop_count = std::max(config_.udp_test_loss_count, 1);
        should_drop = (fragment_header.frame_sequence % period) == 0ULL &&
                fragment_header.fragment_index < std::min<std::size_t>(drop_count, fragment_header.fragment_count);
    } else if (config_.udp_test_loss_pattern == "alternate") {
        should_drop = (fragment_header.frame_sequence % 2ULL) == 1ULL && fragment_header.fragment_index == 0;
    } else {
        return false;
    }

    if (!should_drop) {
        return false;
    }

    std::vector<bool> &dropped_fragments = udp_test_dropped_fragments_[fragment_header.frame_sequence];
    if (dropped_fragments.empty()) {
        dropped_fragments.assign(fragment_header.fragment_count, false);
    } else if (dropped_fragments.size() != fragment_header.fragment_count) {
        dropped_fragments.assign(fragment_header.fragment_count, false);
    }

    if (dropped_fragments[fragment_header.fragment_index]) {
        return false;
    }

    dropped_fragments[fragment_header.fragment_index] = true;
    ++udp_receive_stats_.injected_loss_datagrams;
    ++udp_receive_stats_.injected_loss_fragments;
    return true;
}

// 使用 XOR 奇偶校验恢复一个缺失的数据分片。
// XOR 只能在“恰好缺一个分片”且已收到 parity 的情况下恢复；缺失多个时
// 信息不足，函数保持组装未完成并等待 NACK 重传。
bool StreamClient::MaybeRecoverWithUdpFec(UdpFrameAssembly *assembly) {
    if (!config_.udp_fec_enabled ||
        assembly == nullptr ||
        !assembly->has_fec_payload ||
        assembly->fec_payload_timestamp_ns == 0 ||
        assembly->received_fragments.empty() ||
        assembly->received_fragment_count >= assembly->received_fragments.size()) {
        return false;
    }

    std::size_t missing_index = assembly->received_fragments.size();
    std::size_t missing_count = 0;
    for (std::size_t index = 0; index < assembly->received_fragments.size(); ++index) {
        if (!assembly->received_fragments[index]) {
            missing_index = index;
            ++missing_count;
            if (missing_count > 1) {
                return false;
            }
        }
    }
    if (missing_count != 1) {
        return false;
    }

    // 根据相邻分片的 offset/size 推断缺失分片在整帧中的位置和长度。
    std::uint32_t missing_offset = 0;
    if (missing_index > 0) {
        const std::uint32_t previous_size = assembly->fragment_payload_sizes[missing_index - 1];
        missing_offset = assembly->fragment_offsets[missing_index - 1] + previous_size;
    }

    std::uint32_t missing_size = 0;
    if (missing_index + 1 < assembly->received_fragments.size()) {
        const std::uint32_t next_offset = assembly->fragment_offsets[missing_index + 1];
        if (next_offset < missing_offset) {
            return false;
        }
        missing_size = next_offset - missing_offset;
    } else if (assembly->payload.size() >= missing_offset) {
        missing_size = static_cast<std::uint32_t>(assembly->payload.size() - missing_offset);
    }

    if (missing_size == 0 || missing_size > assembly->fec_payload.size() ||
        static_cast<std::size_t>(missing_offset) + missing_size > assembly->payload.size()) {
        return false;
    }

    // parity = fragment0 XOR fragment1 ...，从 parity 中逐个 XOR 已收到的
    // 分片即可得到缺失分片内容。
    std::vector<std::uint8_t> recovered = assembly->fec_payload;
    for (std::size_t index = 0; index < assembly->received_fragments.size(); ++index) {
        if (!assembly->received_fragments[index]) {
            continue;
        }
        const std::uint32_t fragment_offset = assembly->fragment_offsets[index];
        const std::uint32_t fragment_size = assembly->fragment_payload_sizes[index];
        if (static_cast<std::size_t>(fragment_offset) + fragment_size > assembly->payload.size()) {
            return false;
        }
        XorInto(&recovered, assembly->payload.data() + fragment_offset, fragment_size);
    }

    std::memcpy(
            assembly->payload.data() + missing_offset,
            recovered.data(),
            missing_size);
    assembly->received_fragments[missing_index] = true;
    assembly->fragment_offsets[missing_index] = missing_offset;
    assembly->fragment_payload_sizes[missing_index] = missing_size;
    ++assembly->received_fragment_count;
    ++udp_receive_stats_.fec_recovered_fragments;
    if (assembly->received_fragment_count == assembly->received_fragments.size()) {
        ++udp_receive_stats_.fec_recovered_frames;
    }
    return true;
}

// 判断序列号是否刚刚完成过。UDP 可能重复投递旧数据报，短窗口可避免重复
// 组装/输出，同时不会让长期运行的集合无限增长。
bool StreamClient::IsRecentlyCompletedUdpFrameSequence(std::uint64_t sequence) const {
    return recently_completed_udp_sequences_.find(sequence) != recently_completed_udp_sequences_.end();
}

// 记录已完成帧，并将记录限制在固定窗口内。
void StreamClient::RememberCompletedUdpFrameSequence(std::uint64_t sequence) {
    const std::pair<std::unordered_set<std::uint64_t>::iterator, bool> inserted =
            recently_completed_udp_sequences_.insert(sequence);
    if (!inserted.second) {
        return;
    }

    recently_completed_udp_sequence_order_.push_back(sequence);
    while (recently_completed_udp_sequence_order_.size() > kRecentCompletedUdpSequenceWindow) {
        const std::uint64_t oldest_sequence = recently_completed_udp_sequence_order_.front();
        recently_completed_udp_sequence_order_.pop_front();
        recently_completed_udp_sequences_.erase(oldest_sequence);
    }
}

// 扫描尚未完成的帧，按延迟和重试间隔发送 NACK 请求。
void StreamClient::MaybeSendUdpNackRequests(std::uint64_t now_ns) {
    UdpStatsSnapshotGuard snapshot_guard(this);
    if (!config_.udp_nack_enabled || config_.udp_nack_max_requests_per_packet == 0) {
        return;
    }

    const std::uint64_t nack_delay_ns =
            static_cast<std::uint64_t>(std::max(config_.udp_nack_request_delay_ms, 0)) * 1000ULL * 1000ULL;
    const std::uint64_t nack_retry_ns =
            static_cast<std::uint64_t>(std::max(config_.udp_nack_retry_interval_ms, 0)) * 1000ULL * 1000ULL;

    for (std::map<std::uint64_t, UdpFrameAssembly>::iterator it = udp_assemblies_.begin();
         it != udp_assemblies_.end();
         ++it) {
        UdpFrameAssembly &assembly = it->second;
        if (assembly.received_fragments.empty() ||
            assembly.received_fragment_count >= assembly.received_fragments.size() ||
            assembly.first_seen_timestamp_ns == 0 ||
            now_ns < assembly.first_seen_timestamp_ns ||
            now_ns - assembly.first_seen_timestamp_ns < nack_delay_ns ||
            assembly.nack_attempts >= config_.udp_nack_max_retries) {
            continue;
        }
        if (assembly.last_nack_timestamp_ns != 0 &&
            now_ns >= assembly.last_nack_timestamp_ns &&
            now_ns - assembly.last_nack_timestamp_ns < nack_retry_ns) {
            continue;
        }

        // 只发送缺失的 data fragment index；parity 分片不需要请求。
        std::vector<std::uint16_t> missing_fragments;
        missing_fragments.reserve(assembly.received_fragments.size() - assembly.received_fragment_count);
        for (std::size_t fragment_index = 0; fragment_index < assembly.received_fragments.size(); ++fragment_index) {
            if (!assembly.received_fragments[fragment_index]) {
                missing_fragments.push_back(static_cast<std::uint16_t>(fragment_index));
            }
        }
        if (missing_fragments.empty()) {
            continue;
        }

        if (SendUdpNack(assembly.metadata.sequence, missing_fragments)) {
            assembly.last_nack_timestamp_ns = now_ns;
            ++assembly.nack_attempts;
        }
    }
}

// 将缺失分片列表拆成若干 UDP NACK 报文并发送。一个报文中的请求数量受
// udp_nack_max_requests_per_packet 限制，避免控制报文过大。
bool StreamClient::SendUdpNack(std::uint64_t frame_sequence, const std::vector<std::uint16_t> &missing_fragments) {
    if (socket_fd_ < 0 || missing_fragments.empty()) {
        return false;
    }

    const std::size_t max_requests = std::max<std::size_t>(1, config_.udp_nack_max_requests_per_packet);
    for (std::size_t offset = 0; offset < missing_fragments.size(); offset += max_requests) {
        const std::size_t request_count = std::min(max_requests, missing_fragments.size() - offset);
        // NACK 也是统一消息协议：公共头 + NACK 头 + 若干 NACK item。
        protocol::MessageHeader header{};
        protocol::FillMessageMagic(header.head_id);
        header.message_type = static_cast<std::uint16_t>(protocol::MessageType::kUdpNack);
        header.payload_length = static_cast<std::uint32_t>(
                sizeof(protocol::UdpNackHeader) + request_count * sizeof(protocol::UdpNackItem));

        protocol::UdpNackHeader nack_header{};
        nack_header.request_timestamp_ns = MonotonicNowNs();
        nack_header.request_count = static_cast<std::uint16_t>(request_count);

        std::vector<char> payload(sizeof(header) + header.payload_length);
        std::memcpy(payload.data(), &header, sizeof(header));
        std::memcpy(payload.data() + sizeof(header), &nack_header, sizeof(nack_header));
        for (std::size_t index = 0; index < request_count; ++index) {
            protocol::UdpNackItem item{};
            item.frame_sequence = frame_sequence;
            item.fragment_index = missing_fragments[offset + index];
            std::memcpy(
                    payload.data() + sizeof(header) + sizeof(nack_header) + index * sizeof(item),
                    &item,
                    sizeof(item));
        }

        // 接收线程和 KeepAlive 线程可能同时发送，使用同一把锁保护 socket。
        std::lock_guard<std::mutex> lock(send_mutex_);
        const ssize_t sent = send(socket_fd_, payload.data(), payload.size(), MSG_NOSIGNAL);
        if (sent != static_cast<ssize_t>(payload.size())) {
            return false;
        }
        ++udp_receive_stats_.nack_requests_sent;
        udp_receive_stats_.nack_fragments_requested += request_count;
    }

    return true;
}

// 连接重建或关闭时清空 UDP 统计、组装、去重和 jitter 自适应状态。
void StreamClient::ResetUdpState() {
    udp_receive_stats_ = UdpReceiveStats();
    udp_receive_stats_.jitter_buffer_target_delay_ms =
            static_cast<double>(std::max(config_.udp_jitter_buffer_target_delay_ms, 0));
    last_completed_frame_sequence_ = 0;
    has_last_completed_frame_sequence_ = false;
    next_jitter_buffer_sequence_ = 0;
    has_next_jitter_buffer_sequence_ = false;
    udp_test_dropped_fragments_.clear();
    recently_completed_udp_sequence_order_.clear();
    recently_completed_udp_sequences_.clear();
    previous_network_latency_ms_ = 0.0;
    has_previous_network_latency_ = false;
    adaptive_jitter_.Reset();
    PublishUdpReceiveStatsSnapshot();
}

// 重置 RTP 专用的单帧组装状态；UDP 状态与 RTP 状态彼此独立。
void StreamClient::ResetRtpState() {
    rtp_frame_assembly_.payload.clear();
    rtp_frame_assembly_.timestamp = 0;
    rtp_frame_assembly_.ssrc = 0;
    rtp_frame_assembly_.next_sequence_number = 0;
    rtp_frame_assembly_.capture_timestamp_ns = 0;
    rtp_frame_assembly_.transport_send_timestamp_ns = 0;
    rtp_frame_assembly_.active = false;
    rtp_frame_assembly_.has_sequence_number = false;
    rtp_frame_assembly_.sender_metadata_available = false;
    rtp_frame_assembly_.sender_metadata_invalid = false;
    rtp_frame_assembly_.frame_damaged = false;
    rtp_frame_assembly_.fu_in_progress = false;
}

// 删除超过 kUdpAssemblyTimeoutNs 未收到新分片的帧组装状态，并统计缺失分片。
void StreamClient::PruneExpiredUdpAssemblies(std::uint64_t now_ns) {
    UdpStatsSnapshotGuard snapshot_guard(this);
    for (std::map<std::uint64_t, UdpFrameAssembly>::iterator it = udp_assemblies_.begin();
         it != udp_assemblies_.end();) {
        const UdpFrameAssembly &assembly = it->second;
        if (assembly.last_fragment_timestamp_ns != 0 &&
            now_ns > assembly.last_fragment_timestamp_ns &&
            now_ns - assembly.last_fragment_timestamp_ns > kUdpAssemblyTimeoutNs) {
            ++udp_receive_stats_.timed_out_frames;
            udp_receive_stats_.timed_out_fragments += static_cast<std::uint64_t>(
                    assembly.received_fragments.size() - assembly.received_fragment_count);
            udp_test_dropped_fragments_.erase(it->first);
            it = udp_assemblies_.erase(it);
            continue;
        }
        ++it;
    }
}

// 将完整帧放入按序列号排序的 jitter buffer。相同序列号已存在时视为重复帧。
void StreamClient::BufferCompletedUdpFrame(ReceivedFrame &&frame) {
    const std::uint64_t sequence = frame.metadata.sequence;
    if (udp_jitter_buffer_.find(sequence) != udp_jitter_buffer_.end()) {
        ++udp_receive_stats_.jitter_buffer_dropped_frames;
        return;
    }

    BufferedUdpFrame buffered_frame;
    buffered_frame.buffered_timestamp_ns = frame.receive_timestamp_ns;
    buffered_frame.frame = std::move(frame);
    udp_jitter_buffer_.emplace(sequence, std::move(buffered_frame));
    UpdateJitterBufferDepthStats();
}

// 完成一帧时更新乱序、网络延迟和抖动统计，并把样本交给自适应控制器。
void StreamClient::OnCompletedUdpFrame(const ReceivedFrame &frame, std::uint64_t now_ns) {
    const std::uint64_t sequence = frame.metadata.sequence;
    // 完成顺序不连续说明发生过乱序或丢帧（jitter buffer 之后仍会按序输出）。
    if (has_last_completed_frame_sequence_ && sequence != last_completed_frame_sequence_ + 1) {
        ++udp_receive_stats_.reordered_frames;
    }
    last_completed_frame_sequence_ = sequence;
    has_last_completed_frame_sequence_ = true;

    if (frame.metadata.transport_send_timestamp_ns == 0 || frame.receive_timestamp_ns < frame.metadata.transport_send_timestamp_ns) {
        return;
    }

    // 发送端时间戳与本地接收时间戳均为单调时钟时，二者差值就是网络延迟。
    const double network_latency_ms = static_cast<double>(
            frame.receive_timestamp_ns - frame.metadata.transport_send_timestamp_ns) / 1000000.0;
    if (has_previous_network_latency_) {
        const double jitter_ms = std::abs(network_latency_ms - previous_network_latency_ms_);
        udp_receive_stats_.jitter_last_ms = jitter_ms;
        ++udp_receive_stats_.jitter_samples;
        udp_receive_stats_.jitter_avg_ms +=
                (jitter_ms - udp_receive_stats_.jitter_avg_ms) /
                static_cast<double>(udp_receive_stats_.jitter_samples);
        if (jitter_ms > udp_receive_stats_.jitter_max_ms) {
            udp_receive_stats_.jitter_max_ms = jitter_ms;
        }

        const std::uint64_t total_frag = udp_receive_stats_.fragments_received + udp_receive_stats_.timed_out_fragments;
        const double loss_pct = total_frag == 0 ? 0.0
                : static_cast<double>(udp_receive_stats_.timed_out_fragments) * 100.0 / static_cast<double>(total_frag);
        const std::uint64_t total_emitted = udp_receive_stats_.jitter_buffer_emitted_frames;
        const double skip_pct = total_emitted == 0 ? 0.0
                : static_cast<double>(udp_receive_stats_.jitter_buffer_skipped_frames) * 100.0 / static_cast<double>(total_emitted);

        adaptive_jitter_.RecordJitter(jitter_ms, loss_pct, skip_pct, now_ns);
        udp_receive_stats_.jitter_p50_ms = adaptive_jitter_.jitter_p50_ms();
        udp_receive_stats_.jitter_p95_ms = adaptive_jitter_.jitter_p95_ms();
        udp_receive_stats_.jitter_buffer_active_mode = adaptive_jitter_.active_mode_name();
        udp_receive_stats_.jitter_buffer_quality = adaptive_jitter_.network_quality_name();
    }
    previous_network_latency_ms_ = network_latency_ms;
    has_previous_network_latency_ = true;
}

// 同步记录 jitter buffer 当前深度和历史最大深度。
void StreamClient::UpdateJitterBufferDepthStats() {
    udp_receive_stats_.jitter_buffer_current_depth = udp_jitter_buffer_.size();
    if (udp_receive_stats_.jitter_buffer_current_depth > udp_receive_stats_.jitter_buffer_max_depth) {
        udp_receive_stats_.jitter_buffer_max_depth = udp_receive_stats_.jitter_buffer_current_depth;
    }
}

// 记录某帧在 jitter buffer 中实际等待的时长（最近值、平均值和最大值）。
void StreamClient::RecordJitterBufferWait(std::uint64_t wait_ns) {
    const double wait_ms = static_cast<double>(wait_ns) / 1000000.0;
    udp_receive_stats_.jitter_buffer_wait_last_ms = wait_ms;
    ++udp_receive_stats_.jitter_buffer_wait_samples;
    udp_receive_stats_.jitter_buffer_wait_avg_ms +=
            (wait_ms - udp_receive_stats_.jitter_buffer_wait_avg_ms) /
            static_cast<double>(udp_receive_stats_.jitter_buffer_wait_samples);
    if (wait_ms > udp_receive_stats_.jitter_buffer_wait_max_ms) {
        udp_receive_stats_.jitter_buffer_wait_max_ms = wait_ms;
    }
}

}  // namespace sclient
