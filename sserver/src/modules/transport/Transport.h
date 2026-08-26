// Transport.h
// 传输模块门面（Facade）头文件。
// 根据配置选择具体传输后端（TCP / UDP / RTP），
// 并向上层提供统一的初始化、启动、广播与关闭接口。

#ifndef SSERVER_MODULES_TRANSPORT_TRANSPORT_H
#define SSERVER_MODULES_TRANSPORT_TRANSPORT_H

#include <memory>
#include <string>

#include "common/metrics/LatencyRecorder.h"
#include "common/model/EncodedFrame.h"
#include "config/AppConfig.h"
#include "core/ApplicationContext.h"
#include "core/ModuleState.h"

namespace sserver {
namespace modules {
namespace transport {

// 前置声明：具体传输后端在 .cpp 中定义。
class ITransportBackend;

// 可用的传输后端类型。
enum class TransportBackend {
    kAuto,  // 尚未解析出具体后端
    kTcp,   // TCP 流式传输
    kUdp,   // UDP 数据报传输（含 FEC/NACK 等增强）
    kRtp,   // RTP 实时传输（H.264 打包）
};

// 传输后端解析结果：包含枚举类型与名称（用于日志展示）。
struct TransportBackendSelection {
    TransportBackend backend = TransportBackend::kAuto;
    std::string backend_name = "auto";
};

// 根据传输配置中的 backend 字段解析实际使用的后端：
// "tcp" -> TcpStreamingBackend；"udp" -> UdpStreamingBackend；"rtp" -> RtpStreamingBackend。
inline bool ResolveTransportBackendSelection(
        const config::TransportConfig &config,
        TransportBackendSelection *selection,
        std::string *error_message) {
    if (selection == nullptr) {
        if (error_message != nullptr) {
            *error_message = "transport backend selection output is null";
        }
        return false;
    }

    // TCP：面向连接的流式传输。
    if (config.backend == "tcp") {
        selection->backend = TransportBackend::kTcp;
        selection->backend_name = "tcp";
        return true;
    }

    // UDP：无连接的数据报传输。
    if (config.backend == "udp") {
        selection->backend = TransportBackend::kUdp;
        selection->backend_name = "udp";
        return true;
    }

    // RTP：基于 UDP 的实时传输协议（H.264 RTP 打包）。
    if (config.backend == "rtp") {
        selection->backend = TransportBackend::kRtp;
        selection->backend_name = "rtp";
        return true;
    }

    // 其余值一律拒绝。
    if (error_message != nullptr) {
        *error_message = "transport.backend must be one of 'tcp', 'udp' or 'rtp'";
    }
    return false;
}

// TransportBackendFactory：按解析结果创建传输后端实例。
class TransportBackendFactory {
public:
    static std::unique_ptr<ITransportBackend> Create(
            const TransportBackendSelection &selection,
            const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder,
            std::string *error_message);
};

// Transport：传输模块对外的统一入口。
// 内部持有具体传输后端，负责后端生命周期管理与帧广播。
class Transport {
public:
    // send_latency_recorder 用于统计发送延迟指标。
    explicit Transport(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder);
    ~Transport();

    // 初始化：解析后端并创建/初始化后端实例。
    bool initialize(const core::ApplicationContext &context, std::string *error_message);

    // 启动传输：绑定端口并进入可发送状态。
    bool start(std::string *error_message);

    // 停止传输（后端可再次启动）。
    void stop();

    // 关闭并释放全部资源（不可再启动）。
    void shutdown();

    // 当前模块状态。
    core::ModuleState state() const;

    // 将一帧编码数据广播给所有接收端。
    void Broadcast(common::model::EncodedFramePtr frame);

    // 实际绑定的本地端口。
    int bound_port() const;

    // 当前实际使用的后端类型。
    TransportBackend backend() const;

    // 当前实际使用的后端名称。
    const std::string &backend_name() const;

private:
    // Pimpl：具体实现放在 .cpp 中。
    struct TransportImpl;
    std::unique_ptr<TransportImpl> impl_;
};

}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_TRANSPORT_H
